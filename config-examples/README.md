# SD card examples

Templates for the files the firmware reads from the **root of the SD card** at boot.
Nothing here is needed to build. Copy the ones you want to the card.

## Default — one endpoint

Copy `portal.txt` to the card root and put the complete URL of your endpoint in it.

The endpoint receives:

```txt
POST <your url>
{"message": "...", "agent_context": {"personality_profile": "Standard"}}
```

and must answer:

```json
{"response": "..."}
```

That is the whole setup — no assistant file, no key on the device. Extra request keys
can be ignored. `examples/endpoint/server.py` is a small reference implementation you
run yourself, with the provider key in your own environment.

## MayDay portal (advanced)

The two-assistant backend this firmware was developed against. Copy `mayday-portal/*`
to the card root. `assist1.txt` and `assist2.txt` carry the assistant slug, so requests
go to `{portal.txt}/{slug}/simple`.

## Personas (optional)

Copy `personas/` to the card root. The folder name and the `p1` / `p2` / `p3` file names
are fixed. Format:

```txt
NAME
Title or short role
profile: Standard
Notes - kept on the card, never sent.
```

Allowed profiles: `Standard`, `Tactical`, `Relaxed`, `Maximum Sass`.

## Other files

- `wifi.txt` — SSID on line 1, password on line 2
- `crypto.txt` — up to six CoinGecko coin IDs, one per line

## What is not here

`openai.txt`, `anthropic.txt`, `groq.txt` and `ollama-local.txt` are gone. The firmware
has no provider mode and no API key field, so the deck cannot call a provider directly.
Those formats belonged to `examples/AiTerminal/AiTerminal.ino`, the standalone sketch.
Point the deck at an endpoint instead.
