/*
 * BrewPill gateway (ESP32).
 *
 * Listens for Brew Pill BLE broadcasts and forwards each pill's latest reading:
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

static const char *AP_SSID = "BrewPill-Setup";
static const char *AP_PASS = "brewpill";            // WPA2 needs 8+ characters
static const char *HOSTNAME = "brewpill-gw";
static const uint32_t BREWFATHER_MIN_S = 15 * 60;   // Brewfather rejects faster posts
static const uint32_t STALE_S = 180;                // don't forward readings older than this
static const uint32_t AP_AFTER_S = 60;              // open setup network after this long offline

struct Settings {
    String ssid, pass, brewfather, webhook, name;
    uint32_t webhookEvery = 60;
} cfg;

struct Pill {
    float tilt = 0, temp = 0, sg = 0;
    uint16_t vdd = 0;
    int16_t x = 0, y = 0, z = 0;
    int rssi = 0;
    uint8_t flags = 0;
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
    cfg.webhook = prefs.getString("hook", "");
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
    prefs.putString("name", cfg.name);
    prefs.putUInt("hookEvery", cfg.webhookEvery);
    prefs.end();
}

// ---------- BLE ----------

class PillScanner : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (!dev.haveManufacturerData()) return;
        std::string d = dev.getManufacturerData();
        // Company 0xFFFF, payload version 1 (see brewpill/main.c brewpill_packet)
        if (d.size() < 18 || (uint8_t)d[0] != 0xFF || (uint8_t)d[1] != 0xFF || d[2] != 1) return;
        auto u16 = [&](int i) { return (uint16_t)((uint8_t)d[i] | (uint8_t)d[i + 1] << 8); };
        String key = dev.getAddress().toString().c_str();
        LOCK();
        Pill &p = pills[key];
        p.flags = d[3];
        p.tilt = u16(4) / 100.0f;
        p.temp = (int16_t)u16(6) / 100.0f;
        p.vdd = u16(8);
        p.sg = u16(10) / 10000.0f;
        p.x = u16(12); p.y = u16(14); p.z = u16(16);
        p.rssi = dev.getRSSI();
        p.seen = millis();
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

static int post(const String &url, const String &json, String &reply) {
    HTTPClient http;
    WiFiClientSecure tls;
    WiFiClient plain;
    bool ok = url.startsWith("https://")
        ? (tls.setInsecure(), http.begin(tls, url))   // no CA store on the device
        : http.begin(plain, url);
    if (!ok) { reply = "bad URL"; return -1; }
    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);
    reply = code > 0 ? http.getString().substring(0, 120) : http.errorToString(code);
    http.end();
    return code;
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
        String name = cfg.name + "-" + shortId(key);
        bool calibrated = p.flags & 2;
        String status;

        if (cfg.brewfather.length() && (!p.everBF || now - p.sentBF >= BREWFATHER_MIN_S * 1000)) {
            char json[320];
            snprintf(json, sizeof json,
                "{\"name\":\"%s\",\"temp\":%.2f,\"temp_unit\":\"C\",\"gravity\":%.4f,"
                "\"gravity_unit\":\"G\",\"angle\":%.2f,\"battery\":%.2f,\"rssi\":%d,"
                "\"comment\":\"%s\"}",
                name.c_str(), p.temp, p.sg, p.tilt, p.vdd / 1000.0f, p.rssi,
                calibrated ? "" : "pill uncalibrated");
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
            char json[400];
            snprintf(json, sizeof json,
                "{\"device\":\"%s\",\"name\":\"%s\",\"sg\":%.4f,\"tilt\":%.2f,\"temp_c\":%.2f,"
                "\"vdd_mv\":%u,\"rssi\":%d,\"x_mg\":%d,\"y_mg\":%d,\"z_mg\":%d,"
                "\"calibrated\":%s,\"age_s\":%lu}",
                key.c_str(), name.c_str(), p.sg, p.tilt, p.temp, p.vdd, p.rssi,
                p.x, p.y, p.z, calibrated ? "true" : "false",
                (unsigned long)((now - p.seen) / 1000));
            String reply;
            int code = post(cfg.webhook, json, reply);
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
        h += "<p><b>" + esc(cfg.name + "-" + shortId(kv.first)) + "</b>";
        if (!(p.flags & 2)) h += F(" <small>(uncalibrated: use “Write to pill” in BrewPill.app)</small>");
        h += "<br>" + String(line);
        h += "<br><small>Brewfather: " + esc(p.lastBF) + "<br>Webhook: " + esc(p.lastHook) + "</small></p>";
    }
    h += F("</div>");

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
    h += F("\"><label>Webhook interval, seconds</label><input name=every type=number min=10 value=\"");
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
    offlineSince = 1;                            // so the first connection announces itself

    WiFi.setHostname(HOSTNAME);
    WiFi.mode(WIFI_STA);
    if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    WiFi.setAutoReconnect(true);

    web.on("/", HTTP_GET, handleRoot);
    web.on("/save", HTTP_POST, handleSave);
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
