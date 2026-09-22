{ pkgs, synchrony }:

# The chain is enforced by the virtual network, not by firewall rules: node3
# has no interface on vlan1 at all, so it cannot reach source even in principle.
#
#   source --vlan1-- node1 --vlan2-- node2 --vlan3-- node3
let
  port = 9000;

  node = { upstream ? null, vlans }: { config, ... }: {
    virtualisation.vlans = vlans;
    networking.firewall.allowedTCPPorts = [ port ];
    environment.systemPackages = [ pkgs.iproute2 ];
    boot.kernelModules = [ "sch_netem" ];

    systemd.services.synchrony-node = {
      wantedBy = [ "multi-user.target" ];
      after = [ "network-online.target" ];
      wants = [ "network-online.target" ];
      serviceConfig = {
        ExecStart = pkgs.lib.concatStringsSep " " ([
          (pkgs.lib.getExe synchrony)
          "--id ${config.networking.hostName}"
          "--listen ${toString port}"
        ] ++ pkgs.lib.optional (upstream != null)
          "--upstream ${upstream}:${toString port}");
        Restart = "always";
        RestartSec = 1;
      };
    };
  };
in
pkgs.testers.runNixOSTest {
  name = "synchrony-topology";

  nodes = {
    source = node { vlans = [ 1 ]; };
    node1 = node { vlans = [ 1 2 ]; upstream = "source"; };
    node2 = node { vlans = [ 2 3 ]; upstream = "node1"; };
    node3 = node { vlans = [ 3 ]; upstream = "node2"; };
  };

  testScript = ''
    import time

    # Each guest keeps its own CLOCK_REALTIME and nothing syncs them, so the
    # nodes start hundreds of milliseconds apart. That constant is invisible to
    # Cristian's algorithm and to us, so every assertion below is about how much
    # an estimate MOVES when the network changes, never its absolute value.
    def last_offset_us(m):
        out = m.succeed(
            "journalctl -u synchrony-node -o cat "
            "| grep -o 'offset_us=-\\?[0-9]*' | tail -1"
        )
        return int(out.strip().split("=")[1])


    def settled_offset_us(m, tol_us=3000, tries=40):
        prev = None
        for _ in range(tries):
            time.sleep(1)
            v = last_offset_us(m)
            if prev is not None and abs(v - prev) <= tol_us:
                return v
            prev = v
        raise Exception(f"{m.name} offset never settled, last was {prev}")


    def netem(m, dev, ms, first=False):
        verb = "add" if first else "change"
        m.succeed(f"tc qdisc {verb} dev {dev} root netem delay {ms}ms")


    start_all()

    for m in (source, node1, node2, node3):
        m.wait_for_unit("synchrony-node.service")

    # The point of the whole exercise: node3 can only be heard through the chain.
    node3.fail("ping -c1 -W2 source")
    node3.fail("ping -c1 -W2 node1")
    node3.succeed("ping -c1 -W2 node2")

    # origin is the node that spoke, hops is how many relayed it on, so this
    # asserts both that node3 was heard and that it took the long way round.
    source.wait_until_succeeds(
        "journalctl -u synchrony-node | grep -q 'origin=node3 hops=2'"
    )

    relays = (node1, node2, node3)

    # One row per direction of travel, with the delay each gets in the
    # asymmetric phase: 10ms heading towards the source, 50ms coming back.
    links = (
        (source, "eth1", 50),  # source -> node1
        (node1, "eth1", 10),   # node1  -> source
        (node1, "eth2", 50),   # node1  -> node2
        (node2, "eth1", 10),   # node2  -> node1
        (node2, "eth2", 50),   # node2  -> node3
        (node3, "eth1", 10),   # node3  -> node2
    )

    with subtest("estimates converge on an idle network"):
        base = {m.name: settled_offset_us(m) for m in relays}
        print("baseline offsets (us): " + repr(base))

    with subtest("symmetric delay does not move the estimate"):
        for m, dev, _ in links:
            netem(m, dev, 30, first=True)

        for m in relays:
            moved = settled_offset_us(m) - base[m.name]
            assert abs(moved) < 6000, (
                f"{m.name} moved {moved}us under symmetric delay, "
                "which should cancel"
            )

    # 10ms towards the source, 50ms coming back. Cristian's splits the 60ms
    # round trip down the middle, so each hop is wrong by (10 - 50) / 2 and
    # inherits its upstream's error on top of its own.
    with subtest("asymmetric delay stacks one hop at a time"):
        for m, dev, delay_ms in links:
            netem(m, dev, delay_ms)

        moved = {m.name: settled_offset_us(m) - base[m.name] for m in relays}
        print("offset change under asymmetry (us): " + repr(moved))

        for name, expected in (("node1", -20000), ("node2", -40000),
                               ("node3", -60000)):
            assert abs(moved[name] - expected) < 9000, (
                f"{name} moved {moved[name]}us, expected about {expected}us"
            )

        assert (
            abs(moved["node3"]) > abs(moved["node2"]) > abs(moved["node1"])
        ), f"error did not grow with hop count: {moved}"
  '';
}
