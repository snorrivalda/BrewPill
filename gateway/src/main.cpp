/*
 * BrewPill gateway (ESP32).
 *
 * Listens for Brew Pill BLE broadcasts (raw acceleration, temperature and
 * supply), turns them into tilt, gravity and corrected temperature with each
 * pill's calibration (sent from BrewPill.app to POST /api/cal, kept in flash
 * and keyed by the pill ID in the broadcast), and forwards each pill's latest reading:
 *   - to Brewfather's Custom Stream (at most every 15 minutes, their limit),
 *   - to any JSON webhook (every N seconds).
 *
 * Setup: with no Wi-Fi configured, or after a minute without Wi-Fi, the gateway
 * opens the "BrewPill-Setup" network (password: brewpill). Join it and a setup
 * page opens (or browse to http://192.168.4.1). Once on your Wi-Fi the same page
 * is at http://brewpill-gw.local.
 */
#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <map>
#include <vector>
#include <math.h>

static const char *AP_SSID = "BrewPill-Setup";
static const char *AP_PASS = "brewpill";            // WPA2 needs 8+ characters
static const char *HOSTNAME = "brewpill-gw";
static const char *DEFAULT_WEBHOOK = "https://bruko.net/api/brewpill";   // used until settings are first saved
static const uint32_t BREWFATHER_MIN_S = 15 * 60;   // Brewfather rejects faster posts
static const uint32_t STALE_S = 180;                // don't forward readings older than this
static const uint32_t AP_AFTER_S = 60;              // open setup network after this long offline

struct Settings {
    String ssid, pass, brewfather, webhook, webhookSecret, name;
    uint32_t webhookEvery = 60;
} cfg;

/* Per-pill calibration. A pill without one uses the placeholder curve. */
struct Cal {
    uint8_t axis = 0;                         // capsule long axis: 0 = X, 1 = Y, 2 = Z
    float poly[4] = {0.95f, 0.002f, 0, 0};    // SG = p0 + p1·t + p2·t² + p3·t³ (25° → 1.000, 75° → 1.100)
    float tempOffset = 0;                     // °C added to the pill's die temperature
};

static std::map<String, Cal> cals;             // key: pill ID; guarded by pillsMutex

struct Pill {
    String id;                                 // 8 hex digits from the payload = end of the MAC
    float tilt = 0, temp = 0, sg = 0;          // computed with the pill's calibration
    float rawTemp = 0;                         // die temperature as broadcast
    uint16_t vdd = 0;
    int16_t x = 0, y = 0, z = 0;
    int rssi = 0;
    uint8_t flags = 0;                         // bit 0 accelerometer ok, bit 2 32 kHz crystal
    bool calibrated = false;
    uint32_t seen = 0;               // millis() of the last broadcast
    uint32_t sentBF = 0, sentHook = 0;
    bool everBF = false, everHook = false;
    String lastBF = "never", lastHook = "never";
};

static std::map<String, Pill> pills;                 // key: pill BLE address
static SemaphoreHandle_t pillsMutex;               // BLE task writes, loop() reads
#define LOCK()   xSemaphoreTake(pillsMutex, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(pillsMutex)

static Preferences prefs;
static WebServer web(80);
static DNSServer dns;
static bool apOn = false;
static uint32_t offlineSince = 0;

// ---------- settings ----------

static void loadSettings() {
    prefs.begin("brewpill", true);
    cfg.ssid = prefs.getString("ssid", "");
    cfg.pass = prefs.getString("pass", "");
    cfg.brewfather = prefs.getString("bf", "");
    cfg.webhook = prefs.getString("hook", DEFAULT_WEBHOOK);
    cfg.webhookSecret = prefs.getString("hookKey", "");
    cfg.name = prefs.getString("name", "BrewPill");
    cfg.webhookEvery = prefs.getUInt("hookEvery", 60);
    prefs.end();
}

