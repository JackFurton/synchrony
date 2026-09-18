{ lib, stdenv, cmake }:

stdenv.mkDerivation {
  pname = "synchrony-node";
  version = "0.1.0";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [ cmake ];

  doCheck = true;

  meta = {
    description = "Store-and-forward relay node for a linear multi-hop chain";
    mainProgram = "synchrony-node";
  };
}
