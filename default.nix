{
  pkgs ? import <nixpkgs> { },
  callPackage ? pkgs.callPackage,
}:
callPackage ./package.nix { }
