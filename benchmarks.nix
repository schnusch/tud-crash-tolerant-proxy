{ pkgs }:
{
  # https://stackoverflow.com/a/34785677
  ab = ''
    PS4='$ '
    set -x
    exec ${pkgs.apacheHttpd}/bin/ab \
      -n"$BENCHMARK_REQUESTS" \
      -c"$BENCHMARK_PARALLEL" \
      -r \
      -g"/run/benchmark/$BENCHMARK_TSV" \
      "http://$BENCHMARK_HOST/$BENCHMARK_FILE"
  '';

  cassowary = ''
    PS4='$ '
    set -x
    ${pkgs.cassowary}/bin/cassowary run \
      --url="http://''${BENCHMARK_HOST:?}/''${BENCHMARK_FILE:?}" \
      --requests="''${BENCHMARK_REQUESTS:?}" \
      --concurrency="''${BENCHMARK_PARALLEL:?}" \
      --disable-keep-alive \
      --timeout=30 \
      --raw-output \
      --json-metrics \
      --json-metrics-file="/run/benchmark/''${BENCHMARK_JSON:?}"
    ${pkgs.coreutils}/bin/mv raw.csv "/run/benchmark/''${BENCHMARK_CSV:?}"
  '';

  vegeta = ''
    PS4='$ '
    set -x
    ${pkgs.vegeta}/bin/vegeta attack \
        -connections="''${BENCHMARK_PARALLEL:?}" \
        -max-connections="''${BENCHMARK_PARALLEL:?}" \
        -workers="$((''${BENCHMARK_PARALLEL:?} * 2))" \
        -max-workers="$((''${BENCHMARK_PARALLEL:?} * 2))" \
        -rate=0 \
        -duration="''${BENCHMARK_DURATION:?}s" << eof \
      | ${pkgs.gzip}/bin/gzip --fast \
      > vegeta.gz
    GET http://''${BENCHMARK_HOST:?}/''${BENCHMARK_FILE:?}
    eof
    ${pkgs.coreutils}/bin/ls -dhlp vegeta.gz
    { echo "timestamp_ns,status_code,latency_ns,bytes_out,bytes_in,error,response_body,attack_name,sequence_number,method,url,response_headers"
      ${pkgs.gzip}/bin/gzip -d < vegeta.gz | ${pkgs.vegeta}/bin/vegeta encode -to=csv
    } | ${pkgs.xan}/bin/xan drop response_body,response_headers > "/run/benchmark/''${BENCHMARK_CSV:?}"
  '';

  wrk = ''
    PS4='$ '
    set -x
    exec ${pkgs.wrk}/bin/wrk \
      --connections="''${BENCHMARK_PARALLEL:?}" \
      --duration=2 \
      --latency \
      "http://''${BENCHMARK_HOST:?}:80/''${BENCHMARK_FILE:?}"
  '';
}
