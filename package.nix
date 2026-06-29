{
  lib,
  stdenv,
  callPackage,
  doxygen,
  gtest,
  pkgconf,
  systemd,
}:

stdenv.mkDerivation {
  pname = "crash-tolerant-proxy";
  version = "0.0";

  src = lib.sourceFilesBySuffices ./. [
    "GNUmakefile"
    ".c"
    ".cc"
    ".h"
  ];

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
  ];

  makeFlags = [ "PREFIX=${builtins.placeholder "out"}" ];

  passthru.thesis = callPackage ./thesis/package.nix { };

  meta = {
    maintainers = with lib.maintainers; [ schnusch ];
  };
}
