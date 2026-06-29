{
  pkgs ? import <nixpkgs> { },
  callPackage ? pkgs.callPackage,
}:
let
  package = callPackage ./package.nix { };
in
package.override (prev: {
  stdenv = prev.stdenv // {
    mkDerivation =
      {
        buildInputs ? [ ],
        checkInputs ? [ ],
        nativeBuildInputs ? [ ],
        nativeCheckInputs ? [ ],
        ...
      }:
      pkgs.mkShell {
        buildInputs = buildInputs ++ checkInputs;
        nativeBuildInputs = nativeBuildInputs ++ nativeCheckInputs ++ package.thesis.nativeBuildInputs;
      };
  };
})
