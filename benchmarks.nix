{ lib, pkgs }:
{
  # https://stackoverflow.com/a/34785677
  ab = ''
    PS4='$ '
    set -x
    exec ${lib.getExe' pkgs.apacheHttpd "ab"} \
      -n"$BENCHMARK_REQUESTS" \
      -c"$BENCHMARK_PARALLEL" \
      -r \
      -g"/run/benchmark/$BENCHMARK_TSV" \
      "http://$BENCHMARK_HOST/$BENCHMARK_FILE"
  '';

  cassowary = ''
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

  vegeta = ''
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

  wrk = ''
    PS4='$ '
    set -x
    exec ${lib.getExe pkgs.wrk} \
      --connections="$BENCHMARK_PARALLEL" \
      --duration=2 \
      --latency \
      "http://$BENCHMARK_HOST:80/$BENCHMARK_FILE"
  '';
}