static void saveSettings() {
    prefs.begin("brewpill", false);
    prefs.putString("ssid", cfg.ssid);
    prefs.putString("pass", cfg.pass);
    prefs.putString("bf", cfg.brewfather);
    prefs.putString("hook", cfg.webhook);
    prefs.putString("hookKey", cfg.webhookSecret);
    prefs.putString("name", cfg.name);
    prefs.putUInt("hookEvery", cfg.webhookEvery);
    prefs.end();
}

// ---------- calibration ----------

/* "DDEEFF01", "aa:bb:dd:ee:ff:01" or "aabbddeeff01" → "DDEEFF01"; "" if malformed */
static String pillId(String s) {
    s.replace(":", "");
    s.trim();
    s.toUpperCase();
    if (s.length() == 12) s = s.substring(4);
    if (s.length() != 8) return "";
    for (char c : s) if (!isxdigit((unsigned char)c)) return "";
    return s;
}

/* All calibrations live in one NVS blob, so a save replaces them in a single
 * write: a reset mid-save keeps either the old set or the new one. */
struct CalEntry {
    char id[8];                                // pill ID, not NUL-terminated
    Cal cal;
};

/* Called with pillsMutex held */
static void loadCals() {
    prefs.begin("cal", true);
    size_t n = prefs.getBytesLength("all") / sizeof(CalEntry);
    std::vector<CalEntry> all(n);
    if (n && prefs.getBytes("all", all.data(), n * sizeof(CalEntry)) == n * sizeof(CalEntry))
        for (auto &e : all) if (e.cal.axis < 3) cals[String(e.id, sizeof e.id)] = e.cal;
    prefs.end();
}

/* Takes a snapshot so the flash write doesn't hold pillsMutex */
static void saveCals(const std::map<String, Cal> &snap) {
    std::vector<CalEntry> all;
    for (auto &kv : snap) {
        CalEntry e;
        memcpy(e.id, kv.first.c_str(), sizeof e.id);
        e.cal = kv.second;
        all.push_back(e);
    }
    prefs.begin("cal", false);
    if (all.empty()) prefs.remove("all");
    else prefs.putBytes("all", all.data(), all.size() * sizeof(CalEntry));
    prefs.end();
}

static std::map<String, Cal> calSnapshot() {
    LOCK();
    std::map<String, Cal> snap = cals;
    UNLOCK();
    return snap;
}

/* Tilt of the capsule's long axis from vertical, then gravity from the curve.
 * Same maths as tiltOf()/polyEval() in app/web/index.html. */
static void applyCal(Pill &p, const Cal &c) {
    float g[3] = {(float)p.x, (float)p.y, (float)p.z};
    int a = c.axis, b = (a + 1) % 3, d = (a + 2) % 3;
    p.tilt = atan2f(hypotf(g[b], g[d]), fabsf(g[a])) * (180.0f / (float)M_PI);
    const float *k = c.poly;
    p.sg = k[0] + p.tilt * (k[1] + p.tilt * (k[2] + p.tilt * k[3]));
    p.temp = p.rawTemp + c.tempOffset;
}

/* Called with pillsMutex held: recompute with the pill's current calibration */
static void reapply(Pill &p) {
    auto c = cals.find(p.id);
    p.calibrated = c != cals.end();
    applyCal(p, p.calibrated ? c->second : Cal());
}

// ---------- BLE ----------

class PillScanner : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (!dev.haveManufacturerData()) return;
        std::string d = dev.getManufacturerData();
        // Company 0xFFFF, payload version 2: ID and raw readings (see brewpill/main.c brewpill_packet)
        if (d.size() < 18 || (uint8_t)d[0] != 0xFF || (uint8_t)d[1] != 0xFF || d[2] != 2) return;
        auto u16 = [&](int i) { return (uint16_t)((uint8_t)d[i] | (uint8_t)d[i + 1] << 8); };
        String key = dev.getAddress().toString().c_str();
        LOCK();
        Pill &p = pills[key];
        p.flags = d[3];
        char id[9];
        snprintf(id, sizeof id, "%02X%02X%02X%02X", (uint8_t)d[4], (uint8_t)d[5], (uint8_t)d[6], (uint8_t)d[7]);
        p.id = id;
        p.rawTemp = (int16_t)u16(8) / 100.0f;
        p.vdd = u16(10);
        p.x = u16(12); p.y = u16(14); p.z = u16(16);
        p.rssi = dev.getRSSI();
        p.seen = millis();
        reapply(p);
        UNLOCK();
    }
};

