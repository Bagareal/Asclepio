#include <WiFi.h>
#include <ESPping.h>
#include <WiFiUdp.h>
#include <WakeOnLan.h>
#include <WebServer.h>
#include <Preferences.h>

const char* WIFI_SSID     = "yourSSID";
const char* WIFI_PASSWORD = "yourPassword";

const unsigned long CHECK_INTERVAL_MS = 3UL * 60UL * 1000UL;

const int WOL_RETRIES  = 3;
const int WOL_RETRY_MS = 500;

IPAddress INTERNET_CHECK_IP(8, 8, 8, 8);

// ---------------------------------------------------------------------------
// Persistent configuration (stored in NVS via Preferences)
// ---------------------------------------------------------------------------

const int     MAX_SERVERS    = 16;
const uint8_t CONFIG_VERSION = 1;   // bump this if you change the structs below

struct Homelab {
  char    name[24];
  uint8_t ip[4];
  uint8_t mac[6];
  bool    enabled;
};

struct NetConfig {
  bool    useStatic;
  uint8_t ip[4];
  uint8_t gateway[4];
  uint8_t subnet[4];
  uint8_t dns[4];
};

// Used only on first boot (or if the saved config is missing/invalid).
// After that, everything is managed from the /settings page.
const Homelab DEFAULT_SERVERS[] = {
  { "yourHostName",  {192, 168, 1, 10}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, true },
  { "yourHostName2", {192, 168, 1, 11}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, true },
  { "yourHostName3", {192, 168, 1, 12}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, true },
};
const int NUM_DEFAULT_SERVERS = sizeof(DEFAULT_SERVERS) / sizeof(DEFAULT_SERVERS[0]);

const NetConfig DEFAULT_NET = {
  false,                 // DHCP by default
  {192, 168, 1, 200},    // static IP (prefilled in the form)
  {192, 168, 1, 1},      // gateway
  {255, 255, 255, 0},    // subnet
  {192, 168, 1, 1},      // DNS
};

Homelab     servers[MAX_SERVERS];
int         numServers = 0;
NetConfig   netCfg;
Preferences prefs;

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

const int LOG_SIZE = 10;

struct LogEntry {
  char          name[24];
  IPAddress     ip;
  bool          alive;
  bool          wolSent;
  unsigned long timestampMs;
};

LogEntry logBuffer[LOG_SIZE];
int logNext  = 0;
int logCount = 0;

void addLogEntry(const char* name, IPAddress ip, bool alive, bool wolSent) {
  LogEntry& e = logBuffer[logNext];
  strncpy(e.name, name, sizeof(e.name) - 1);
  e.name[sizeof(e.name) - 1] = '\0';
  e.ip = ip;
  e.alive = alive;
  e.wolSent = wolSent;
  e.timestampMs = millis();

  logNext = (logNext + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

WiFiUDP UDP;
WakeOnLan WOL(UDP);
WebServer webServer(80);

unsigned long lastCheck = 0;
bool internetOnline = false;

bool staticFallback = false;          // true if static IP failed and we fell back to DHCP
bool restartPending = false;
unsigned long restartRequestedAt = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

IPAddress toIP(const uint8_t* b) {
  return IPAddress(b[0], b[1], b[2], b[3]);
}

void fromIP(const IPAddress& a, uint8_t* b) {
  for (int i = 0; i < 4; i++) b[i] = a[i];
}

String macToString(const uint8_t* m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

// Accepts AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF (case insensitive)
bool parseMac(String s, uint8_t* out) {
  s.trim();
  s.replace("-", ":");
  unsigned int b[6];
  char tail;
  int n = sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c",
                 &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail);
  if (n != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
  return true;
}

String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;";  break;
      default:   o += c;
    }
  }
  return o;
}

// ---------------------------------------------------------------------------
// Config load / save
// ---------------------------------------------------------------------------

void loadDefaults() {
  numServers = min(NUM_DEFAULT_SERVERS, MAX_SERVERS);
  for (int i = 0; i < numServers; i++) servers[i] = DEFAULT_SERVERS[i];
  netCfg = DEFAULT_NET;
}

