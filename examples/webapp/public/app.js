// The page's side: the visit count (JSON), the clock (server-sent events), the chat (a WebSocket).
fetch("/visits").then(r => r.json()).then(v => {
  document.getElementById("visits").textContent = `Visit ${v.visits}, answered by node ${v.node}.`;
});

new EventSource("/clock").addEventListener("tick", e => {
  document.getElementById("clock").textContent = new Date(Number(e.data) * 1000).toLocaleTimeString();
});

const log = document.getElementById("log");
const chat = new WebSocket(`ws://${location.host}/chat`);
chat.onmessage = e => {
  const line = document.createElement("div");
  line.textContent = e.data;
  log.append(line);
  log.scrollTop = log.scrollHeight;
};
document.getElementById("say").onsubmit = e => {
  e.preventDefault();
  const text = document.getElementById("text");
  if (text.value) chat.send(text.value);
  text.value = "";
};
