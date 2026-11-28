#!/usr/bin/env python3
"""The native backend checked against threaded code: random definitions of the words it writes in line (literals,
stack words, arithmetic, comparisons, within, true false, if/else, do/?do/+loop/leave, i j, >r r@ r>, locals and
their frame, @ ! c@ c! - of a buffer, and of what is no address - exit, unloop exit, calls of small definitions
with nothing but such words in them, which it writes into their callers, and sequences' walks - each map filter
find reduce over a list, a string, bytes, their quotations written into them, some seeing the word's locals), each
run under catch four ways - machine code with its fast paths, the same with every path of a run sent round by its
stub (CLANKFORTH_STUBS_ONLY: the stack's top written from its registers to its cells and read back, at each word),
machine code calling every word (CLANKFORTH_CALLS_ONLY), and threaded code (CLANKFORTH_THREADED) - and what each
leaves compared. After an error only
the message is compared: the cells under the depth catch restores hold whatever was written last, and a fast path
writes no value no one reads.

    tools/native-diff.py build-min/clankforth [first-seed [seeds [definitions]]]

Use a build without the stack checker (-DCLANKFORTH_CHECKER=OFF): the definitions are random, their stack comments
none. Exits 1 if any seed differs (its file and the four outputs are kept, named after it)."""
import os
import random
import subprocess
import sys

WORDS = ("dup drop swap over nip + - and or xor * = <> < > <= >= u< u> 0= 0<> 0< 0> 0<= 0>= "
         "1+ 1- invert negate cells true false within").split()
COMPARES = "= <> < > <= >= u< u> 0= 0<> 0< 0> 0<= 0>=".split()
# memory: a buffer's cells and bytes (aligned or not), and fetches and stores of what is no address (an error, the
# same words said) - never a number that might be one
MEMORY = ["buf @", "buf c@", "buf 5 + @", "buf 11 + c@", "buf !", "buf 8 + !", "buf 3 + !", "buf 9 + c!",
          "0 @", "0 c@", "42 @", "-8 c@", "0 !", "7 c!", "-16 !"]
LITERALS = [0, 1, -1, 2, 3, 7, -5, 100, 255, 2**31 - 1, -2**31, 2**31, -2**31 - 1, 2**40, -2**63, 2**63 - 1,
            123456789012]   # (both sides of what fits an instruction's 32 bits)
MAX_DEPTH = 3
# what a walk walks: lists (cells), strings and bytes (bytes) - none, one, several; `bb` is the program's own
SEQUENCES = ["{ 3 -1 250 7 }", "{ }", "{ 5 }", '" hello"', '" "', '" x"', "bb", "{ 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 }"]


