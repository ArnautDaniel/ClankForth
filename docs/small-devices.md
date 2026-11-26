# Small devices: a Raspberry Pi and the like

ClankForth runs on 64-bit Linux - so on a Raspberry Pi 3, 4, 5 or Zero 2 W with **64-bit Raspberry Pi OS** - and
CI runs its whole test suite on ARM64 for every change. A 32-bit system is refused as it builds: an actor's address
holds its node's number over 32 bits, so a cell must be 64.

## Building on the Pi

```sh
sudo apt install cmake gcc libssl-dev        # libssl-dev only for TLS (https, TDS 8): leave it out to go without
git clone https://github.com/ArnautDaniel/ClankForth.git && cd ClankForth
cmake -S . -B build && cmake --build build --parallel
cd build && ctest                            # the tests: a minute or two on a Pi 4
./clankforth ../examples/hello-server.fs     # a web server of actors, on the Pi
```

## Smaller and lighter

| what | how | what it saves |
|---|---|---|
| no stack checker | `-DCLANKFORTH_CHECKER=OFF` | the checker's code; definitions compile faster (a stack comment is a comment) |
| no TLS | `-DCLANKFORTH_TLS=OFF` (or no OpenSSL found) | OpenSSL not linked: a smaller process, nothing to keep patched |
| built for size | `-DCMAKE_BUILD_TYPE=MinSizeRel` | the binary: about 650 KB stripped (x86-64), everything in it |
| a smaller dictionary | `CLANKFORTH_DICTIONARY_MB=4 ./clankforth app.fs` | the space reserved for words and data (16 MB at first; the whole web stack, jsondb and sql take about half a megabyte) |
| one node | don't `start-node` | a thread and a Forth each: a node per core is for a server's load, not a sensor's |

```sh
cmake -S . -B build-small -DCMAKE_BUILD_TYPE=MinSizeRel -DCLANKFORTH_CHECKER=OFF -DCLANKFORTH_TLS=OFF
cmake --build build-small --parallel && strip build-small/clankforth
```

- **No native code on ARM**: the native backend (machine code for each definition) is x86-64 only. On ARM every word
  runs threaded - the same language and results, slower in tight loops; `.native` says which a system has.
- **Memory**: a running system is about 12 MB (the runtime's tables and stacks, with or without the web stack
  loaded) - room enough on a Pi Zero 2 W's 512 MB for a web server, a database and many actors. An idle actor is
  about 1 KB (its stacks are a handler's, lent while it runs); a websocket held open, some 3 KB.
- **Idle is idle**: `serve` (and a node's loop) waits (`epoll` on Linux) for a socket, a timer or a message - a
  server with nothing to do takes no CPU, however many connections and actors it holds. Timers (`after`) wake it
  when they are due.
- `jsondb` fsyncs every write: on an SD card, batch writes (`db-batch`) or keep the database on a USB drive if it
  takes many.

## Hardware: GPIO, SPI, a radio

ClankForth has no words for a board's pins: those are a host's (docs/hosts.md) - a program in C that embeds
ClankForth and adds its own words with `forth_prim` (a word to set a pin through Linux's GPIO character device, one
to move bytes through `/dev/spidev0.0`), each told to an actor as its events come. `src/tool.c` is a host of a few
lines to start from.

A microcontroller without Linux (an nRF52, an ESP32) is out of ClankForth's reach: it is built on an operating
system's threads, sockets and files.
