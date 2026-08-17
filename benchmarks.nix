{ lib, pkgs }:
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

  first-byte = {
    upstreamHost = "benchmark.";
    benchmarkScript =
      let
        timeToFirstByte = pkgs.writeCBin "time-to-first-byte" ''
          #include "${./tools/time-to-first-byte.c}"
        '';
      in
      ''
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

  vegeta.benchmarkScript = ''
    PS4='$ '
    set -x
    ${lib.getExe pkgs.vegeta} attack \
        -connections="$BENCHMARK_PARALLEL" \
        -max-connections="$BENCHMARK_PARALLEL" \
        -workers="$(($BENCHMARK_PARALLEL * 2))" \
        -max-workers="$(($BENCHMARK_PARALLEL * 2))" \
        -rate=0 \
        -duration="''${BENCHMARK_DURATION}s" << eof \
      | ${lib.getExe pkgs.gzip} --fast \
      > vegeta.gz
    GET http://$BENCHMARK_HOST/$BENCHMARK_FILE
    eof
    ${lib.getExe' pkgs.coreutils "ls"} -dhlp vegeta.gz
    { echo "timestamp_ns,status_code,latency_ns,bytes_out,bytes_in,error,response_body,attack_name,sequence_number,method,url,response_headers"
      ${lib.getExe pkgs.gzip} -d < vegeta.gz | ${lib.getExe pkgs.vegeta} encode -to=csv
    } | ${lib.getExe pkgs.xan} drop response_body,response_headers > "/run/benchmark/$BENCHMARK_CSV"
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
