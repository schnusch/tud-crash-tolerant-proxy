{
  lib,
  stdenv,
  callPackage,
  doxygen,
  gtest,
  makeWrapper,
  pkgconf,
  valgrind,
  libbacktrace ? null,
  systemd ? null,
  valgrindWorker ? false,
  extraCppFlags ? [ ],
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
  preBuild =
    let
      params = {
        EXTRA_CPPFLAGS = {
          "-DUSE_LIBBACKTRACE" = libbacktrace != null;
          "-DUSE_SYSTEMD" = systemd != null;
        }
        // lib.genAttrs extraCppFlags (_: true);
        EXTRA_LDFLAGS = {
          "-lbacktrace" = libbacktrace != null;
          "-lsystemd" = systemd != null;
        };
      };
      activeFlags =
        flags: lib.concatLists (lib.mapAttrsToList (flag: enable: lib.optional enable flag) flags);
      mkMakeFlag = var: flags: "${var}=${lib.concatStringsSep " " (activeFlags flags)}";
    in
    lib.concatStrings (
      lib.mapAttrsToList (var: flags: ''
        makeFlagsArray+=(${lib.escapeShellArg (mkMakeFlag var flags)})
      '') params
    );

  postFixup = lib.optionalString valgrindWorker ''
    mv "$out/libexec/crash-tolerant-proxy-worker" "$out/libexec/.crash-tolerant-proxy-worker"
    makeWrapper ${lib.getExe valgrind} "$out/libexec/crash-tolerant-proxy-worker" \
      --add-flags -- \
      --add-flags "$out/libexec/.crash-tolerant-proxy-worker"
    cat "$out/libexec/crash-tolerant-proxy-worker"
  '';

  dontStrip = (libbacktrace != null);
  doCheck = true;

  nativeBuildInputs = [
    doxygen
    makeWrapper
  ];

  buildInputs = [
    libbacktrace
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
    mainProgram = "crash-tolerant-proxy";
    maintainers = with lib.maintainers; [ schnusch ];
  };
}
