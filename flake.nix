{
  description = "Store-and-forward relay chain: source <- node1 <- node2 <- node3";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      linuxSystems = [ "aarch64-linux" "x86_64-linux" ];
      forAll = nixpkgs.lib.genAttrs;
    in
    {
      packages = forAll systems (system: {
        default = self.packages.${system}.synchrony-node;
        synchrony-node =
          nixpkgs.legacyPackages.${system}.callPackage ./nix/package.nix { };
      });

      devShells = forAll systems (system: {
        default = nixpkgs.legacyPackages.${system}.mkShell {
          packages = with nixpkgs.legacyPackages.${system}; [
            cmake
            clang-tools
          ];
        };
      });

      # VM tests need a Linux builder; on darwin see docs/running-tests.md.
      checks = forAll linuxSystems (system: {
        topology = import ./nix/topology.nix {
          pkgs = nixpkgs.legacyPackages.${system};
          synchrony = self.packages.${system}.synchrony-node;
        };
      });
    };
}