static void startBLE() {
    BLEDevice::init("");
    BLEScan *scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new PillScanner(), true);   // true: report duplicates
    scan->setActiveScan(false);
    scan->setInterval(160);
    scan->setWindow(80);                                            // 50% duty: leave airtime for Wi-Fi
}

static void keepScanning() {
    static uint32_t last = 0;
    if (millis() - last < 30000 && last) return;
    last = millis();
    BLEScan *scan = BLEDevice::getScan();
    scan->stop();
    scan->clearResults();
    scan->start(0, nullptr, false);                                 // runs until stopped
}

// ---------- forwarding ----------

static String shortId(const String &key) {
    String s = key.substring(key.length() - 5);
    s.replace(":", "");
    s.toUpperCase();
    return s;
}

static int post(const String &url, const String &json, String &reply, const String &bearer = "") {
    HTTPClient http;
    WiFiClientSecure tls;
    WiFiClient plain;
    bool ok = url.startsWith("https://")
        ? (tls.setInsecure(), http.begin(tls, url))   // no CA store on the device
        : http.begin(plain, url);
    if (!ok) { reply = "bad URL"; return -1; }
    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/json");
    if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
    int code = http.POST(json);
    reply = code > 0 ? http.getString().substring(0, 120) : http.errorToString(code);
    http.end();
    return code;
}

static String pillName(const String &key) { return cfg.name + "-" + shortId(key); }

static String hookJson(const String &key, const Pill &p, uint32_t now, bool test = false) {
    char json[460];
    snprintf(json, sizeof json,
        "{\"device\":\"%s\",\"pill_id\":\"%s\",\"name\":\"%s\",\"sg\":%.4f,\"tilt\":%.2f,\"temp_c\":%.2f,"
        "\"vdd_mv\":%u,\"rssi\":%d,\"x_mg\":%d,\"y_mg\":%d,\"z_mg\":%d,"
        "\"calibrated\":%s,\"age_s\":%lu%s}",
        key.c_str(), p.id.c_str(), pillName(key).c_str(), p.sg, p.tilt, p.temp, p.vdd, p.rssi,
        p.x, p.y, p.z, p.calibrated ? "true" : "false",
        (unsigned long)((now - p.seen) / 1000), test ? ",\"test\":true" : "");
    return json;
}

/* One webhook post right now, marked "test":true. Uses the freshest pill
 * reading, or made-up values if no pill has been heard. Returns "code reply". */
static String sendTestHook() {
    String status;
    if (!cfg.webhook.length()) status = "no webhook URL saved";
    else if (WiFi.status() != WL_CONNECTED) status = "Wi-Fi not connected";
    else {
        String key = "00:00:00:00:00:00";
        Pill p;
        p.sg = 1.0500f; p.tilt = 45.0f; p.temp = 20.0f; p.vdd = 3000;
        LOCK();
        uint32_t now = millis();                    // inside the lock, so no pill is newer than now
        p.seen = now;
        const std::pair<const String, Pill> *best = nullptr;
        for (auto &kv : pills)
            if (!best || now - kv.second.seen < now - best->second.seen) best = &kv;
        if (best) { key = best->first; p = best->second; }
        UNLOCK();
        String reply;
        int code = post(cfg.webhook, hookJson(key, p, now, true), reply, cfg.webhookSecret);
        status = String(code) + " " + reply;
    }
    Serial.printf("Test webhook: %s\n", status.c_str());
    return status;
}

