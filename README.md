# synchrony

A four-node store-and-forward relay chain, built to learn C++ and distributed
time/state sync the hard way.

```
source <--vlan1-- node1 <--vlan2-- node2 <--vlan3-- node3
```

node3 has no path to source. It can only be heard by relaying through node2 and
node1, and each node is in turn the upstream for the next one. This is the shape
real mesh backhaul takes when nodes are spread across terrain with no direct
link home, and it is the shape that makes clock sync and data sync interesting:
error accumulates per hop, and any node dying partitions everything behind it.

## Build

```sh
nix build .#synchrony-node     # or: cmake -S . -B build && cmake --build build
nix develop                    # cmake, clang-tools
```

## Run the chain locally

Four processes on loopback, no VMs:

```sh
B=./result/bin/synchrony-node
$B --id source --listen 9000 &
$B --id node1  --listen 9001 --upstream 127.0.0.1:9000 &
$B --id node2  --listen 9002 --upstream 127.0.0.1:9001 &
$B --id node3  --listen 9003 --upstream 127.0.0.1:9002 &
```

source logs `delivered at source: hello path=node3>node2>node1`, which is the
whole chain in one line.

Loopback proves the code path but not the topology: nothing stops node3 dialling
source directly. That is what the VM test is for.

## Run the VM test

```sh
nix flake check          # Linux only, see docs/running-tests.md
```

Four QEMU VMs on three isolated virtual LANs. node3 has no interface on vlan1,
so the chain is enforced by the network rather than by anyone's good behaviour.

## Where it is going

See the issues. Roughly: wire protocol, then Cristian's algorithm hop-by-hop to
watch clock error stack across three hops, then hybrid logical clocks, then
Merkle anti-entropy pulling only from the upstream neighbour, then chained Nix
substituters so a real derivation crosses all three hops, then kill node2 and
make the queue survive it.
