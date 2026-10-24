#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── Quantum-Meteor Worker ────────────────────────────────────────────────────
// One host for the data screens. Plain HTTP is deliberate: the worker answers
// on :80 without a redirect, the payloads are public, and skipping the TLS
// handshake keeps a screen open under a second. The one exception is SWPC's
// Kp forecast, which has no worker route and is fetched over TLS.
#define QM_BASE "http://quantum-meteor.qsmoke.workers.dev"

// GET a URL and parse the body into `doc`. Picks TLS for https:// callers so a
// module never has to know which client to build. Payloads here are small
// (133 B – 5 KB); a screen that pulls anything larger should stream it instead.
inline bool httpJson(const char *url, JsonDocument &doc, uint16_t timeoutMs = 9000) {
    const bool secure = strncmp(url, "https://", 8) == 0;

    WiFiClient       plain;
    WiFiClientSecure tls;
    if (secure) tls.setInsecure();

    HTTPClient http;
    http.setTimeout(timeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(secure ? (WiFiClient &)tls : (WiFiClient &)plain, url)) return false;

    http.addHeader("User-Agent", "T-Deck-AI/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    // getString() rather than getStream(): /ews and /news come back without a
    // Content-Length (chunked), and a raw stream would hand ArduinoJson the
    // chunk-size lines. The bodies here are 0.1–7 KB, so the copy is cheap.
    String body = http.getString();
    http.end();
    if (body.isEmpty()) return false;

    return !deserializeJson(doc, body);
}
