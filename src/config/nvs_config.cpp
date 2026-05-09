#include "nvs_config.h"
#include <Preferences.h>

static Preferences s_prefs;

void nvsInit() {}

void nvsPutString(const char *key, const String &val) {
    s_prefs.begin("mayday", false);
    s_prefs.putString(key, val);
    s_prefs.end();
}

String nvsGetString(const char *key, const String &def) {
    s_prefs.begin("mayday", true);
    String v = s_prefs.getString(key, def);
    s_prefs.end();
    return v;
}

void nvsPutInt(const char *key, int val) {
    s_prefs.begin("mayday", false);
    s_prefs.putInt(key, val);
    s_prefs.end();
}

int nvsGetInt(const char *key, int def) {
    s_prefs.begin("mayday", true);
    int v = s_prefs.getInt(key, def);
    s_prefs.end();
    return v;
}
