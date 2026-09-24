{
  lib,
  pkgs,
  self,
}:
let
  timeToFirstByte = pkgs.writeCBin "time-to-first-byte" ''
    #include "${self.outPath}/tools/time-to-first-byte.c"
  '';

  injectPGid = pkgs.writeShellScript "inject" ''
    set -euo pipefail
    group="$1"
    crash="$2"
    ${lib.getExe' pkgs.procps "ps"} -A --no-heading -o pgid,pid \
    | (
      set --
      while read pgid pid; do
        [ "$pgid" -ne "$group" ] || set -- "$@" "$pid"
      done
      PS4='$ '
      set -x
      set +e
      for pid in "$@"; do
        inject "$pid" "$crash"
      done
      :
    )
  '';

  inject = crash: {
    upstreamHost = "benchmark.";
    benchmarkScript = ''
      PS4='$ '
      set -x

      ${lib.getExe' pkgs.binutils "objdump"} -d -M intel ${lib.getExe timeToFirstByte}

      while ! host=$(${lib.getExe' pkgs.glibc.getent "getent"} hosts "$BENCHMARK_HOST"); do
        ${lib.getExe' pkgs.coreutils "sleep"} .1
      done
      host="''${host%% *}"

      ${lib.getExe' pkgs.coreutils "sleep"} 1
      exec ${lib.getExe timeToFirstByte} "$host" "''${BENCHMARK_PARALLEL:?}" 3 > "/run/benchmark/''${BENCHMARK_CSV:?}"
    '';
    background = pkgs.writeShellScript "background" ''
      PS4='$ '
      set -eux
      ${lib.getExe' pkgs.coreutils "sleep"} 3

      : "Before error injection:"
      ${lib.getExe' pkgs.psmisc "pstree"} --unicode --long --show-pids --show-pgids 1

      ${lib.getExe pkgs.strace} -fe trace=rt_sigqueueinfo ${injectPGid} "$(< /run/proxy.pgid)" ${lib.escapeShellArgs crash}

      : "After error injection:"
      ${lib.getExe' pkgs.psmisc "pstree"} --unicode --long --show-pids --show-pgids 1
    '';
  };
in
{
  # https://stackoverflow.com/a/34785677
  ab.benchmarkScript = ''
    PS4='$ '
    set -x
    exec ${lib.getExe' pkgs.apacheHttpd "ab"} \
      -n"$BENCHMARK_REQUESTS" \
      -c"$BENCHMARK_PARALLEL" \
      -r \
      -g"/run/benchmark/$BENCHMARK_TSV" \
      "http://$BENCHMARK_HOST/$BENCHMARK_FILE"
  '';

  cassowary.benchmarkScript = ''
    PS4='$ '
    set -x
    ${lib.getExe pkgs.cassowary} run \
      --url="http://$BENCHMARK_HOST/$BENCHMARK_FILE" \
      --requests="$BENCHMARK_REQUESTS" \
      --concurrency="$BENCHMARK_PARALLEL" \
      --disable-keep-alive \
      --timeout=30 \
      --raw-output \
      --json-metrics \
      --json-metrics-file="/run/benchmark/$BENCHMARK_JSON"
    ${lib.getExe' pkgs.coreutils "mv"} raw.csv "/run/benchmark/$BENCHMARK_CSV"
  '';

  fail-accept = inject [ "ACCEPT_POST" ];
  fail-connect = inject [ "CONNECT_POST" ];

  first-byte = {
    upstreamHost = "benchmark.";
    benchmarkScript = ''
      PS4='$ '
      set -x

      while ! host=$(${lib.getExe' pkgs.glibc.getent "getent"} hosts "$BENCHMARK_HOST"); do
        ${lib.getExe' pkgs.coreutils "sleep"} .1
      done
      host="''${host%% *}"

      while ! ${lib.getExe pkgs.strace} ${lib.getExe timeToFirstByte} "$host"; do
        ${lib.getExe' pkgs.coreutils "sleep"} .1
      done

      { echo "time,select"
        for i in $(${lib.getExe' pkgs.coreutils "seq"} "$BENCHMARK_REQUESTS"); do
          ${lib.getExe timeToFirstByte} "$host"
        done
      } | ${lib.getExe' pkgs.coreutils "tee"} "/run/benchmark/$BENCHMARK_CSV"
    '';
  };

  random = {
    upstreamHost = "random.";
    benchmarkScript = ''
      PS4='$ '
      set -x
      exec ${lib.getExe (pkgs.python3.withPackages (ps: [ ps.numpy ]))} \
        ${self.outPath}/tools/rand.py \
        -n"$BENCHMARK_REQUESTS" \
        -c"$BENCHMARK_PARALLEL" \
        "$BENCHMARK_HOST" 80
    '';
    background = pkgs.writeShellScript "background" ''
      PS4='$ '
      set -eux
      ${lib.getExe' pkgs.coreutils "sleep"} 3
      ${lib.getExe' pkgs.procps "ps"} -A
      exec ${injectPGid} "$(< /run/proxy.pgid)"
    '';
  };

  vegeta.benchmarkScript =
    let
      vegeta' = pkgs.vegeta.overrideAttrs (prevAttrs: {
        patches = (prevAttrs.patches or [ ]) ++ [ ./vegeta-discard-body.patch ];
      });
    in
    ''
      export PATH=${
        lib.makeBinPath [
          vegeta'
          pkgs.xan
          pkgs.gzip
          pkgs.netcat
          pkgs.glibc.getent
          pkgs.coreutils
        ]
      }
      PS4='$ '
      set -x

      while ! host=$(getent hosts "$BENCHMARK_HOST"); do
        sleep .1
      done
      host="''${host%% *}"

      while ! nc -z "$host" 80; do
        sleep .1
      done

      vegeta attack \
          -connections="$BENCHMARK_PARALLEL" \
          -max-connections="$BENCHMARK_PARALLEL" \
          -workers="$(($BENCHMARK_PARALLEL * 2))" \
          -max-workers="$(($BENCHMARK_PARALLEL * 2))" \
          -rate=0 \
          -duration="''${BENCHMARK_DURATION}s" << eof \
        | gzip --fast \
        > vegeta.gz
      GET http://$host/$BENCHMARK_FILE
      eof
      ls -dhlp vegeta.gz
      { echo "timestamp_ns,status_code,latency_ns,bytes_out,bytes_in,error,response_body,attack_name,sequence_number,method,url,response_headers"
        gzip -d < vegeta.gz | vegeta encode -to=csv
      } | xan drop response_body,response_headers \
        | xan sort --parallel --external --numeric --select=latency_ns \
        | gzip \
        > "/run/benchmark/$BENCHMARK_CSV.gz"
    '';

  wrk.benchmarkScript = ''
    PS4='$ '
    set -x
    exec ${lib.getExe pkgs.wrk} \
      --connections="$BENCHMARK_PARALLEL" \
      --duration=2 \
      --latency \
      "http://$BENCHMARK_HOST:80/$BENCHMARK_FILE"
  '';
}
