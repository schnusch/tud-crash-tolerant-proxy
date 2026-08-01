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
}