static void forward() {
    if (WiFi.status() != WL_CONNECTED) return;
    std::map<String, Pill> snap;
    LOCK();
    snap = pills;
    UNLOCK();

    uint32_t now = millis();
    for (auto &kv : snap) {
        const String &key = kv.first;
        const Pill &p = kv.second;
        if (now - p.seen > STALE_S * 1000) continue;
        String name = pillName(key);
        bool calibrated = p.calibrated;
        String status;

        if (cfg.brewfather.length() && (!p.everBF || now - p.sentBF >= BREWFATHER_MIN_S * 1000)) {
            char json[320];
            snprintf(json, sizeof json,
                "{\"name\":\"%s\",\"temp\":%.2f,\"temp_unit\":\"C\",\"gravity\":%.4f,"
                "\"gravity_unit\":\"G\",\"angle\":%.2f,\"battery\":%.2f,\"rssi\":%d,"
                "\"comment\":\"%s\"}",
                name.c_str(), p.temp, p.sg, p.tilt, p.vdd / 1000.0f, p.rssi,
                calibrated ? "" : "uncalibrated: send from BrewPill.app");
            String reply;
            int code = post(cfg.brewfather, json, reply);
            status = String(code) + " " + reply;
            Serial.printf("Brewfather %s: %s\n", name.c_str(), status.c_str());
            LOCK();
            Pill &live = pills[key];
            live.sentBF = now; live.everBF = true; live.lastBF = status;
            UNLOCK();
        }

        if (cfg.webhook.length() && (!p.everHook || now - p.sentHook >= cfg.webhookEvery * 1000)) {
            String reply;
            int code = post(cfg.webhook, hookJson(key, p, now), reply, cfg.webhookSecret);
            status = String(code) + " " + reply;
            Serial.printf("Webhook %s: %s\n", name.c_str(), status.c_str());
            LOCK();
            Pill &live = pills[key];
            live.sentHook = now; live.everHook = true; live.lastHook = status;
            UNLOCK();
        }
    }
}

// ---------- Wi-Fi ----------

