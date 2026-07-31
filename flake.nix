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

            performance-baseline = lib.pipe self.packages.${system}.default [
              (
                pkg:
                pkg.override (prev: {
                  extraCppFlags = (prev.extraCppFlags or [ ]) ++ [
                    "-DPERFORMANCE_BASELINE"
                  ];
                })
              )
              (
                pkg:
                pkg.overrideAttrs (_: {
                  pname = "performance-baseline";
                  doCheck = false;
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
          compose =
            let
              escapeDollarSigns = builtins.replaceStrings [ "$" ] [ "$$" ];
              commonCommand = [
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
                commonCommand ++ [ (lib.getExe self.packages.${system}.default) ]
              );
              baselineCommand = map escapeDollarSigns (
                commonCommand ++ [ (lib.getExe self.packages.${system}.performance-baseline) ]
              );
            in
            lib.flip lib.mapAttrs (import ./benchmarks.nix { inherit pkgs; }) (
              _: benchmarkCommand:
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
                  benchmarkCommand = map escapeDollarSigns benchmarkCommand;
                };
              }
            );
        }
      );

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

                    tee proxy.env << eof
                  LOG_LEVEL=''${LOG_LEVEL:-2147483646}
                  eof

                    tee benchmark.env << eof
                  BENCHMARK_TSV=result.tsv
                  BENCHMARK_HOST=''${1-proxy.}
                  BENCHMARK_FILE=1M
                  BENCHMARK_REQUESTS=100
                  BENCHMARK_PARALLEL=10
                  eof

                    exec ${lib.getExe pkgs.podman-compose} --podman-path=${lib.getExe pkgs.podman} up --exit-code-from=benchmark
                  ) || e=$?
                  rm -fr "$temp"
                  exit ''${e:-0}
                ''
              );
            }
          );
        }
      );
    };
}
