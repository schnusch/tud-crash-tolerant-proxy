{ callPackage }:
(callPackage ./package.nix { }).overrideAttrs (
  {
    makeFlags ? [ ],
    postBuild ? "",
    postInstall ? "",
    ...
  }:
  {
    makeFlags = makeFlags ++ [ "EXTRA_CPPFLAGS=-DPERFORMANCE_BASELINE" ];
    doCheck = false;

    preBuild = ''
      sed -e '/^all:/ s,,& bin/launcher,' -i GNUmakefile
    '';

    postInstall = ''
      ${postInstall}
      cp bin/launcher "$out/bin/crash-tolerant-proxy"
    '';
  }
)