static void setAP(bool on) {
    if (on == apOn) return;
    apOn = on;
    if (on) {
        WiFi.mode(cfg.ssid.length() ? WIFI_AP_STA : WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        dns.start(53, "*", WiFi.softAPIP());               // captive portal: every name → us
        Serial.printf("Setup network \"%s\" open at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
        dns.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        Serial.println("Setup network closed");
    }
}

static void manageWiFi() {
    if (!cfg.ssid.length()) { setAP(true); return; }
    if (WiFi.status() == WL_CONNECTED) {
        if (offlineSince) {
            Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
            MDNS.end();
            MDNS.begin(HOSTNAME);
            MDNS.addService("http", "tcp", 80);
        }
        offlineSince = 0;
        setAP(false);
    } else {
        if (!offlineSince) offlineSince = millis() | 1;
        if (millis() - offlineSince > AP_AFTER_S * 1000) setAP(true);
    }
}

// ---------- web page ----------

static String esc(const String &s) {
    String o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

static void handleRoot() {
    String h;
    h.reserve(5000);
    h += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>BrewPill Gateway</title><style>"
           "body{font:15px -apple-system,system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;"
           "background:#f4f3f0;color:#0b0b0b}"
           "@media(prefers-color-scheme:dark){body{background:#121211;color:#f0efec}"
           ".card{background:#1a1a19!important;border-color:#2e2e2c!important}input{background:#222220;color:#f0efec}}"
           ".card{background:#fcfcfb;border:1px solid #e3e2dd;border-radius:10px;padding:14px 16px;margin:12px 0}"
           "h1{font-size:20px}h2{font-size:15px;margin:0 0 8px}label{display:block;margin:10px 0 2px;font-size:13px;opacity:.8}"
           "input{width:100%;box-sizing:border-box;font:inherit;padding:7px 9px;border:1px solid #ccc;border-radius:6px}"
           "button{margin-top:14px;font:inherit;padding:8px 16px;border:0;border-radius:6px;background:#2a78d6;color:#fff}"
           "small{opacity:.7}td{padding:2px 12px 2px 0}</style></head><body><h1>BrewPill Gateway</h1>");

    h += F("<div class=card><h2>Status</h2><table>");
    h += "<tr><td>Wi-Fi</td><td>" + (WiFi.status() == WL_CONNECTED
            ? esc(WiFi.SSID()) + " · " + WiFi.localIP().toString() + " · " + String(WiFi.RSSI()) + " dBm"
            : String(cfg.ssid.length() ? "connecting…" : "not set up")) + "</td></tr>";
    h += "<tr><td>Uptime</td><td>" + String(millis() / 60000) + " min</td></tr></table></div>";

    std::map<String, Pill> snap;
    LOCK();
    snap = pills;
    UNLOCK();
    h += F("<div class=card><h2>Pills</h2>");
    if (snap.empty()) h += F("<small>No BrewPill heard yet.</small>");
    for (auto &kv : snap) {
        const Pill &p = kv.second;
        char line[220];
        snprintf(line, sizeof line, "SG %.4f · %.1f °C · tilt %.1f° · %.2f V · %d dBm · %lus ago",
                 p.sg, p.temp, p.tilt, p.vdd / 1000.0f, p.rssi, (unsigned long)((millis() - p.seen) / 1000));
        h += "<p><b>" + esc(pillName(kv.first)) + "</b> <small>ID " + esc(p.id) + "</small>";
        if (!p.calibrated) h += F(" <small>(uncalibrated: use “Send to gateway” in BrewPill.app)</small>");
        if (!(p.flags & 1)) h += F(" <small>(accelerometer not responding)</small>");
        h += "<br>" + String(line);
        h += "<br><small>Brewfather: " + esc(p.lastBF) + "<br>Webhook: " + esc(p.lastHook) + "</small></p>";
    }
    h += F("</div>");

    std::map<String, Cal> calSnap = calSnapshot();
    h += F("<div class=card><h2>Calibration</h2>");
    if (calSnap.empty()) h += F("<small>None stored. Calibrate in BrewPill.app and use “Send to gateway”.</small>");
    for (auto &kv : calSnap) {
        const Cal &c = kv.second;
        char line[200];
        snprintf(line, sizeof line, "%c axis · SG = %.6g + %.6g·t + %.6g·t² + %.6g·t³ · temp %+.2f °C",
                 "XYZ"[c.axis], c.poly[0], c.poly[1], c.poly[2], c.poly[3], c.tempOffset);
        h += "<form method=post action=/api/cal style='margin:8px 0'><b>Pill " + esc(kv.first) +
             "</b><br><small>" + String(line) + "</small>"
             "<input type=hidden name=device value=\"" + esc(kv.first) + "\">"
             "<input type=hidden name=delete value=1><input type=hidden name=page value=1>"
             "<button style='margin-top:6px;background:#a33'>Remove</button></form>";
    }
    h += F("</div>");

    if (cfg.webhook.length())
        h += F("<form class=card method=post action=/test><h2>Webhook test</h2>"
               "<small>Posts one reading now (the latest pill's, or sample values) with "
               "<code>\"test\":true</code> and the saved secret.</small><br>"
               "<button>Send test webhook</button></form>");

    h += F("<form class=card method=post action=/save><h2>Settings</h2>"
           "<label>Wi-Fi network</label><input name=ssid autocomplete=off value=\"");
    h += esc(cfg.ssid);
    h += F("\"><label>Wi-Fi password <small>(blank keeps the saved one)</small></label>"
           "<input name=pass type=password autocomplete=new-password>"
           "<label>Brewfather Custom Stream URL <small>(Settings → Power-ups → Custom Stream; blank = off)</small></label>"
           "<input name=bf placeholder='http://log.brewfather.net/stream?id=…' value=\"");
    h += esc(cfg.brewfather);
    h += F("\"><label>Webhook URL <small>(receives JSON with sg, tilt, temp_c…; blank = off)</small></label>"
           "<input name=hook placeholder='https://example.com/brewpill' value=\"");
    h += esc(cfg.webhook);
    h += F("\"><label>Webhook secret <small>(sent as <code>Authorization: Bearer …</code>; ");
    h += cfg.webhookSecret.length() ? F("one is saved; blank keeps it)") : F("none saved; blank = none)");
    h += F("</small></label><input name=hookKey type=password autocomplete=new-password>");
    if (cfg.webhookSecret.length())
        h += F("<label><input type=checkbox name=hookKeyClear style='width:auto'> Remove the saved secret</label>");
    h += F("<label>Webhook interval, seconds</label><input name=every type=number min=10 value=\"");
    h += String(cfg.webhookEvery);
    h += F("\"><label>Name in Brewfather / webhook</label><input name=name value=\"");
    h += esc(cfg.name);
    h += F("\"><button>Save and restart</button></form></body></html>");
    web.send(200, "text/html; charset=utf-8", h);
}

static void handleSave() {
    cfg.ssid = web.arg("ssid");
    cfg.ssid.trim();
    if (web.arg("pass").length()) cfg.pass = web.arg("pass");
    cfg.brewfather = web.arg("bf"); cfg.brewfather.trim();
    cfg.webhook = web.arg("hook"); cfg.webhook.trim();
    if (web.hasArg("hookKeyClear")) cfg.webhookSecret = "";
    else {
        String k = web.arg("hookKey");
        k.replace("\r", ""); k.replace("\n", "");   // keep it from splitting the Authorization header
        k.trim();
        if (k.length()) cfg.webhookSecret = k;
    }
    cfg.name = web.arg("name"); cfg.name.trim();
    if (!cfg.name.length()) cfg.name = "BrewPill";
    cfg.webhookEvery = max(10L, web.arg("every").toInt());
    saveSettings();
    web.send(200, "text/html; charset=utf-8",
             "<meta name=viewport content='width=device-width'><body style='font:16px system-ui;padding:20px'>"
             "Saved. The gateway is restarting and joining your Wi-Fi.<br><br>"
             "Then open <b>http://brewpill-gw.local</b> on that network.</body>");
    delay(1500);
    ESP.restart();
}

static void handleTest() {
    String status = sendTestHook();
    web.send(200, "text/html; charset=utf-8",
             "<meta name=viewport content='width=device-width'><body style='font:16px system-ui;padding:20px'>"
             "Test webhook to <b>" + esc(cfg.webhook) + "</b>:<br><br><code>" + esc(status) +
             "</code><br><br><a href=/>Back</a></body>");
}

static void sendJson(int code, const String &json) { web.send(code, "application/json", json); }

static void calError(const char *msg) { sendJson(400, String("{\"ok\":false,\"error\":\"") + msg + "\"}"); }

/* POST /api/cal: device (pill ID or MAC), axis (x/y/z or 0..2), p0..p3, temp_offset;
 * or device + delete=1 */
static void handleCalPost() {
    String key = pillId(web.arg("device"));
    if (!key.length()) return calError("device must be a pill ID (8 hex digits) or MAC");
    bool del = web.arg("delete") == "1";
    Cal c;
    if (!del) {
        String ax = web.arg("axis");
        ax.toLowerCase();
        int a = ax.length() == 1 ? String("xyz012").indexOf(ax[0]) : -1;
        if (a < 0) return calError("axis must be x, y, z or 0-2");
        if (!web.hasArg("p0") || !web.hasArg("p1")) return calError("p0 and p1 are required");
        c.axis = a % 3;
        for (int i = 0; i < 4; i++) c.poly[i] = web.arg("p" + String(i)).toFloat();   // missing → 0
        c.tempOffset = web.arg("temp_offset").toFloat();
        for (float v : c.poly) if (!isfinite(v)) return calError("bad coefficient");
        if (!isfinite(c.tempOffset)) return calError("bad temp_offset");
    }
    LOCK();
    if (del) cals.erase(key); else cals[key] = c;
    for (auto &kv : pills) if (kv.second.id == key) reapply(kv.second);
    std::map<String, Cal> snap = cals;
    UNLOCK();
    saveCals(snap);
    Serial.printf("Calibration %s for %s\n", del ? "removed" : "stored", key.c_str());
    if (web.hasArg("page")) { web.sendHeader("Location", "/"); web.send(303, "text/plain", ""); return; }
    sendJson(200, "{\"ok\":true}");
}

/* GET /api/cal: every stored calibration */
static void handleCalGet() {
    std::map<String, Cal> snap = calSnapshot();
    String j = "{";
    for (auto &kv : snap) {
        const Cal &c = kv.second;
        char item[200];
        snprintf(item, sizeof item, "%s\"%s\":{\"axis\":%u,\"poly\":[%.9g,%.9g,%.9g,%.9g],\"temp_offset\":%.3f}",
                 j.length() > 1 ? "," : "", kv.first.c_str(), c.axis,
                 c.poly[0], c.poly[1], c.poly[2], c.poly[3], c.tempOffset);
        j += item;
    }
    sendJson(200, j + "}");
}

/* One serial line per pill each minute, so reception can be checked over USB */
static void logPills() {
    std::map<String, Pill> snap;
    LOCK();
    snap = pills;
    UNLOCK();
    if (snap.empty()) { Serial.println("No BrewPill heard yet"); return; }
    for (auto &kv : snap) {
        const Pill &p = kv.second;
        Serial.printf("%s-%s  SG %.4f  %.1f C  tilt %.1f  %.2f V  %d dBm  %lus ago\n",
                      cfg.name.c_str(), shortId(kv.first).c_str(), p.sg, p.temp, p.tilt,
                      p.vdd / 1000.0f, p.rssi, (unsigned long)((millis() - p.seen) / 1000));
    }
}

// ---------- main ----------

void setup() {
    Serial.begin(115200);
    Serial.println("\nBrewPill gateway");
    pillsMutex = xSemaphoreCreateMutex();
    loadSettings();
    LOCK();
    loadCals();
    UNLOCK();
    Serial.printf("%u pill calibration(s) stored\n", (unsigned)cals.size());
    offlineSince = 1;                            // so the first connection announces itself

    WiFi.setHostname(HOSTNAME);
    WiFi.mode(WIFI_STA);
    if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    WiFi.setAutoReconnect(true);

    web.on("/", HTTP_GET, handleRoot);
    web.on("/save", HTTP_POST, handleSave);
    web.on("/test", HTTP_POST, handleTest);
    web.on("/api/cal", HTTP_POST, handleCalPost);
    web.on("/api/cal", HTTP_GET, handleCalGet);
    web.onNotFound([] {
        if (!apOn) { web.send(404, "text/plain", "Not found"); return; }
        web.sendHeader("Location", "http://192.168.4.1/");   // captive portal: everything → setup
        web.send(302, "text/plain", "");
    });
    web.begin();

    startBLE();
}

void loop() {
    manageWiFi();
    if (apOn) dns.processNextRequest();
    web.handleClient();
    keepScanning();

    if (Serial.available()) {
        int c = Serial.read();
        if (c == 't' || c == 'T') sendTestHook();
    }

    static uint32_t lastForward = 0;
    if (millis() - lastForward > 5000) {
        lastForward = millis();
        forward();
    }

    static uint32_t lastLog = 0;
    if (millis() - lastLog > 60000 || !lastLog) {
        lastLog = millis();
        logPills();
    }
    delay(2);
}
