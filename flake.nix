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
                    ''-DUSER_AGENT="\"performance-baseline\""''
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

          newpgrp = lib.getExe (
            pkgs.writeCBin "newpgrp" ''
              #include <errno.h>
              #include <stdio.h>
              #include <unistd.h>
              int main(int argc, char **argv) {
                  if(argc <= 1) {
                      errno = EINVAL;
                  } else if(setpgid(0, 0) == 0) {
                      fprintf(stderr, ">>> pgid: %d\n", (int)getpgid(0));
                      execvp(argv[1], argv + 1);
                  }
                  perror(argv[0]);
                  return 1;
              }
            ''
          );

          startProxy = pkgs.writeShellScript "start-proxy.sh" ''
            set -eu

            pgid_file=""
            upstream=""
            while getopts 'g:H:' opt; do
              case "$opt" in
                g)
                  pgid_file="$OPTARG"
                  ;;
                H)
                  upstream="$OPTARG"
                  ;;
                *)
                  exit 2
                  ;;
              esac
            done
            shift $((OPTIND - 1))

            PS4='$ '
            set -x

            # Resolve upstream host.
            [ -n "$upstream" ] || exit 2
            while ! resolved=$(${pkgs.glibc.getent}/bin/getent hosts "$upstream"); do
              ${lib.getExe' pkgs.coreutils "sleep"} .1
            done
            upstream="''${resolved%% *}"
            unset resolved

            # Save process group ID to $pgid_file.
            [ -z "$pgid_file" ] || ${lib.getExe' pkgs.coreutils "tee"} "$pgid_file" > /dev/null << eof
            -$$
            eof

            exec ${
              lib.escapeShellArgs (
                [ newpgrp ]
                ++ lib.optionals useStrace [
                  (lib.getExe pkgs.strace)
                  "-f"
                  "-e"
                  "trace=!openat"
                  "-e"
                  "status=failed"
                  "--"
                ]
              )
            } "$@" -l 0.0.0.0:80 --upstream-address="$upstream:80"
          '';

          proxyPort = 12345;
          baselinePort = 12346;
          nginxPort = 12347;

          nginxFiles = toString (
            pkgs.runCommandLocal "zero" { } ''
              mkdir -p "$out"
              for size in 0 1K 1M 4M 16M 64M 256M; do
                head -c"$size" /dev/zero | tr '\0' '\f' > "$out/$size"
              done
            ''
          );

          compose =
            lib.flip lib.mapAttrs (import ./benchmarks.nix { inherit lib pkgs; }) (
              _:
              {
                benchmarkScript,
                upstreamHost ? "nginx.",
              }:
              pkgs.replaceVarsWith {
                src = ./compose.yaml;
                replacements = lib.mapAttrs (_: v: builtins.replaceStrings [ "$" ] [ "$$" ] (builtins.toJSON v)) {
                  # Global constants.
                  inherit
                    baselinePort
                    nginxFiles
                    nginxPort
                    proxyPort
                    ;
                  baselineCommand = [
                    startProxy
                    "-H${upstreamHost}"
                    "--"
                    (lib.getExe self.packages.${system}.performance-baseline)
                  ];
                  proxyCommand = [
                    startProxy
                    "-H${upstreamHost}"
                    "--"
                    (lib.getExe self.packages.${system}.libcrash.signal)
                  ];
                  benchmarkCommand = [
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
        in
        proxyPkgs
        // {
          devShell = pkgs.callPackage ./shell.nix { };

          inherit compose;
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
                  LOG_LEVEL=''${LOG_LEVEL:-2147483647}
                  eof

                    tee benchmark.env >&2 << eof
                  ${
                    let
                      defaults = {
                        BENCHMARK_CSV = "result.csv";
                        BENCHMARK_TSV = "result.tsv";
                        BENCHMARK_JSON = "result.json";
                        BENCHMARK_HOST = "proxy.";
                        BENCHMARK_FILE = "1M";
                        BENCHMARK_REQUESTS = "100";
                        BENCHMARK_PARALLEL = "10";
                        BENCHMARK_DURATION = "60";
                      };
                    in
                    lib.concatStringsSep "\n" (
                      lib.mapAttrsToList (k: v: "${k}=\${${k}-${lib.escapeShellArg v}}") defaults
                    )
                  }
                  eof

                    ${lib.getExe pkgs.podman-compose} --podman-path=${lib.getExe pkgs.podman} up --build --exit-code-from=benchmark || e=$?
                    ${lib.getExe pkgs.podman-compose} --podman-path=${lib.getExe pkgs.podman} down --remove-orphans
                    exit ''${e:-0}
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

      formatter = forAllSystemsWithPkgs (system: pkgs: pkgs.nixfmt);
    };
}
