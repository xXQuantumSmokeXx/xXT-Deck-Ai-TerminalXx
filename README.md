# MayDay Terminal — AI Chat for LILYGO T-Deck

AI chat terminal firmware for the LILYGO T-Deck (ESP32-S3). Supports OpenAI, Anthropic Claude, Groq, Ollama, and any OpenAI-compatible backend.

## Hardware Required

- [LILYGO T-Deck](https://www.lilygo.cc/products/t-deck) (ESP32-S3 version)

## Flashing

1. Install [PlatformIO](https://platformio.org/)
2. Clone this repo
3. Run `pio run` to build
4. Copy `.pio/build/T-Deck/firmware.bin` to your SD card
5. Install via the [bmorcelli M5Launcher](https://github.com/bmorcelli/M5Stick-Launcher) or flash directly via USB

## SD Card Setup (Recommended)

Drop these files in the **root** of the SD card before first boot. They self-destruct after being read.

### `wifi.txt`
```
YourWiFiSSID
YourWiFiPassword
```

### `config.txt`
```
type: openai
key:  your-api-key-here
url:  https://api.groq.com/openai/v1
model: llama-3.1-8b-instant
```

> **Important:** Save files as plain text with no extra `.txt` extension. In Windows Notepad use File → Save As → set "Save as type" to **All Files**.

> **Security note:** `config.txt` and `wifi.txt` are **not** deleted automatically after being read — settings are saved to the device's internal memory (NVS). Once your device is configured and working, delete both files from the SD card to avoid leaving your API key and WiFi password sitting on it.

## Supported APIs

| Provider | type | url | example model |
|----------|------|-----|---------------|
| OpenAI | `openai` | `https://api.openai.com/v1` | `gpt-4o-mini` |
| Groq (free) | `openai` | `https://api.groq.com/openai/v1` | `llama-3.1-8b-instant` |
| Anthropic | `anthropic` | *(leave blank)* | `claude-3-haiku-20240307` |
| Ollama (local) | `openai` | `http://your-ip:11434/v1` | `llama3` |
| Any OpenAI-compatible | `openai` | your URL | your model |

## On-Device Commands

Type and press Enter:

| Command | Action |
|---------|--------|
| `setwifi` | Change WiFi credentials |
| `setapi` | Reconfigure API settings |
| `clear` | Clear chat and conversation history |

## Controls

- **Trackball UP** — scroll up through chat history
- **Trackball DOWN** — scroll down

## First Boot

1. Device reads `wifi.txt` and `config.txt` from SD card automatically
2. If files are not found, you will be prompted to enter settings manually
3. Settings are saved to device memory (NVS) — SD card files are not needed on subsequent boots

## API Spec (for custom backends)

**OpenAI-compatible:**
```
POST {url}/chat/completions
Authorization: Bearer {key}
{"model":"...","messages":[{"role":"user","content":"..."},...]}
→ {"choices":[{"message":{"content":"..."}}]}
```

**Anthropic:**
```
POST https://api.anthropic.com/v1/messages
x-api-key: {key}
anthropic-version: 2023-06-01
{"model":"...","max_tokens":1024,"messages":[...]}
→ {"content":[{"type":"text","text":"..."}]}
```
