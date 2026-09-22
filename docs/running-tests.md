# Running the VM test

`nix build .#checks.<system>.topology` boots four NixOS guests under QEMU. That
needs a Linux builder with KVM, which a mac is not, so there are two ways to run
it and they are for different purposes.

## CI

`.github/workflows/check.yml` runs it on every push and pull request. Nothing to
set up, and it is the only route that keeps working when a laptop drifts. The one
non-obvious part is the udev rule: GitHub's runners ship `/dev/kvm` root-owned,
and without opening it up the guests fall back to software emulation and the job
runs long enough to matter.

Use this as the source of truth. Use the local builder below for iteration.

## Local builder, on Apple Silicon

Lima running Ubuntu with nested virtualization on, plus Nix inside it, registered
as a Nix remote builder. Nested virt needs an M3 or newer host: the NixOS test
driver boots its own QEMU guests inside the Lima VM, so `/dev/kvm` has to exist
in the guest, not just on the host.

```sh
limactl create --name=nixbuilder ops/lima-nixbuilder.yaml
limactl start nixbuilder
limactl shell nixbuilder test -e /dev/kvm && echo "nested virt works"
```

Then give the host's Nix daemon a way in. The daemon runs as root, so it is
root's ssh config that has to know the host, not yours:

```sh
ssh-keygen -t ed25519 -N "" -f ~/.ssh/nix_remote_builder_ed25519
limactl shell nixbuilder sudo tee -a /home/builder/.ssh/authorized_keys \
  < ~/.ssh/nix_remote_builder_ed25519.pub
```

```sh
sudo tee /etc/ssh/ssh_config.d/100-nixbuilder.conf <<'CONF'
Host nixbuilder
  HostName 127.0.0.1
  Port 31022
  User builder
  IdentityFile /Users/YOU/.ssh/nix_remote_builder_ed25519
  IdentitiesOnly yes
  StrictHostKeyChecking accept-new
  UserKnownHostsFile /etc/nix/known_hosts_nixbuilder
CONF

sudo tee /etc/nix/machines <<'CONF'
ssh-ng://builder@nixbuilder aarch64-linux /Users/YOU/.ssh/nix_remote_builder_ed25519 6 1 nixos-test,benchmark,big-parallel,kvm - -
CONF
```

Check it took:

```sh
sudo nix store info --store ssh-ng://builder@nixbuilder
nix build .#checks.aarch64-linux.topology -L
```

Without going through the host daemon at all, you can also just run the test
inside the VM against a pushed branch, which is useful for a quick check:

```sh
limactl shell nixbuilder \
  nix build --refresh \
    "github:JackFurton/synchrony/main#checks.aarch64-linux.topology" -L
```

`--refresh` is not optional. Nix caches what a `github:` ref resolves to for an
hour, so without it you re-test whatever revision you ran last time and the
failure you get back belongs to code you have already replaced. Check with
`nix flake metadata github:JackFurton/synchrony/main` if a result looks like it
is answering the wrong question.

## Determinate's Native Linux Builder

Determinate Nix ships one, and on a mac with a FlakeHub login it would replace
all of the above. Logged out it reports `The Native Linux Builder is not
currently available`, so it is an account feature rather than a fourth option.
Worth revisiting if this repo ever gets a FlakeHub account attached.
