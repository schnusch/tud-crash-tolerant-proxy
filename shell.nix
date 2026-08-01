{
  pkgs ? import <nixpkgs> { },
  callPackage ? pkgs.callPackage,
}:
let
  package = callPackage ./package.nix { };

  extraPackages = with pkgs; [
    clang-tools
    iperf3
    sockperf
  ];
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
        nativeBuildInputs = nativeBuildInputs ++ nativeCheckInputs ++ extraPackages;
      };
  };
})
