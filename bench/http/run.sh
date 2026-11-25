#!/bin/sh
# The web benchmark: requests a second for a server already listening - a bare request (wrk's own: one header) and
# one as a browser sends it (eleven headers, a cookie). A request's headers are most of what a server reads: the
# bare figure alone flatters one that reads them slowly.
#
#   bench/http/run.sh [port [path [seconds [connections]]]]        (8101 json 8 64)
#
#   BENCH_PORT=8101 build/clankforth bench/http/server.fs -- 1 &   bench/http/run.sh 8101
#   PORT=8102 node bench/http/server.js 1 &                        bench/http/run.sh 8102
#   PORT=8103 bun bench/http/server.js &                           bench/http/run.sh 8103
#
# Needs wrk. On a machine the load generator shares with the server, the workers past (cores - 2) have no core.
port=${1:-8101}
path=${2:-json}
secs=${3:-8}
conns=${4:-64}
url="http://127.0.0.1:$port/$path"

rate() { awk '/Requests\/sec/ { printf "%d", $2 }'; }
wrk -t2 -c"$conns" -d2s "$url" >/dev/null   # (warmed: its code compiled, its connections open)
bare=$(wrk -t2 -c"$conns" -d"${secs}s" "$url" | rate)
browser=$(wrk -t2 -c"$conns" -d"${secs}s" \
    -H "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36" \
    -H "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8" \
    -H "Accept-Language: en-US,en;q=0.9" \
    -H "Accept-Encoding: gzip, deflate, br" \
    -H "Cookie: session=0123456789abcdef0123456789abcdef; theme=dark; _ga=GA1.1.123456789.1700000000" \
    -H "Referer: http://127.0.0.1:$port/" \
    -H "Sec-Fetch-Dest: document" -H "Sec-Fetch-Mode: navigate" -H "Sec-Fetch-Site: same-origin" \
    -H "Upgrade-Insecure-Requests: 1" -H "Cache-Control: max-age=0" "$url" | rate)
echo "/$path on $port: a bare request $bare a second, a browser's $browser"
