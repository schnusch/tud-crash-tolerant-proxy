{
  inputs = {
    nixpkgs = {
      type = "indirect";
      id = "nixpkgs";
      ref = "624af665418d3c65d544145b4d34ad696439570e";
    };
  };

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      forAllSystems = lib.genAttrs (lib.filter (lib.hasSuffix "-linux") lib.systems.flakeExposed);
      forAllSystemsWithPkgs = f: forAllSystems (system: f system nixpkgs.legacyPackages.${system});

      useStrace = false;
      useValgrind = false;
    in
    {
      packages = forAllSystemsWithPkgs (
        system: pkgs:
        let
          proxyPkgs = {
            default = pkgs.callPackage ./package.nix {
              valgrindWorker = useValgrind;
            };

            libcrash = lib.genAttrs [ "nop" "signal" ] (
              libcrashFlavor: self.packages.${system}.default.override { inherit libcrashFlavor; }
            );

            performance-baseline = lib.pipe self.packages.${system}.default [
              (
                pkg:
                pkg.override (prev: {
                  libcrashFlavor = null;
                  extraCppFlags = (prev.extraCppFlags or [ ]) ++ [
                    "-DPERFORMANCE_BASELINE"
                  ];
                })
              )
              (
                pkg:
                pkg.overrideAttrs (prevAttrs: {
                  pname = "performance-baseline";
                  doCheck = false;

                  preBuild = ''
                    ${prevAttrs.preBuild or ""}
                    sed -e '/^all:/ s,,& bin/launcher,' -i GNUmakefile
                  '';

                  postInstall = ''
                    ${prevAttrs.postInstall or ""}
                    cp bin/launcher "$out/bin/crash-tolerant-proxy"
                  '';
                })
              )
            ];
          };

          nginxFiles = toString (
            pkgs.runCommandLocal "zero" { } ''
              mkdir -p "$out"
              for size in 0 1K 1M 4M 16M 64M 256M; do
                head -c"$size" /dev/zero | tr '\0' '\f' > "$out/$size"
              done
            ''
          );
        in
        proxyPkgs
        // {
          devShell = pkgs.callPackage ./shell.nix { };

          compose =
            let
              escapeDollarSigns = builtins.replaceStrings [ "$" ] [ "$$" ];
              commandPrefix = [
                # Resolve and append upstream address.
                pkgs.runtimeShell
                "-c"
                ''
                  PS4='$ '
                  set -ex
                  upstream=$(${pkgs.glibc.getent}/bin/getent hosts nginx.)
                  upstream="''${upstream%% *}"
                  exec "$@" -l 0.0.0.0:80 --upstream-address="''${upstream:?}:80"
                ''
                "--"
              ]
              ++ lib.optionals useStrace [
                (lib.getExe pkgs.strace)
                "-f"
                "-e"
                "trace=!openat"
                "-e"
                "status=failed"
                "--"
              ];

              proxyPort = 12345;
              baselinePort = 12346;
              nginxPort = 12347;
              proxyCommand = map escapeDollarSigns (
                commandPrefix ++ [ (lib.getExe self.packages.${system}.default) ]
              );
              baselineCommand = map escapeDollarSigns (
                commandPrefix ++ [ (lib.getExe self.packages.${system}.performance-baseline) ]
              );
            in
            lib.flip lib.mapAttrs (import ./benchmarks.nix { inherit pkgs; }) (
              _: benchmarkScript:
              pkgs.replaceVarsWith {
                src = ./compose.yaml;
                replacements = lib.mapAttrs (_: builtins.toJSON) {
                  inherit
                    baselineCommand
                    baselinePort
                    nginxFiles
                    nginxPort
                    proxyCommand
                    proxyPort
                    ;
                  benchmarkCommand = map escapeDollarSigns [
                    pkgs.runtimeShell
                    "-c"
                    (
                      ''
                        set -euo pipefail
                      ''
                      + benchmarkScript
                    )
                  ];
                };
              }
            );
        }
      );

      devShells = forAllSystemsWithPkgs (system: pkgs: self.packages.${system}.devShell);

      apps = forAllSystemsWithPkgs (
        system: pkgs: {
          default = self.apps.${system}.compose.ab;
          compose = lib.flip lib.mapAttrs self.packages.${system}.compose (
            _: compose: {
              type = "app";
              program = toString (
                pkgs.writeShellScript "run" ''
                  if [ $# -gt 1 ]; then
                    echo "Usage: $0 [target_host]" >&2
                    exit 2
                  fi

                  PS4='$ '
                  set -ex
                  temp=$(mktemp -d)
                  (
                    mkdir -p benchmark
                    ln -rs benchmark "$temp/"
                    cd "$temp"
                    mkdir empty
                    ln -s ${compose} compose.yaml
                    cat compose.yaml

                    tee proxy.env >&2 << eof
                  LOG_LEVEL=''${LOG_LEVEL:-2147483646}
                  eof

                    tee benchmark.env >&2 << eof
                  ${
                    let
                      defaults = {
                        BENCHMARK_TSV = "result.tsv";
                        BENCHMARK_HOST = "proxy.";
                        BENCHMARK_FILE = "1M";
                        BENCHMARK_REQUESTS = "100";
                        BENCHMARK_PARALLEL = "10";
                      };
                    in
                    lib.concatStringsSep "\n" (
                      lib.mapAttrsToList (k: v: "${k}=\${${k}-${lib.escapeShellArg v}}") defaults
                    )
                  }
                  eof

                    exec ${lib.getExe pkgs.podman-compose} --podman-path=${lib.getExe pkgs.podman} up --exit-code-from=benchmark
                  ) || e=$?
                  rm -fr "$temp"
                  exit ''${e:-0}
                ''
              );
            }
          );
          ci = {
            doxygen = {
              type = "app";
              program = lib.getExe pkgs.doxygen;
            };
          };
        }
      );
    };
}
