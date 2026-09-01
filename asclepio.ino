#include <WiFi.h>
#include <ESPping.h>
#include <WiFiUdp.h>
#include <WakeOnLan.h>
#include <WebServer.h>

const char* WIFI_SSID     = "yourSSID";
const char* WIFI_PASSWORD = "yourPassword";

// IPAddress staticIP(192, 168, 1, 200);
// IPAddress gateway(192, 168, 1, 1);
// IPAddress subnet(255, 255, 255, 0);
// IPAddress dns(192, 168, 1, 1);

const unsigned long CHECK_INTERVAL_MS = 3UL * 60UL * 1000UL;

const int WOL_RETRIES  = 3;
const int WOL_RETRY_MS = 500;

IPAddress INTERNET_CHECK_IP(8, 8, 8, 8);

struct Homelab {
  const char* name;
  IPAddress   ip;
  byte        mac[6];
};

Homelab servers[] = {
  { "yourHostName",  IPAddress(192, 168, 1, xxx), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },
  { "yourHostName2",  IPAddress(192, 168, 1, xxx), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },
  { "yourHostName3",  IPAddress(192, 168, 1, xxx), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} },
};
const int NUM_SERVERS = sizeof(servers) / sizeof(servers[0]);

const int LOG_SIZE = 10;

struct LogEntry {
  char        name[24];
  IPAddress   ip;
  bool        alive;
  bool        wolSent;
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

WiFiUDP UDP;
WakeOnLan WOL(UDP);
WebServer webServer(80);

unsigned long lastCheck = 0;
bool internetOnline = false;

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  // WiFi.config(staticIP, gateway, subnet, dns);
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
}

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

  for (int i = 0; i < NUM_SERVERS; i++) {
    Homelab& s = servers[i];
    bool alive = Ping.ping(s.ip, 2);
    bool wolSent = false;

    if (alive) {
      Serial.printf("[OK]      %s (%s) responding\n", s.name, s.ip.toString().c_str());
    } else {
      Serial.printf("[OFFLINE] %s (%s) not responding\n", s.name, s.ip.toString().c_str());
      sendWol(s);
      wolSent = true;
    }

    addLogEntry(s.name, s.ip, alive, wolSent);
  }
  Serial.println("== Check complete ==\n");
}

String formatAgo(unsigned long entryMs) {
  unsigned long agoSec = (millis() - entryMs) / 1000UL;
  if (agoSec < 60) return String(agoSec) + "s ago";
  return String(agoSec / 60) + "m ago";
}


void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Asclepio</title>";
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;max-width:480px;margin:0 auto;padding:32px 16px;background:#f4f3ef;color:#1a1a1a;}";
  html += "h1{font-size:18px;font-weight:600;margin:0 0 2px;}";
  html += ".card{background:#ffffff;border-radius:14px;padding:16px 18px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,0.06);}";
  html += ".status-row{display:flex;align-items:center;justify-content:space-between;}";
  html += ".status-row .meta{font-size:12px;color:#8a8a86;margin-top:6px;}";
  html += ".pill{padding:4px 12px;border-radius:999px;font-size:12px;font-weight:600;letter-spacing:.2px;}";
  html += ".pill-ok{background:#e1f5ee;color:#0a7d3c;} .pill-off{background:#fcebeb;color:#c0392b;}";
  html += ".section-title{font-size:12px;font-weight:600;color:#8a8a86;text-transform:uppercase;letter-spacing:.5px;margin:0 0 10px 4px;}";
  html += ".row{display:flex;align-items:center;justify-content:space-between;padding:10px 4px;border-bottom:1px solid #eeede8;}";
  html += ".row:last-child{border-bottom:none;}";
  html += ".row .name{font-weight:500;font-size:14px;}";
  html += ".row .time{font-size:12px;color:#8a8a86;margin-top:2px;}";
  html += ".dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px;}";
  html += ".dot-ok{background:#0a7d3c;} .dot-off{background:#c0392b;} .dot-warn{background:#c97a12;}";
  html += ".result{font-size:13px;font-weight:500;display:flex;align-items:center;}";
  html += ".result-ok{color:#0a7d3c;} .result-off{color:#c0392b;} .result-warn{color:#c97a12;}";
  html += "@media (prefers-color-scheme: dark){";
  html += "body{background:#0f0f0f;color:#eaeaea;}";
  html += ".card{background:#1a1a1a;box-shadow:none;border:1px solid #262626;}";
  html += ".status-row .meta{color:#888;}";
  html += ".pill-ok{background:#0a3d2a;color:#4ade80;} .pill-off{background:#4a1a1a;color:#f87171;}";
  html += ".section-title{color:#888;}";
  html += ".row{border-bottom:1px solid #262626;}";
  html += ".row .time{color:#888;}";
  html += ".dot-ok{background:#4ade80;} .dot-off{background:#f87171;} .dot-warn{background:#fbbf24;}";
  html += ".result-ok{color:#4ade80;} .result-off{color:#f87171;} .result-warn{color:#fbbf24;}";
  html += "}";
  html += "</style></head><body>";

  html += "<div class='card'><div class='status-row'>";
  html += "<div><h1>Asclepio</h1>";
  html += "<div class='meta'>" + WiFi.localIP().toString() + " &middot; up " + String(millis() / 60000UL) + " min</div></div>";
  html += "<span class='pill ";
  html += internetOnline ? "pill-ok'>connected" : "pill-off'>unreachable";
  html += "</span></div></div>";

  html += "<p class='section-title'>Recent checks</p><div class='card'>";

  if (logCount == 0) {
    html += "<div class='row'><span class='time'>No checks yet</span></div>";
  }

  for (int i = 0; i < logCount; i++) {
    int idx = (logNext - 1 - i + LOG_SIZE) % LOG_SIZE;
    LogEntry& e = logBuffer[idx];
    html += "<div class='row'><div>";
    html += "<div class='name'>" + String(e.name) + "</div>";
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

void setupWebServer() {
  webServer.on("/", handleRoot);
  webServer.begin();
  Serial.println("Asclepio web server started on port 80");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectWiFi();
  setupWebServer();

  checkAllServers();
  lastCheck = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  webServer.handleClient();

  if (millis() - lastCheck >= CHECK_INTERVAL_MS) {
    checkAllServers();
    lastCheck = millis();
  }
}