class Generator:
    def __init__(self, seed):
        self.rnd = random.Random(seed)
        self.helpers = []   # small definitions, straight through: what the backend writes into their callers

    def helper(self):
        """a definition of a few words with no branch in it (some with locals, some calling an earlier one)"""
        r = self.rnd
        locals_ = r.random() < 0.3
        body = []
        for _ in range(r.randint(1, 6)):
            x = r.random()
            if x < 0.3:
                body.append(self.literal())
            elif x < 0.75:
                body.append(r.choice(WORDS))
            elif x < 0.82:
                body += r.choice(MEMORY).split()
            elif x < 0.9 and self.helpers:
                body.append(r.choice(self.helpers))
            elif locals_:
                body.append(r.choice("abd"))
        name = f"h{len(self.helpers)}"
        self.helpers.append(name)
        return f": {name} {'{: a b | d :} ' if locals_ else ''}{' '.join(body)} ;"

    def literal(self):
        r = self.rnd
        return str(r.choice(LITERALS) if r.random() < 0.7 else r.randint(-1000, 1000))

    def loop(self, depth, locals_, pushed, loops):
        r = self.rnd
        limit, start = r.randint(-2, 5), r.randint(-2, 5)
        kind = r.choice(["do", "?do"])
        step = r.choice(["loop", "loop", f"{r.choice([1, 2, -1, 3])} +loop"])
        down = step.startswith("-")
        start, limit = (max(start, limit), min(start, limit)) if down else (min(start, limit), max(start, limit))
        if kind == "do" and start == limit:   # (do runs once at least: bounded only if it starts short of its limit)
            limit += -1 if down else 1
        body = self.seq(depth + 1, locals_, pushed, loops + 1)
        if r.random() < 0.5:
            body = ["i"] + body
        if r.random() < 0.15:
            body += ["i", str(r.randint(0, 3)), "=", "if", "leave", "then"]
        return [str(limit), str(start), kind] + body + [step]

    def walk(self, depth, locals_):
        """a sequence walked by a quotation (its own loops, >r and exit: none of the word's are open in it)"""
        r = self.rnd
        body = self.seq(depth + 1, locals_, False, 0)
        kind = r.choice(["each", "each", "map", "filter", "find", "reduce"])
        seq = r.choice(SEQUENCES)
        if kind == "each":
            return [seq, "[:"] + body + [";]", "each"]
        if kind == "map":   # (what it makes: its length and its last item, if it has one)
            return [seq, "[:"] + body + [";]", "map", "dup", "length", "swap", "dup", "length", "if", "-1", "swap",
                    "nth", "else", "drop", "then"]
        if kind == "filter":
            return [seq, "[:"] + body + [r.choice(COMPARES), ";]", "filter", "length"]
        if kind == "find":
            return [seq, "[:"] + body + [r.choice(COMPARES), ";]", "find"]
        return [seq, self.literal(), "[:"] + body + [";]", "reduce"]

    def seq(self, depth, locals_, pushed, loops):
        """words for a definition's body: pushed - a >r is open; loops - how many do loops are"""
        r = self.rnd
        out = []
        for _ in range(r.randint(1, 7)):
            x = r.random()
            if x < 0.3:
                out.append(self.literal())
            elif x < 0.65:
                out.append(r.choice(WORDS))
            elif x < 0.7 and depth < MAX_DEPTH:
                out += [self.literal(), r.choice(COMPARES), "if"] + self.seq(depth + 1, locals_, pushed, loops)
                if r.random() < 0.5:
                    out += ["else"] + self.seq(depth + 1, locals_, pushed, loops)
                out.append("then")
            elif x < 0.75 and depth < MAX_DEPTH:
                out += [r.choice(COMPARES), "if"] + self.seq(depth + 1, locals_, pushed, loops) + ["then"]
            elif x < 0.8 and depth < MAX_DEPTH - 1:
                out += self.loop(depth, locals_, pushed, loops)
            elif x < 0.83 and depth < MAX_DEPTH - 1:
                out += self.walk(depth, locals_)
            elif x < 0.85:
                out += ["dup", ">r"] + self.seq(depth + 1, locals_, True, loops)
                if r.random() < 0.4:
                    out += ["r@", "+"]
                out += ["r>", r.choice(["+", "drop", "xor"])]
            elif x < 0.86 and depth < MAX_DEPTH and not pushed and loops == 0:   # (an exit with a >r or a loop
                out += [r.choice(COMPARES), "if", self.literal(), "exit", "then"]  # open is no program at all)
            elif x < 0.87 and depth < MAX_DEPTH and not pushed and loops == 1:
                out += ["i", str(r.randint(0, 3)), "=", "if", "unloop", "exit", "then"]
            elif x < 0.88 and loops > 0:
                out.append(r.choice(["i", "i", "j"]) if loops >= 2 and not pushed else "i")
            elif x < 0.91:
                out += r.choice(MEMORY).split()
            elif x < 0.95 and self.helpers:
                out.append(r.choice(self.helpers))
            elif locals_ and x < 0.98:
                out.append(r.choice("abcde"))
            elif locals_:
                out += [self.literal(), "to", r.choice("abc")]
        return out

    def program(self, n):
        lines = ["variable depth0",
                 "create buf 4 cells allot  buf 4 cells 0 fill",
                 ": bb ( -- b )  <bytes> 65 over b-push 200 over b-push 7 over b-push 0 over b-push ;",
                 ": show ( -- )  depth depth0 @ - 0 max 0 ?do . loop cr ;",
                 ": clear ( -- )  depth 0 ?do drop loop ;"]
        for k in range(n):
            if self.rnd.random() < 0.3:
                lines.append(self.helper())
            locals_ = self.rnd.random() < 0.3
            body = self.seq(0, locals_, False, 0)
            lines.append(f": t{k} {'{: a b c | d e :} ' if locals_ else ''}{' '.join(body)} ;")
            given = " ".join(self.literal() for _ in range(self.rnd.randint(0, 4)))
            lines.append(f": r{k} {given} {'1 2 3 ' if locals_ else ''}['] t{k} catch "
                         f"if  .\" E:\" error-message type  depth depth0 !  then ;")
            lines.append(f"depth depth0 !  r{k} .\" {k}:\" show clear")
        return "\n".join(lines) + "\n"


MODES = {"native": {}, "stubs-only": {"CLANKFORTH_STUBS_ONLY": "1"}, "calls-only": {"CLANKFORTH_CALLS_ONLY": "1"},
         "threaded": {"CLANKFORTH_THREADED": "1"}}


def run(tool, path, env):
    done = subprocess.run([tool, path], env={**os.environ, **env}, capture_output=True, timeout=120)
    return done.stdout + done.stderr + f"\n(exit {done.returncode})\n".encode()


def main():
    tool = os.path.abspath(sys.argv[1])
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    seeds = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 400
    failed = 0
    for seed in range(first, first + seeds):
        path = f"native-diff-{seed}.fs"
        with open(path, "w") as out:
            out.write(Generator(seed).program(n))
        outputs = {mode: run(tool, path, env) for mode, env in MODES.items()}
        if len(set(outputs.values())) == 1:
            os.remove(path)
            continue
        failed += 1
        for mode, text in outputs.items():
            with open(f"native-diff-{seed}.{mode}.txt", "wb") as out:
                out.write(text)
        print(f"seed {seed}: the four ways differ - {path}, native-diff-{seed}.*.txt")
    print(f"{seeds} seeds of {n} definitions: {seeds - failed} the same, {failed} differ")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
