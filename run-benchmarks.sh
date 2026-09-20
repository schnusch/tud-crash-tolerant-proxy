#!/bin/sh
set -eu

export LOG_LEVEL=0

test_inject() {
  mkdir -p benchmark
  temp=$(mktemp -dp benchmark)
  (
    export BENCHMARK_CSV="${temp#*/}/part.csv"
    PS4='$ '
    set -x
    echo 'start,parallel,accept,write' > "$temp/result.csv"
    for BENCHMARK_PARALLEL in $(seq 1000 | tac); do
      for retry in $(seq 10); do
        export BENCHMARK_PARALLEL
        nix run -L ".?submodules=1#compose.transform_nop.${1}" || continue
        cat "$temp/part.csv" >> "$temp/result.csv"
      done
    done
    mv "$temp/result.csv" "benchmarks/${1}.csv"
  ) || e=$?
  rm -fr "$temp"
  return ${e:-0}
}

for test in fail-accept; do
  test_inject "$test"
done
