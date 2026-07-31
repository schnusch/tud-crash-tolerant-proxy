{ callPackage }:
callPackage ./package.nix {
  extraCppFlags = [ "-DPERFORMANCE_BASELINE" ];
}
