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
    def last_offset_us(m):
        out = m.succeed(
            "journalctl -u synchrony-node -o cat "
            "| grep -o 'offset_us=-\\?[0-9]*' | tail -1"
        )
        return int(out.strip().split("=")[1])


    def wait_for_offset(m, lo_us, hi_us):
        def settled(_):
            try:
                return lo_us <= last_offset_us(m) <= hi_us
            except Exception:
                return False

        retry(settled)


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

    with subtest("a symmetric link syncs to within a millisecond"):
        for m in (node1, node2, node3):
            wait_for_offset(m, -5000, 5000)

    # Now make every link lopsided in the same direction: 10ms towards the
    # source, 50ms coming back. Cristian's algorithm splits the 60ms round trip
    # down the middle, so every hop believes it is 20ms off, and each one
    # inherits its upstream's error on top of its own.
    with subtest("asymmetric delay stacks one hop at a time"):
        source.succeed("tc qdisc add dev eth1 root netem delay 50ms")
        node1.succeed("tc qdisc add dev eth1 root netem delay 10ms")
        node1.succeed("tc qdisc add dev eth2 root netem delay 50ms")
        node2.succeed("tc qdisc add dev eth1 root netem delay 10ms")
        node2.succeed("tc qdisc add dev eth2 root netem delay 50ms")
        node3.succeed("tc qdisc add dev eth1 root netem delay 10ms")

        wait_for_offset(node1, -26000, -14000)
        wait_for_offset(node2, -52000, -28000)
        wait_for_offset(node3, -78000, -42000)

        measured = {m.name: last_offset_us(m) for m in (node1, node2, node3)}
        print("measured offsets (us): " + repr(measured))

        assert (
            abs(measured["node3"])
            > abs(measured["node2"])
            > abs(measured["node1"])
        ), f"error did not grow with hop count: {measured}"
  '';
}
