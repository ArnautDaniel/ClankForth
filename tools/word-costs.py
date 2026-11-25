#!/usr/bin/env python3
"""What each Forth word's machine code cost in a callgrind profile: callgrind counts the native backend's code by
address (it has no names for it), and CLANKFORTH_CODE_MAP's list says which word each address is.

    CLANKFORTH_CODE_MAP=code.map valgrind --tool=callgrind --callgrind-out-file=cg.out ./clankforth app.fs
    tools/word-costs.py code.map cg.out [words]

(The out file is written as the program ends; `callgrind_control -d` writes one from a server still running.)
Prints how much of the run was machine code, then the words with the most instructions (30, or `words`) - a word's
own, not those of the words and primitives it calls: callgrind_annotate lists the C side by name."""
import bisect
import collections
import re
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    shown = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    code = []   # (start, size, name), by start
    with open(sys.argv[1], encoding="utf-8", errors="replace") as lines:
        for line in lines:
            parts = line.rstrip("\n").split(" ", 2)
            if len(parts) == 3:
                code.append((int(parts[0], 16), int(parts[1], 16), parts[2]))
    code.sort()
    starts = [start for start, _, _ in code]
    listing = subprocess.run(["callgrind_annotate", "--threshold=100", sys.argv[2]], capture_output=True, text=True,
                             check=True).stdout
    costs = collections.Counter()
    total = 0
    for line in listing.splitlines():
        if "PROGRAM TOTALS" in line:
            total = int(line.split()[0].replace(",", ""))
        found = re.match(r"\s*([\d,]+) \(.*?\)\s+\?\?\?:0x([0-9a-f]+)", line)   # (???: code with no file - ours)
        if not found:
            continue
        cost, address = int(found.group(1).replace(",", "")), int(found.group(2), 16)
        k = bisect.bisect_right(starts, address) - 1
        inside = k >= 0 and address < code[k][0] + code[k][1]
        costs[code[k][2] if inside else "(not in the map)"] += cost
    machine = sum(costs.values())
    if total == 0 or machine == 0:
        sys.exit("no machine code in that profile (is it callgrind's out file, and the map this run's?)")
    print(f"{machine:,} of {total:,} instructions in machine code ({100 * machine / total:.1f}%)")
    for name, cost in costs.most_common(shown):
        print(f"{cost:15,} {100 * cost / total:5.1f}%  {name}")


if __name__ == "__main__":
    main()
