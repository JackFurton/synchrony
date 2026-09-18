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
  '';
}
