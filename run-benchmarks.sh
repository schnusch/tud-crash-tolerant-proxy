#!/bin/sh
set -eu

export LOG_LEVEL=0
export BENCHMARK_DURATION=600

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

test_vegeta() {
  export BENCHMARK_CSV="vegeta/${1}/${BENCHMARK_HOST}/${BENCHMARK_FILE}-${BENCHMARK_PARALLEL}c-${BENCHMARK_DURATION}s.csv"
  export BENCHMARK_FILE BENCHMARK_PARALLEL
  (
    export BENCHMARK_HOST="${BENCHMARK_HOST}."
    PS4='$ '
    set -x
    mkdir -p "benchmark/${BENCHMARK_CSV%/*}"
    exec nix run -L ".?submodules=1#compose.${1}.vegeta"
  ) || return $?
}

while read BENCHMARK_HOST xfrm; do
  for BENCHMARK_FILE in 1K 1M 16M 256M; do
    for BENCHMARK_PARALLEL in 1 10 100 1000; do
      [ -z "${BENCHMARK_HOST##\#*}" ] || test_vegeta "$xfrm" < /dev/null
    done
  done
done << EOF
haproxy  transform_nop
nginx    transform_nop
baseline transform_nop
proxy    transform_nop
haproxy  transform_headers
baseline transform_headers
proxy    transform_headers
EOF