void saveServers() {
  prefs.begin("asclepio", false);
  prefs.putUChar("ver", CONFIG_VERSION);
  prefs.putUChar("cnt", (uint8_t)numServers);
  if (numServers > 0) {
    prefs.putBytes("srv", servers, sizeof(Homelab) * numServers);
  } else {
    prefs.remove("srv");
  }
  prefs.end();
}

void saveNetwork() {
  prefs.begin("asclepio", false);
  prefs.putUChar("ver", CONFIG_VERSION);
  prefs.putBytes("net", &netCfg, sizeof(netCfg));
  prefs.end();
}

bool loadConfig() {
  if (!prefs.begin("asclepio", true)) return false;

  bool ok = prefs.getUChar("ver", 0) == CONFIG_VERSION;
  if (ok) {
    int cnt = prefs.getUChar("cnt", 255);
    ok = cnt <= MAX_SERVERS;
    if (ok && cnt > 0) {
      size_t expected = sizeof(Homelab) * cnt;
      ok = prefs.getBytes("srv", servers, expected) == expected;
    }
    if (ok) numServers = cnt;
  }
  if (ok) {
    ok = prefs.getBytes("net", &netCfg, sizeof(netCfg)) == sizeof(netCfg);
  }

  prefs.end();
  return ok;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

void connectWiFi() {
  bool useStatic = netCfg.useStatic && !staticFallback;

  WiFi.mode(WIFI_STA);
  if (useStatic) {
    WiFi.config(toIP(netCfg.ip), toIP(netCfg.gateway), toIP(netCfg.subnet), toIP(netCfg.dns));
  } else {
    WiFi.config(IPAddress(), IPAddress(), IPAddress());   // 0.0.0.0 = DHCP
  }

  Serial.printf("Connecting to WiFi (%s)", useStatic ? "static IP" : "DHCP");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, local IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("WiFi MAC: ");
  Serial.println(WiFi.macAddress());

  // Safety net: if the static config is wrong, fall back to DHCP so the
  // web UI stays reachable and the setting can be fixed.
  if (useStatic && !Ping.ping(toIP(netCfg.gateway), 3)) {
    Serial.println("Gateway unreachable with static IP, falling back to DHCP");
    staticFallback = true;
    WiFi.disconnect(true);
    delay(500);
    connectWiFi();
  }
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

void sendWol(const Homelab& s) {
  Serial.printf("  -> Sending Wake-on-LAN to %s (%d attempts)\n", s.name, WOL_RETRIES);
  for (int i = 0; i < WOL_RETRIES; i++) {
    WOL.sendMagicPacket(const_cast<uint8_t*>(s.mac), 6);
    delay(WOL_RETRY_MS);
  }
}

void checkAllServers() {
  Serial.println("== Checking servers ==");

  internetOnline = Ping.ping(INTERNET_CHECK_IP, 2);
  Serial.printf("Internet: %s\n", internetOnline ? "connected" : "unreachable");

  for (int i = 0; i < numServers; i++) {
    Homelab& s = servers[i];
    IPAddress ip = toIP(s.ip);

    if (!s.enabled) {
      Serial.printf("[PAUSED]  %s (%s) skipped\n", s.name, ip.toString().c_str());
      continue;
    }

    bool alive = Ping.ping(ip, 2);
    bool wolSent = false;

    if (alive) {
      Serial.printf("[OK]      %s (%s) responding\n", s.name, ip.toString().c_str());
    } else {
      Serial.printf("[OFFLINE] %s (%s) not responding\n", s.name, ip.toString().c_str());
      sendWol(s);
      wolSent = true;
    }

    addLogEntry(s.name, ip, alive, wolSent);
  }
  Serial.println("== Check complete ==\n");
}

String formatAgo(unsigned long entryMs) {
  unsigned long agoSec = (millis() - entryMs) / 1000UL;
  if (agoSec < 60) return String(agoSec) + "s ago";
  return String(agoSec / 60) + "m ago";
}

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------

#define ICON_GEAR "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='3'/><path d='M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z'/></svg>"
#define ICON_BACK "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><polyline points='15 18 9 12 15 6'/></svg>"

const char PAGE_CSS[] = R"css(
*{box-sizing:border-box;}
body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;max-width:480px;margin:0 auto;padding:32px 16px;background:#f4f3ef;color:#1a1a1a;}
h1{font-size:18px;font-weight:600;margin:0 0 2px;}
a{color:inherit;}
.card{background:#ffffff;border-radius:14px;padding:16px 18px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,0.06);}
.status-row{display:flex;align-items:center;justify-content:space-between;}
.meta{font-size:12px;color:#8a8a86;margin-top:6px;}
.head-right{display:flex;align-items:center;gap:8px;}
.head-left{display:flex;align-items:center;gap:6px;margin-left:-8px;}
.head-left h1{margin:0;}
.icon-btn{display:inline-flex;align-items:center;justify-content:center;width:32px;height:32px;border-radius:10px;color:#8a8a86;text-decoration:none;}
.icon-btn:hover,.icon-btn:focus-visible{background:#eeede8;color:#1a1a1a;}
.icon-btn svg{width:18px;height:18px;}
.pill{padding:4px 12px;border-radius:999px;font-size:12px;font-weight:600;letter-spacing:.2px;}
.pill-ok{background:#e1f5ee;color:#0a7d3c;} .pill-off{background:#fcebeb;color:#c0392b;}
.section-title{font-size:12px;font-weight:600;color:#8a8a86;text-transform:uppercase;letter-spacing:.5px;margin:0 0 10px 4px;}
.row{display:flex;align-items:center;justify-content:space-between;padding:10px 4px;border-bottom:1px solid #eeede8;}
.row:last-child{border-bottom:none;}
.name{font-weight:500;font-size:14px;}
.time{font-size:12px;color:#8a8a86;margin-top:2px;}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px;}
.dot-ok{background:#0a7d3c;} .dot-off{background:#c0392b;} .dot-warn{background:#c97a12;}
.result{font-size:13px;font-weight:500;display:flex;align-items:center;}
.result-ok{color:#0a7d3c;} .result-off{color:#c0392b;} .result-warn{color:#c97a12;}

.srv{padding:12px 4px;border-bottom:1px solid #eeede8;}
.srv:last-child{border-bottom:none;}
.srv-head{display:flex;align-items:center;justify-content:space-between;gap:12px;}
.srv.paused .name,.srv.paused .time{opacity:.45;}
.tag{font-size:12px;color:#c97a12;margin-top:4px;}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;}
details summary{cursor:pointer;font-size:13px;color:#8a8a86;margin-top:8px;list-style:none;display:inline-block;}
details summary::-webkit-details-marker{display:none;}
details summary:hover,details[open] summary{color:#1a1a1a;}
summary.add{margin-top:0;font-weight:600;color:#1a1a1a;}
form.fields{display:grid;gap:10px;margin-top:10px;}
fieldset{border:none;padding:0;margin:0;display:grid;gap:10px;}
fieldset:disabled{opacity:.45;}
label{display:grid;gap:4px;font-size:12px;color:#8a8a86;}
input[type=text]{font:inherit;font-size:14px;padding:8px 10px;border:1px solid #dcdbd5;border-radius:8px;background:#fff;color:#1a1a1a;width:100%;}
input[type=text]:focus{outline:none;border-color:#0a7d3c;box-shadow:0 0 0 3px rgba(10,125,60,.15);}
.radio-row{display:flex;gap:18px;}
.radio-row label{display:flex;align-items:center;gap:6px;font-size:14px;color:inherit;}
.actions{display:flex;gap:8px;justify-content:flex-end;margin-top:2px;}
.btn{font:inherit;font-size:13px;font-weight:600;padding:8px 14px;border-radius:8px;border:none;cursor:pointer;background:#1a1a1a;color:#fff;}
.btn-danger{background:transparent;color:#c0392b;border:1px solid #f0caca;margin-right:auto;}
.btn:focus-visible,.switch:focus-visible{outline:2px solid #0a7d3c;outline-offset:2px;}
.switch{position:relative;flex-shrink:0;width:40px;height:22px;border:none;border-radius:999px;background:#d4d3ce;cursor:pointer;padding:0;transition:background .15s;}
.switch span{position:absolute;top:3px;left:3px;width:16px;height:16px;border-radius:50%;background:#fff;transition:left .15s;}
.switch.on{background:#0a7d3c;} .switch.on span{left:21px;}
.kv{display:flex;justify-content:space-between;font-size:13px;padding:4px 0;}
.kv span:first-child{color:#8a8a86;}
.flash{font-size:13px;padding:10px 14px;border-radius:10px;margin-bottom:16px;}
.flash-ok{background:#e1f5ee;color:#0a7d3c;} .flash-err{background:#fcebeb;color:#c0392b;}
.card .flash{margin:10px 0 0;}
.hint{font-size:12px;color:#8a8a86;margin:12px 0 0;}
@media (prefers-reduced-motion: reduce){.switch,.switch span{transition:none;}}

@media (prefers-color-scheme: dark){
body{background:#0f0f0f;color:#eaeaea;}
.card{background:#1a1a1a;box-shadow:none;border:1px solid #262626;}
.meta,.section-title,.time,.kv span:first-child,label,.hint,details summary{color:#888;}
details summary:hover,details[open] summary,summary.add{color:#eaeaea;}
.icon-btn{color:#888;} .icon-btn:hover,.icon-btn:focus-visible{background:#262626;color:#eaeaea;}
.pill-ok{background:#0a3d2a;color:#4ade80;} .pill-off{background:#4a1a1a;color:#f87171;}
.row,.srv{border-bottom:1px solid #262626;}
.dot-ok{background:#4ade80;} .dot-off{background:#f87171;} .dot-warn{background:#fbbf24;}
.result-ok{color:#4ade80;} .result-off{color:#f87171;} .result-warn{color:#fbbf24;}
.tag{color:#fbbf24;}
input[type=text]{background:#121212;border-color:#333;color:#eaeaea;}
.btn{background:#eaeaea;color:#0f0f0f;}
.btn-danger{background:transparent;color:#f87171;border-color:#4a1a1a;}
.switch{background:#3a3a3a;} .switch.on{background:#22a55b;}
.flash-ok{background:#0a3d2a;color:#4ade80;} .flash-err{background:#4a1a1a;color:#f87171;}
}
)css";

String pageHead(const char* title, const String& extraHead = "") {
  String h = "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  h += extraHead;
  h += "<title>";
  h += title;
  h += "</title><style>";
  h += PAGE_CSS;
  h += "</style></head><body>";
  return h;
}

String field(const char* label, const char* name, const String& value,
             const char* placeholder, int maxLen) {
  String f = "<label>";
  f += label;
  f += "<input type='text' name='";
  f += name;
  f += "' value='";
  f += value;
  f += "' placeholder='";
  f += placeholder;
  f += "' maxlength='";
  f += maxLen;
  f += "' required autocomplete='off' autocapitalize='off' spellcheck='false'></label>";
  return f;
}

String serverFields(const String& name, const String& ip, const String& mac) {
  return field("Name", "name", name, "proxmox", 23)
       + field("IP address", "ip", ip, "192.168.1.10", 15)
       + field("MAC address", "mac", mac, "AA:BB:CC:DD:EE:FF", 17);
}

const char* flashText(const String& m, bool& isErr) {
  isErr = m.startsWith("err_");
  if (m == "added")    return "Server added.";
  if (m == "saved")    return "Changes saved.";
  if (m == "deleted")  return "Server deleted.";
  if (m == "on")       return "Monitoring resumed.";
  if (m == "off")      return "Monitoring paused. This server won't be pinged or woken up.";
  if (m == "err_name") return "Enter a name (up to 23 characters).";
  if (m == "err_ip")   return "Invalid IP address. Use the format 192.168.1.10.";
  if (m == "err_mac")  return "Invalid MAC address. Use the format AA:BB:CC:DD:EE:FF.";
  if (m == "err_full") return "Server limit reached. Delete one to add another.";
  if (m == "err_id")   return "Server not found. Reload the page and try again.";
  if (m == "err_net")  return "Invalid network settings. Check IP, gateway, subnet mask and DNS.";
  return nullptr;
}

void redirectSettings(const char* msg) {
  webServer.sendHeader("Location", String("/settings?m=") + msg);
  webServer.send(303);
}

int serverIdArg() {
  if (!webServer.hasArg("id")) return -1;
  int id = webServer.arg("id").toInt();
  return (id >= 0 && id < numServers) ? id : -1;
}

// ---- Main page -------------------------------------------------------------

void handleRoot() {
  String html = pageHead("Asclepio", "<meta http-equiv='refresh' content='15'>");

  html += "<div class='card'><div class='status-row'>";
  html += "<div><h1>Asclepio</h1>";
  html += "<div class='meta'>" + WiFi.localIP().toString() + " &middot; up " + String(millis() / 60000UL) + " min</div></div>";
  html += "<div class='head-right'><span class='pill ";
  html += internetOnline ? "pill-ok'>connected" : "pill-off'>unreachable";
  html += "</span><a class='icon-btn' href='/settings' title='Settings' aria-label='Settings'>" ICON_GEAR "</a>";
  html += "</div></div></div>";

  html += "<p class='section-title'>Recent checks</p><div class='card'>";

  if (logCount == 0) {
    html += "<div class='row'><span class='time'>No checks yet</span></div>";
  }

  for (int i = 0; i < logCount; i++) {
    int idx = (logNext - 1 - i + LOG_SIZE) % LOG_SIZE;
    LogEntry& e = logBuffer[idx];
    html += "<div class='row'><div>";
    html += "<div class='name'>" + htmlEscape(e.name) + "</div>";
    html += "<div class='time'>" + formatAgo(e.timestampMs) + "</div>";
    html += "</div><div class='result ";
    if (e.alive) {
      html += "result-ok'><span class='dot dot-ok'></span>online";
    } else if (e.wolSent) {
      html += "result-warn'><span class='dot dot-warn'></span>WoL sent";
    } else {
      html += "result-off'><span class='dot dot-off'></span>offline";
    }
    html += "</div></div>";
  }

  html += "</div></body></html>";
  webServer.send(200, "text/html", html);
}

// ---- Settings page ---------------------------------------------------------

void handleSettings() {
  // No auto-refresh here, it would wipe what you're typing.
  String html = pageHead("Asclepio settings");

  html += "<div class='card'><div class='head-left'>";
  html += "<a class='icon-btn' href='/' title='Back' aria-label='Back'>" ICON_BACK "</a><h1>Settings</h1>";
  html += "</div></div>";

  if (webServer.hasArg("m")) {
    bool isErr;
    const char* t = flashText(webServer.arg("m"), isErr);
    if (t) {
      html += "<div class='flash ";
      html += isErr ? "flash-err'>" : "flash-ok'>";
      html += t;
      html += "</div>";
    }
  }

  // Servers
  html += "<p class='section-title'>Servers (" + String(numServers) + "/" + String(MAX_SERVERS) + ")</p><div class='card'>";

  if (numServers == 0) {
    html += "<div class='row'><span class='time'>No servers yet. Add one below.</span></div>";
  }

  for (int i = 0; i < numServers; i++) {
    Homelab& s = servers[i];
    String id   = String(i);
    String name = htmlEscape(s.name);
    String ip   = toIP(s.ip).toString();
    String mac  = macToString(s.mac);

    html += "<div class='srv";
    if (!s.enabled) html += " paused";
    html += "'><div class='srv-head'><div>";
    html += "<div class='name'>" + name + "</div>";
    html += "<div class='time mono'>" + ip + " &middot; " + mac + "</div>";
    if (!s.enabled) html += "<div class='tag'>Paused</div>";
    html += "</div>";

    html += "<form method='post' action='/server/toggle'><input type='hidden' name='id' value='" + id + "'>";
    html += "<button class='switch";
    if (s.enabled) html += " on";
    html += "' title='";
    html += s.enabled ? "Pause monitoring" : "Resume monitoring";
    html += "' aria-label='";
    html += s.enabled ? "Pause monitoring" : "Resume monitoring";
    html += "'><span></span></button></form></div>";

    html += "<details><summary>Edit</summary>";
    html += "<form class='fields' method='post' action='/server/save'>";
    html += "<input type='hidden' name='id' value='" + id + "'>";
    html += serverFields(name, ip, mac);
    html += "<div class='actions'>";
    html += "<button class='btn btn-danger' formaction='/server/delete' formnovalidate ";
    html += "onclick=\"return confirm('Delete this server?')\">Delete</button>";
    html += "<button class='btn'>Save changes</button>";
    html += "</div></form></details></div>";
  }
  html += "</div>";

  if (numServers < MAX_SERVERS) {
    html += "<div class='card'><details><summary class='add'>+ Add server</summary>";
    html += "<form class='fields' method='post' action='/server/save'>";
    html += "<input type='hidden' name='id' value='new'>";
    html += serverFields("", "", "");
    html += "<div class='actions'><button class='btn'>Add server</button></div>";
    html += "</form></details></div>";
  }

  // Asclepio network
  html += "<p class='section-title'>Asclepio network</p><div class='card'>";
  html += "<div class='kv'><span>Current IP</span><span class='mono'>" + WiFi.localIP().toString();
  html += (netCfg.useStatic && !staticFallback) ? " (static)" : " (DHCP)";
  html += "</span></div>";
  html += "<div class='kv'><span>MAC</span><span class='mono'>" + WiFi.macAddress() + "</span></div>";

  if (staticFallback) {
    html += "<div class='flash flash-err'>The gateway didn't answer with the static IP, so Asclepio is using DHCP until the next restart. Check the settings below.</div>";
  }

  html += "<form class='fields' method='post' action='/network' onsubmit=\"return confirm('Asclepio will restart to apply the new network settings. Continue?')\">";
  html += "<div class='radio-row'>";
  html += "<label><input type='radio' name='mode' value='dhcp' onchange='sm()'";
  if (!netCfg.useStatic) html += " checked";
  html += ">DHCP</label>";
  html += "<label><input type='radio' id='ms' name='mode' value='static' onchange='sm()'";
  if (netCfg.useStatic) html += " checked";
  html += ">Static IP</label></div>";

  html += "<fieldset id='sf'";
  if (!netCfg.useStatic) html += " disabled";
  html += ">";
  html += field("IP address",  "ip",      toIP(netCfg.ip).toString(),      "192.168.1.200", 15);
  html += field("Gateway",     "gateway", toIP(netCfg.gateway).toString(), "192.168.1.1",   15);
  html += field("Subnet mask", "subnet",  toIP(netCfg.subnet).toString(),  "255.255.255.0", 15);
  html += field("DNS",         "dns",     toIP(netCfg.dns).toString(),     "192.168.1.1",   15);
  html += "</fieldset>";
  html += "<div class='actions'><button class='btn'>Save and restart</button></div></form>";
  html += "<p class='hint'>If the static IP doesn't work, Asclepio falls back to DHCP on its own, so this page stays reachable.</p>";
  html += "</div>";

  html += "<script>function sm(){document.getElementById('sf').disabled=!document.getElementById('ms').checked;}</script>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

// ---- Actions ---------------------------------------------------------------

void handleServerSave() {
  bool isNew = webServer.arg("id") == "new";

  String name = webServer.arg("name");
  name.trim();
  IPAddress ip;
  uint8_t mac[6];

  if (name.length() == 0 || name.length() >= sizeof(Homelab::name)) { redirectSettings("err_name"); return; }
  if (!ip.fromString(webServer.arg("ip")))                          { redirectSettings("err_ip");   return; }
  if (!parseMac(webServer.arg("mac"), mac))                          { redirectSettings("err_mac");  return; }

  Homelab* s;
  if (isNew) {
    if (numServers >= MAX_SERVERS) { redirectSettings("err_full"); return; }
    s = &servers[numServers];
    s->enabled = true;
  } else {
    int id = serverIdArg();
    if (id < 0) { redirectSettings("err_id"); return; }
    s = &servers[id];
  }

  strncpy(s->name, name.c_str(), sizeof(s->name) - 1);
  s->name[sizeof(s->name) - 1] = '\0';
  fromIP(ip, s->ip);
  memcpy(s->mac, mac, 6);
  if (isNew) numServers++;

  saveServers();
  Serial.printf("Server %s: %s\n", isNew ? "added" : "updated", s->name);
  redirectSettings(isNew ? "added" : "saved");
}

void handleServerDelete() {
  int id = serverIdArg();
  if (id < 0) { redirectSettings("err_id"); return; }

  Serial.printf("Server deleted: %s\n", servers[id].name);
  for (int i = id; i < numServers - 1; i++) servers[i] = servers[i + 1];
  numServers--;

  saveServers();
  redirectSettings("deleted");
}

void handleServerToggle() {
  int id = serverIdArg();
  if (id < 0) { redirectSettings("err_id"); return; }

  servers[id].enabled = !servers[id].enabled;
  saveServers();
  Serial.printf("Server %s: %s\n", servers[id].name, servers[id].enabled ? "resumed" : "paused");
  redirectSettings(servers[id].enabled ? "on" : "off");
}

void handleNetwork() {
  NetConfig n = netCfg;
  n.useStatic = webServer.arg("mode") == "static";

  if (n.useStatic) {
    IPAddress ip, gw, sn, dns;
    if (!ip.fromString(webServer.arg("ip"))      ||
        !gw.fromString(webServer.arg("gateway")) ||
        !sn.fromString(webServer.arg("subnet"))  ||
        !dns.fromString(webServer.arg("dns"))) {
      redirectSettings("err_net");
      return;
    }
    fromIP(ip,  n.ip);
    fromIP(gw,  n.gateway);
    fromIP(sn,  n.subnet);
    fromIP(dns, n.dns);
  }

  netCfg = n;
  saveNetwork();

  String extra, target;
  if (n.useStatic) {
    target = "http://" + toIP(n.ip).toString() + "/";
    extra  = "<meta http-equiv='refresh' content='10;url=" + target + "'>";
  }

  String html = pageHead("Asclepio restarting", extra);
  html += "<div class='card'><h1>Restarting</h1><div class='meta'>";
  if (n.useStatic) {
    html += "New address: <a href='" + target + "'>" + target + "</a>. You'll be redirected in 10 seconds.";
  } else {
    html += "Asclepio will get its address via DHCP. Look it up in your router's lease list.";
  }
  html += "</div></div></body></html>";
  webServer.send(200, "text/html", html);

  Serial.println("Network settings saved, restarting");
  restartPending = true;
  restartRequestedAt = millis();
}

void setupWebServer() {
  webServer.on("/",              HTTP_GET,  handleRoot);
  webServer.on("/settings",      HTTP_GET,  handleSettings);
  webServer.on("/server/save",   HTTP_POST, handleServerSave);
  webServer.on("/server/delete", HTTP_POST, handleServerDelete);
  webServer.on("/server/toggle", HTTP_POST, handleServerToggle);
  webServer.on("/network",       HTTP_POST, handleNetwork);
  webServer.onNotFound([]() { webServer.send(404, "text/plain", "Not found"); });
  webServer.begin();
  Serial.println("Asclepio web server started on port 80");
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!loadConfig()) {
    Serial.println("No valid saved config, loading defaults");
    loadDefaults();
    saveServers();
    saveNetwork();
  }
  Serial.printf("Loaded %d server(s)\n", numServers);

  connectWiFi();
  setupWebServer();

  checkAllServers();
  lastCheck = millis();
}

void loop() {
  if (restartPending && millis() - restartRequestedAt > 1500) {
    ESP.restart();
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  webServer.handleClient();

  if (millis() - lastCheck >= CHECK_INTERVAL_MS) {
    checkAllServers();
    lastCheck = millis();
  }
}
