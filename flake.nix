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

      findPaths =
        term:
        let
          findPaths' =
            path: set:
            if (term set) then
              [ path ]
            else
              lib.concatLists (lib.mapAttrsToList (name: findPaths' (path ++ [ name ])) set);
        in
        findPaths' [ ];

      findPackages = findPaths lib.isDerivation;
      findApps = findPaths (set: (set.type or "") == "app");
    in
    {
      packages = forAllSystemsWithPkgs (
        system: pkgs:
        let
          proxyPkgs = {
            default = pkgs.callPackage "${self.outPath}/package.nix" {
              valgrindWorker = useValgrind;
            };

            libcrash =
              lib.flip lib.mapAttrs
                {
                  nop = { };
                  signal = prevAttrs: {
                    preBuild = ''
                      ${prevAttrs.preBuild or ""}
                      sed -e '/^all:/ s,,& bin/inject,' -i GNUmakefile
                    '';
                    postInstall = ''
                      ${prevAttrs.postInstall or ""}
                      cp bin/inject "$out/bin/inject"
                    '';
                  };
                }
                (
                  libcrashFlavor: overrideAttrs:
                  (self.packages.${system}.default.override { inherit libcrashFlavor; }).overrideAttrs overrideAttrs
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

            background=""
            pgid_file=""
            upstream=""
            while getopts 'D:g:H:' opt; do
              case "$opt" in
                D)
                  background="$OPTARG"
                  ;;
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
            export PATH="''${PATH-}''${PATH:+:}"${
              lib.makeBinPath [
                pkgs.glibc.getent
                pkgs.coreutils
              ]
            }

            # Resolve upstream host.
            [ -n "$upstream" ] || exit 2
            while ! resolved=$(getent hosts "$upstream"); do
              sleep .1
            done
            upstream="''${resolved%% *}"
            unset resolved

            if [ -n "$background" ]; then
              ("$background" &)
            fi

            # The write end is closed when all processes of the proxy termianted.
            mkfifo /tmp/stdin.fifo
            ${
              lib.escapeShellArgs (
                lib.optionals useStrace [
                  (lib.getExe pkgs.strace)
                  "-f"
                  "-e"
                  "trace=!openat"
                  "-e"
                  "status=failed"
                  "--"
                ]
                ++ [
                  pkgs.runtimeShell
                  "-c"
                  ''
                    set -e
                    # Save process group ID to $pgid_file.
                    [ -z "$1" ] || tee "$1" > /dev/null << eof
                    $$
                    eof
                    shift
                    exec ${newpgrp} "$@"
                  ''
                  "-"
                ]
              )
            } "$pgid_file" \
              "$@" -l 0.0.0.0:80 --upstream-address="$upstream:80" < /tmp/stdin.fifo &

            (
              (
                sleep 1
                exec ${lib.getExe' pkgs.psmisc "pstree"} --unicode --long --show-pids --show-pgids 1
              ) &
            )
            exec > /tmp/stdin.fifo
            rm -f /tmp/stdin.fifo >&2

            # `yes` will be killed by SIGPIPE if the write end is closed.
            exec yes ""
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

          randomCommand = [
            (lib.getExe (pkgs.python3.withPackages (ps: [ ps.numpy ])))
            "${self.outPath}/tools/rand.py"
            "-l"
            "0.0.0.0"
            "80"
          ];

          # Generate a compose file per benchmark and transformation.
          compose =
            lib.flip lib.mapAttrs
              {
                # Per transformation package override.
                transform_expensive = lib.flip lib.pipe [
                  (
                    package:
                    package.overrideAttrs (prevAttrs: {
                      buildInputs = (prevAttrs.buildInputs or [ ]) ++ [ pkgs.openssl ];
                    })
                  )
                  (
                    package:
                    package.override (prev: {
                      extraCppFlags = (prev.extraCppFlags or [ ]) ++ [ "-DEXPENSIVE_TRANSFORM" ];
                      extraLdFlags = (prev.extraLdFlags or [ ]) ++ [ "-lcrypto" ];
                    })
                  )
                ];
                transform_headers = lib.id;
                transform_nop =
                  package:
                  package.override (prev: {
                    extraCppFlags = (prev.extraCppFlags or [ ]) ++ [ "-DNOP_TRANSFORM" ];
                  });
              }
              (
                name: override:
                let
                  baselinePackage = override self.packages.${system}.performance-baseline;
                  proxyPackage = override self.packages.${system}.libcrash.signal;
                in
                lib.flip lib.mapAttrs (import "${self.outPath}/benchmarks.nix" { inherit lib pkgs self; }) (
                  _:
                  {
                    benchmarkScript,
                    upstreamHost ? "nginx.",
                    background ? null,
                  }:
                  pkgs.replaceVarsWith {
                    src = "${self.outPath}/compose.yaml";
                    replacements = lib.mapAttrs (_: v: builtins.replaceStrings [ "$" ] [ "$$" ] (builtins.toJSON v)) {
                      # Global constants.
                      inherit
                        baselinePort
                        nginxFiles
                        nginxPort
                        proxyPort
                        randomCommand
                        ;
                      # Vary by flavor and benchmark.
                      baselineCommand = [
                        startProxy
                        "-H${upstreamHost}"
                        "--"
                        (lib.getExe baselinePackage)
                      ];
                      proxyPath = "${proxyPackage}/bin";
                      proxyCommand = [
                        startProxy
                        "-H${upstreamHost}"
                        "-g/run/proxy.pgid"
                      ]
                      ++ (lib.optional (background != null) "-D${background}")
                      ++ [
                        "--"
                        "crash-tolerant-proxy"
                      ];
                      proxyHealthcheck = {
                        # Healthcheck does not seem to work in podman-compose.
                        # It says somewhere that systemd is used for scheduling...
                        test = [ "NONE" ];
                      };
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
                      haproxyDockerfile = ''
                        FROM docker.io/library/haproxy:3.3-alpine3.24
                        COPY <<EOF /usr/local/etc/haproxy/haproxy.cfg
                        global
                            nbthread 1
                        ${
                          if name == "transform_nop" then
                            ''
                              listen proxy
                                  mode tcp
                                  bind :80
                                  server nginx ${upstreamHost}:80
                            ''
                          else
                            ''
                              listen proxy
                                  mode http
                                  bind :80
                                  server nginx ${upstreamHost}:80
                                  http-request set-header Connection "close"
                                  http-request set-header User-Agent "HAProxy"
                                  http-request set-header DNT "1"
                                  http-request set-header Sec-GPC "1"
                                  http-response set-header Connection "close"
                                  http-response set-header Server "HAProxy"
                                  http-response set-header X-Clacks-Overhead "GNU Terry Pratchett"
                                  http-response set-header X-Proxy-PID "0"
                            ''
                        }EOF
                      '';
                    };
                  }
                )
              );
        in
        proxyPkgs
        // {
          devShell = pkgs.callPackage "${self.outPath}/shell.nix" { };

          inherit compose;

          compose_all = pkgs.runCommandLocal "compose" { } (
            lib.concatMapStrings (
              path:
              let
                dir = lib.concatStringsSep "/" path;
              in
              ''
                mkdir -p "$out"/${lib.escapeShellArg dir}
                ln -s ${
                  lib.attrByPath path "/dev/null" self.packages.${system}.compose
                } "$out"/${lib.escapeShellArg dir}/compose.yaml
              ''
            ) (findPackages self.packages.${system}.compose)
          );
        }
      );

      devShells = forAllSystemsWithPkgs (system: pkgs: self.packages.${system}.devShell);

      apps = forAllSystemsWithPkgs (
        system: pkgs: {
          # The default app just lists all packages and apps of the flake.
          default = {
            type = "app";
            program = toString (
              let
                packages = findPackages self.packages.${system};
                apps = findApps self.apps.${system};
                toNixCommand =
                  subCommand: path:
                  "  nix ${subCommand} -L ${lib.escapeShellArg ".?submodules=1#${lib.concatStringsSep "." path}"}";
              in
              pkgs.writeScript "help" ''
                #!${lib.getExe' pkgs.coreutils "tail"} +2
                Available packages:
                ${lib.concatMapStringsSep "\n" (toNixCommand "build") packages}

                Available apps:
                ${lib.concatMapStringsSep "\n" (toNixCommand "run") apps}
              ''
            );
          };

          ci = {
            doxygen = {
              type = "app";
              program = lib.getExe pkgs.doxygen;
            };
          };

          # Run podman-compose with the packaged compose files.
          compose =
            let
              toApp =
                set:
                if !lib.isDerivation set then
                  lib.mapAttrs (_: toApp) set
                else
                  let
                    compose_yaml = set;
                  in
                  {
                    type = "app";
                    program = toString (
                      pkgs.writeShellScript "run" ''
                        PS4='$ '
                        set -eux

                        export PATH=${
                          lib.makeBinPath [
                            pkgs.coreutils
                            pkgs.podman
                            pkgs.podman-compose
                          ]
                        }"''${PATH:+:}''${PATH:-}"

                        temp=$(mktemp -d)
                        (
                          mkdir -p "$temp/foo" benchmark
                          ln -rs benchmark "$temp/foo/"
                          cd "$temp/foo"
                          mkdir empty
                          ln -s ${compose_yaml} compose.yaml
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

                          podman-compose up --build --exit-code-from=benchmark || e=$?
                          podman-compose down --remove-orphans
                          exit ''${e:-0}
                        ) || e=$?
                        rm -fr "$temp"
                        exit ''${e:-0}
                      ''
                    );
                  };
            in
            toApp self.packages.${system}.compose;
        }
      );

      formatter = forAllSystemsWithPkgs (system: pkgs: pkgs.nixfmt);
    };
}
