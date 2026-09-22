{
  lib,
  stdenvNoCC,
  fetchurl,
  pandoc,
  qpdf,
  texliveFull,
  zopfli,
  date ? null,
}:

stdenvNoCC.mkDerivation {
  pname = "crash-tolerant-proxy-thesis";
  version = "0.0";

  src = ./.;

  nativeBuildInputs = [
    pandoc
    qpdf
    texliveFull
    zopfli
  ];

  outputs = [
    "out"
    "html"
  ];

  preBuild = ''
    ln -fs ${
      fetchurl {
        url = "https://pandoc.org/demo/ieee.csl";
        hash = "sha256:9b023e1b62d7459fefe7f989fbc6be1d54638952ab2581111b5895de0aa444e1";
      }
    } ieee.csl
    export HOME=$(mktemp -d)
  ''
  + lib.optionalString (date != null) ''
    { echo '---'; echo 'date:' ${lib.escapeShellArg (builtins.toJSON date)}; echo '...'; } > 02_date.yaml
  '';

  installPhase = ''
    runHook preInstall

    cp -r out/crash-tolerant-proxy "$html"
    mkdir "$out"
    cp out/crash-tolerant-proxy.pdf "$out/"

    runHook postInstall
  '';

  meta = {
    maintainers = with lib.maintainers; [ schnusch ];
  };
}
