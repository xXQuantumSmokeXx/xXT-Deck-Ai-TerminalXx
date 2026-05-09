#pragma once
#include <Arduino.h>

// All keys live in the "mayday" NVS namespace.
void     nvsInit();
void     nvsPutString(const char *key, const String &val);
String   nvsGetString(const char *key, const String &def = "");
void     nvsPutInt(const char *key, int val);
int      nvsGetInt(const char *key, int def = 0);
