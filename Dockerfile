# A Linux image of the clankforth tool: built here, run with an app's scripts mounted or copied in (/app).
#   docker build -t clankforth .
#   docker run -p 8080:8080 -v $PWD/app:/app clankforth /app/main.fs -- 8080
# It logs to stdout (JSON lines: logging's log-json, http.ops' request-log) for the platform's collector, and stops
# on SIGTERM after draining (on-shutdown; http-drain) - what a container orchestrator sends before it stops one.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build gcc libc6-dev libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCLANKFORTH_TESTS=OFF && ninja -C build clankforth-tool

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/* && useradd --system --home /app app
COPY --from=build /src/build/clankforth /usr/local/bin/clankforth
USER app
WORKDIR /app
ENTRYPOINT ["clankforth"]
