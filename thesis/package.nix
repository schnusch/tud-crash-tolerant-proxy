{
  lib,
  stdenvNoCC,
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
    export HOME=$(mktemp -d)
  ''
  + lib.optionalString (date != null) ''
    { echo '---'; echo 'date:' ${lib.escapeShellArg (builtins.toJSON date)}; echo '...'; } > 02_date.yaml
  '';

  installPhase = ''
    runHook preInstall

    cp -r out/crash-tolerant-proxy "$html"
    mkdir "$out"
    cp -r out/crash-tolerant-proxy.pdf "$out/"

    runHook postInstall
  '';

  meta = {
    maintainers = with lib.maintainers; [ schnusch ];
  };
}
