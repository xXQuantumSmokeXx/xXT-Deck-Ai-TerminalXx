#!/usr/bin/env python3
"""
Minimal chat endpoint for the T-Deck AI Terminal.

The deck posts:   {"message": "...", "agent_context": {"personality_profile": "..."}}
and expects back: {"response": "..."}

This puts that contract in front of any OpenAI-compatible API (Ollama, LM Studio,
vLLM, OpenAI, Groq, OpenRouter). It runs on your own machine and keeps the provider
key in your environment — the deck never sees a key.

    # local Ollama, no key needed
    python server.py

    # any OpenAI-compatible provider
    UPSTREAM=https://api.openai.com/v1 MODEL=gpt-4o-mini API_KEY=sk-... python server.py

Environment:
    UPSTREAM  provider base URL  (default http://127.0.0.1:11434/v1)
    MODEL     model name         (default llama3)
    API_KEY   provider key       (optional, sent as Bearer)
    TOKEN     shared secret      (optional, requires X-Portal-Token to match)
    HOST      listen address     (default 0.0.0.0 so the deck can reach it)
    PORT      listen port        (default 8080)

Then put http://<your-pc-ip>:8080/chat in portal.txt on the SD card.
Set TOKEN if this machine is reachable by anyone you would not hand your key to.
"""

import json
import os
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM = os.environ.get("UPSTREAM", "http://127.0.0.1:11434/v1").rstrip("/")
MODEL    = os.environ.get("MODEL", "llama3")
API_KEY  = os.environ.get("API_KEY", "")
TOKEN    = os.environ.get("TOKEN", "")
HOST     = os.environ.get("HOST", "0.0.0.0")
PORT     = int(os.environ.get("PORT", "8080"))

# The four profiles the firmware can send. Everything else falls back to Standard.
PROFILES = {
    "Standard":     "You are a helpful assistant. Be concise and direct.",
    "Tactical":     "You are a terse field operator. Answer in short, practical sentences.",
    "Relaxed":      "You are a calm assistant. Keep answers short and easy going.",
    "Maximum Sass": "You are a witty assistant with a sharp tongue. Keep answers short.",
}
SCREEN_NOTE = " You are running on a T-Deck handheld terminal with a 320x240 screen, so keep replies brief."


def ask_upstream(message: str, profile: str) -> str:
    messages = [
        {"role": "system", "content": PROFILES.get(profile, PROFILES["Standard"]) + SCREEN_NOTE},
        {"role": "user", "content": message},
    ]
    payload = json.dumps({"model": MODEL, "messages": messages, "max_tokens": 512}).encode()
    req = urllib.request.Request(
        UPSTREAM + "/chat/completions",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    if API_KEY:
        req.add_header("Authorization", "Bearer " + API_KEY)
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.loads(resp.read().decode())
    return data["choices"][0]["message"]["content"].strip()


class Handler(BaseHTTPRequestHandler):
    def _send(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._send(200, {"status": "ok", "model": MODEL, "upstream": UPSTREAM})

    def do_POST(self):
        if TOKEN and self.headers.get("X-Portal-Token", "") != TOKEN:
            self._send(401, {"error": "bad or missing X-Portal-Token"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length).decode() or "{}")
            message = str(body.get("message", "")).strip()
            if not message:
                self._send(400, {"error": "missing message"})
                return
            profile = str(body.get("agent_context", {}).get("personality_profile", "Standard"))
            self._send(200, {"response": ask_upstream(message, profile)})
        except urllib.error.HTTPError as e:
            self._send(502, {"error": "upstream %d: %s" % (e.code, e.reason)})
        except Exception as e:  # noqa: BLE001 - a reference server should answer, not crash
            self._send(502, {"error": str(e)})

    def log_message(self, fmt, *args):
        print("[endpoint] " + fmt % args)


if __name__ == "__main__":
    print("[endpoint] model=%s upstream=%s" % (MODEL, UPSTREAM))
    print("[endpoint] listening on http://%s:%d/chat" % (HOST, PORT))
    if not TOKEN:
        print("[endpoint] no TOKEN set - anyone who can reach this port can use your key")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
