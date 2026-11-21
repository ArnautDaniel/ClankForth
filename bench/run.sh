#!/bin/sh
# Times each benchmark case: bench/run.sh [path/to/clankforth]
tool=${1:-build-release/clankforth}
dir=$(dirname "$0")
for case in "30 fib drop" "50000000 sum-loop drop" "300000 collatz" "5000000 locals-loop" "5000000 tuples" "300000 lists"; do
    start=$(date +%s.%N)
    echo "$case bye" | "$tool" "$dir/bench.fs" -i >/dev/null
    end=$(date +%s.%N)
    printf '%-24s %.2fs\n' "$case" "$(echo "$end - $start" | bc)"
done
