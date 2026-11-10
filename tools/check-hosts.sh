#!/bin/sh
# The hosts' suites with this checkout as their ClankForth: what "done" means for a change here (CLAUDE.md).
#
#   tools/check-hosts.sh                  the hosts checked out beside this repo
#   tools/check-hosts.sh ../ClankMUD ...  these (each the folder with the host's CMakeLists.txt)
#
# Each host is configured with CLANKFORTH_DIR set to this checkout (its submodule left as it is), built in a
# folder of its own under CHECK_HOSTS_BUILD (default: the temporary folder), and its ctest run. The exit status is
# the number of hosts that failed. A host needs what its own build needs (SDL3, the game's data for its tests).
set -u

here=$(cd "$(dirname "$0")/.." && pwd)
build_root=${CHECK_HOSTS_BUILD:-${TMPDIR:-/tmp}/clankforth-hosts}

if [ $# -eq 0 ]; then   # (the folders beside this one whose build takes a CLANKFORTH_DIR)
    for dir in "$here"/../*/ "$here"/../*/*/; do
        if [ "$(cd "$dir" && pwd)" != "$here" ] && grep -qs CLANKFORTH_DIR "$dir/CMakeLists.txt"; then
            set -- "$@" "$dir"
        fi
    done
fi
if [ $# -eq 0 ]; then
    echo "check-hosts: no host found beside $here (give their folders)" >&2
    exit 1
fi

failed=0
mkdir -p "$build_root"
for host in "$@"; do
    host=$(cd "$host" && pwd) || { failed=$((failed + 1)); continue; }
    name=$(basename "$host")
    build="$build_root/$name"
    log="$build_root/$name.log"
    echo "== $name ($host)"
    if ! { cmake -S "$host" -B "$build" -G Ninja -DCLANKFORTH_DIR="$here" && ninja -C "$build"; } > "$log" 2>&1; then
        tail -n 30 "$log"
        echo "== $name: it did not build (the whole log: $log)"
        failed=$((failed + 1))
    elif ! (cd "$build" && SDL_AUDIO_DRIVER=dummy ctest --output-on-failure) > "$log" 2>&1; then
        tail -n 30 "$log"
        echo "== $name: its tests FAILED (the whole log: $log)"
        failed=$((failed + 1))
    else
        tail -n 3 "$log"
    fi
done
echo "== $(($# - failed)) of $# hosts pass"
exit "$failed"
