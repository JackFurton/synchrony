# Running the VM test

`nix flake check` builds a NixOS VM test, which needs KVM and a Linux builder.
On an Apple Silicon mac there is no local Linux builder by default:

```
$ nix store info --store ssh-ng://builder@linux-builder
ssh: Could not resolve hostname linux-builder
```

Three ways out, cheapest first.

**nix-darwin's linux-builder.** If you run nix-darwin, set
`nix.linux-builder.enable = true`. It provisions an aarch64-linux VM and wires
it into `nix.buildMachines`, after which `nix flake check` works unchanged. This
is the least friction, and the reason to consider adopting nix-darwin even
though this repo does not otherwise need it.

**A Linux VM you manage.** Lima or UTM running NixOS, added to
`nix.buildMachines` by hand. More moving parts, but the VM is reusable for other
things.

**CI.** A GitHub Actions job on `ubuntu-latest` running `nix flake check`. Slow
feedback, but it is the only option that also protects the repo from a laptop
that drifts.

Until one of these is in place the test expression still evaluates, which catches
typos but proves nothing about behaviour:

```sh
nix eval .#checks.aarch64-linux.topology.drvPath
```
