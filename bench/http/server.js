// The web benchmark's routes for Node and Bun (bench/http/run.sh): no framework, what each runtime's own server gives.
//   PORT=8102 node bench/http/server.js [workers]      PORT=8103 bun bench/http/server.js   (one process: run several)
const port = +(process.env.PORT || 8102);
const body = () => JSON.stringify({ message: "Hello, World!", id: 42, ok: true });

if (typeof Bun !== "undefined") {
  Bun.serve({
    port,
    reusePort: true, // (several processes on the one port)
    fetch(req) {
      const path = new URL(req.url).pathname;
      if (path === "/plain") return new Response("Hello, World!");
      if (path.startsWith("/hello/")) return new Response(`Hello, ${decodeURIComponent(path.slice(7))}!`);
      if (path === "/json") return new Response(body(), { headers: { "Content-Type": "application/json" } });
      return new Response(null, { status: 404 });
    },
  });
} else {
  const cluster = require("node:cluster");
  const workers = +(process.argv[2] || 1);
  if (cluster.isPrimary && workers > 1) {
    for (let i = 0; i < workers; i++) cluster.fork();
  } else {
    require("node:http")
      .createServer((req, res) => {
        const path = req.url;
        if (path === "/plain") {
          res.writeHead(200, { "Content-Type": "text/plain" });
          res.end("Hello, World!");
        } else if (path.startsWith("/hello/")) {
          res.writeHead(200, { "Content-Type": "text/plain" });
          res.end(`Hello, ${decodeURIComponent(path.slice(7))}!`);
        } else if (path === "/json") {
          res.writeHead(200, { "Content-Type": "application/json" });
          res.end(body());
        } else {
          res.writeHead(404);
          res.end();
        }
      })
      .listen(port);
  }
}
