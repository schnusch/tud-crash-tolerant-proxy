{
  pkgs ? import <nixpkgs> { },
  callPackage ? pkgs.callPackage,
  stdenv ? pkgs.stdenv,
}:
callPackage ./package.nix {
  stdenv = stdenv // {
    mkDerivation =
      { buildInputs, checkInputs, ... }:
      pkgs.mkShell {
        buildInputs = buildInputs ++ checkInputs;
      };
  };
}
