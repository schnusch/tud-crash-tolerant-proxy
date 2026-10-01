{
  lib,
  stdenvNoCC,
  fetchurl,
  pandoc,
  pv,
  python3,
  qpdf,
  texliveFull,
  xan,
  zopfli,
  date ? null,
}:

stdenvNoCC.mkDerivation {
  pname = "crash-tolerant-proxy-thesis";
  version = "0.0";

  src = ./.;

  nativeBuildInputs = [
    pandoc
    pv
    python3
    qpdf
    texliveFull
    xan
    zopfli
  ];

  outputs = [
    "out"
    "html"
  ];

  preBuild =
    lib.optionalString (date != null) ''
      { echo '---'; echo 'date:' ${lib.escapeShellArg (builtins.toJSON date)}; echo '...'; } > 02_date.yaml
    ''
    + ''
      mkdir -p ../benchmark/vegeta
    ''
    + lib.concatStrings (
      lib.mapAttrsToList
        (name: hash: ''
          tar -C ../benchmark/vegeta -xzf ${
            fetchurl {
              url = "https://github.com/schnusch/tud-crash-tolerant-proxy/releases/download/benchmark/${name}";
              inherit hash;
            }
          }
        '')
        {
          "transform_expensive.tar.gz" =
            "sha256:4a2d93ba698169dba0053e7d6758992ebbeebc5116f7aeec58ac4276ddaba0de";
          "transform_headers.tar.gz" =
            "sha256:0803ad31f35a0d3f0524035da6092bf8aa6bbba85658a6783cb6e0876e6ccc69";
          "transform_nop.tar.gz" = "sha256:81450bd55a71a57937a8704c2b4de03745b9c96d9ab3001e038e662e1f49b8d3";
        }
    )
    + ''
      ln -fs ${
        fetchurl {
          url = "https://pandoc.org/demo/ieee.csl";
          hash = "sha256:9b023e1b62d7459fefe7f989fbc6be1d54638952ab2581111b5895de0aa444e1";
        }
      } ieee.csl
      patchShebangs --build tools/
      export HOME=$(mktemp -d)
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
