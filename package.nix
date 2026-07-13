{
  lib,
  stdenv,
  callPackage,
  doxygen,
  gtest,
  pkgconf,
  systemd,
  valgrind,
}:

stdenv.mkDerivation {
  pname = "crash-tolerant-proxy";
  version = "0.0";

  src = lib.sourceFilesBySuffices ./. [
    "GNUmakefile"
    "Makefile"
    ".c"
    ".cc"
    ".h"
  ];

  makeFlags = [ "PREFIX=${builtins.placeholder "out"}" ];
  doCheck = true;

  nativeBuildInputs = [
    doxygen
  ];

  buildInputs = [
    systemd
  ];

  nativeCheckInputs = [
    pkgconf
  ];

  checkInputs = [
    gtest
    valgrind
  ];

  passthru.thesis = callPackage ./thesis/package.nix { };

  meta = {
    maintainers = with lib.maintainers; [ schnusch ];
  };
}
