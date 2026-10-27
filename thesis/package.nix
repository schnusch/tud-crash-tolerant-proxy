{
  lib,
  stdenvNoCC,
  fetchurl,
  lndir,
  pandoc,
  pv,
  python3,
  qpdf,
  symlinkJoin,
  texliveFull,
  unzip,
  xan,
  zopfli,
  date ? null,
  usePrereqs ? true,
}:

let

  linkTarballs = lib.concatStrings (
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
  );

in

stdenvNoCC.mkDerivation (finalAttrs: {
  pname = "crash-tolerant-proxy-thesis";
  version = "0.0";

  src = ./.;

  nativeBuildInputs = [
    pandoc
    qpdf
    texliveFull
    unzip
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
    + (
      if usePrereqs then
        ''
          ${lib.getExe lndir} -silent ${finalAttrs.passthru.vegetaPrereqs}
        ''
      else
        linkTarballs
    )
    + ''
      ln -fs ${
        fetchurl {
          url = "https://pandoc.org/demo/ieee.csl";
          hash = "sha256:9b023e1b62d7459fefe7f989fbc6be1d54638952ab2581111b5895de0aa444e1";
        }
      } ieee.csl
      ln -fs ${
        fetchurl {
          url = "https://github.com/tud-cd/tudcd-scr/releases/download/0.7.1/tudcdscr-sharelatex-beamer-0.7.1.zip";
          hash = "sha256:db1624a75c37d5481029d90d587cfdf025ed645df0562113e6c377bbf74ddd9d";
        }
      } presentation/tudcdscr-sharelatex-beamer-0.7.1.zip
      patchShebangs --build tools/
      export HOME=$(mktemp -d)
    '';

  installPhase = ''
    runHook preInstall

    cp -r out/crash-tolerant-proxy "$html"
    mkdir "$out"
    cp out/crash-tolerant-proxy.pdf "$out/"
    cp presentation/main.pdf "$out/presentation.pdf"

    runHook postInstall
  '';

  passthru = {
    # Build complex CSVs separately.
    vegetaPrereqs =
      let
        buildVegeta =
          target:
          stdenvNoCC.mkDerivation {
            name = baseNameOf target;

            src = lib.sourceByRegex finalAttrs.src [
              "GNUmakefile"
              "tikz"
              "tikz/include"
              "tikz/include/vegeta\\.tex"
              "tools"
              "tools/.*"
            ];

            nativeBuildInputs = [
              pv
              python3
              xan
            ];

            makeFlags = [ target ];

            preBuild = ''
              mkdir -p ../benchmark/vegeta
            ''
            + linkTarballs
            + ''
              patchShebangs --build tools/
              export HOME=$(mktemp -d)
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p "$out/data"
              cp -rv data/vegeta "$out/data"
              runHook postInstall
            '';

            inherit (finalAttrs) meta;
          };

        vegetaSources = builtins.filter (x: builtins.match "vegeta-.*\\.tex" x != null) (
          builtins.attrNames (builtins.readDir "${finalAttrs.src}/tikz")
        );
        vegetaTargets = builtins.map (x: "tmp/html/tikz/" + x) vegetaSources;
      in
      symlinkJoin (finalAttrs: {
        name = "vegeta";
        paths = builtins.attrValues finalAttrs.passthru;
        passthru = lib.genAttrs vegetaTargets buildVegeta;
      });
  };

  meta = {
    maintainers = with lib.maintainers; [ schnusch ];
  };
})
