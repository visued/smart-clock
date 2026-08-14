// SmartClock AI — firmware custom para JUZIPi SD PRO (ESP8266 ESP-12F/12E)
// Tema: PHOSPHOR CONSOLE (CRT terminal) — relógio 7-seg fósforo, scanlines,
// easter egg de saldo animado por card: SALDO (LED verde pulsando + cursor no
// medidor) / SEM SALDO (vermelho piscando) / SEM CHAVE (âmbar 1Hz).
// Tela: ST7789 240x240 (MOSI=13 SCLK=14 CS=15 DC=2 RST=4 BL=5)
// WiFi: reusa credenciais salvas no SDK. OTA em /update (binário com prefixo SDP).

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <time.h>

#include "logos.h"

// ---------- paleta PHOSPHOR CONSOLE ----------
#define BG            0x0821
#define SCANLINE      0x0020
#define CARD          0x2104
#define CARD_BORDER   0x5AEB
#define DIVIDER       0x3186
#define GRAY          0x94B2
#define BAR_EMPTY     0x4208
#define PHOSPHOR      0xBFED
#define PHOSPHOR_DIM  0x53A4
#define CYAN          0x07FF
#define GREEN         0x07E0
#define GREEN_DIM     0x0460
#define YELLOW        0xFFE0
#define RED           0xF800
#define RED_DIM       0xA000
#define WHITE         0xFFFF
#define BLACK         0x0000
#define DS_BLUE       0x4B5F
#define OC_AMBER      0xFD20

#define MAX_PROVIDERS 6
#define ANIM_MS       250UL      // tick de animação

// ---------- config (persistida em /config.json) ----------
String cfgUsageUrl = "http://192.168.18.7:8787/usage";
int cfgUsageInterval = 300;      // segundos entre consultas
long cfgTzOffset = -10800L;      // Brasil UTC-3

// ---------- estado de uso ----------
struct Limit { String label; float percent = -1.0f; String reset; };
struct Provider {
  String id;
  String name;
  String label;
  String reset;
  String state;                  // saldo | sem_saldo | sem_chave | erro
  float percent = -1.0f;
  bool ok = false;
  Limit limits[3];
  int limitCount = 0;
};
Provider provs[MAX_PROVIDERS];
int provCount = 0;
String lastUsageJson = "{}";
unsigned long lastPollOk = 0;
unsigned long lastPollAttempt = 0;

struct Weather { float temp = 0.0f; String cond; String icon; bool ok = false; };
Weather wth;

String lastFetchErr = "nunca";

int animFrame = 0;
int clockColonX = 120;           // posição dos dois-pontos do relógio

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

struct Badge { const char* id; const char* letters; uint16_t color; };
static const Badge BADGES[] = {
  {"deepseek",     "DS", DS_BLUE},
  {"ollama_cloud", "OC", OC_AMBER},
  {nullptr, nullptr, 0}
};

uint16_t badgeColor(const String& id) {
  for (int i = 0; BADGES[i].id; i++) if (id == BADGES[i].id) return BADGES[i].color;
  return 0x8410;
}
const char* badgeLetters(const String& id) {
  for (int i = 0; BADGES[i].id; i++) if (id == BADGES[i].id) return BADGES[i].letters;
  return "AI";
}
uint16_t usageColor(float pct) {
  if (pct < 50.0f) return GREEN;
  if (pct < 100.0f) return YELLOW;
  return RED;
}

// estado derivado (fallback quando o coletor não manda "state")
String providerState(const Provider& p) {
  if (p.state.length()) return p.state;
  if (!p.ok) return "erro";
  if (p.label.indexOf("sem chave") >= 0) return "sem_chave";
  if (p.percent >= 100.0f) return "sem_saldo";
  return "saldo";
}

// ---------- config persistida ----------
void loadConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, f)) { f.close(); return; }
  cfgUsageUrl = doc["usage_url"] | cfgUsageUrl;
  cfgUsageInterval = doc["usage_interval"] | cfgUsageInterval;
  cfgTzOffset = doc["tz_offset"] | cfgTzOffset;
  f.close();
}
void saveConfig() {
  File f = LittleFS.open("/config.json", "w");
  if (!f) return;
  DynamicJsonDocument doc(512);
  doc["usage_url"] = cfgUsageUrl;
  doc["usage_interval"] = cfgUsageInterval;
  doc["tz_offset"] = cfgTzOffset;
  serializeJson(doc, f);
  f.close();
}

// ---------- coleta de uso ----------
bool fetchUsage() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, cfgUsageUrl)) { lastFetchErr = "begin falhou"; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    lastFetchErr = "http " + String(code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, body)) { lastFetchErr = "json invalido"; return false; }

  JsonArray arr = doc["providers"].as<JsonArray>();
  provCount = 0;
  for (JsonObject p : arr) {
    if (provCount >= MAX_PROVIDERS) break;
    provs[provCount].id = p["id"] | "";
    provs[provCount].name = p["name"] | provs[provCount].id;
    provs[provCount].label = p["label"] | "";
    provs[provCount].reset = p["reset"] | "";
    provs[provCount].state = p["state"] | "";
    provs[provCount].percent = p["percent"] | -1.0f;
    provs[provCount].ok = p["ok"] | false;
    JsonArray lims = p["limits"].as<JsonArray>();
    provs[provCount].limitCount = 0;
    for (JsonObject l : lims) {
      if (provs[provCount].limitCount >= 3) break;
      Limit& lim = provs[provCount].limits[provs[provCount].limitCount];
      lim.label = l["label"] | "";
      lim.percent = l["percent"] | -1.0f;
      lim.reset = l["reset"] | "";
      provs[provCount].limitCount++;
    }
    provCount++;
  }
  JsonObject w = doc["weather"].as<JsonObject>();
  wth.ok = w["ok"] | false;
  if (wth.ok) {
    wth.temp = w["temp"] | 0.0f;
    wth.cond = w["condition"] | "";
    wth.icon = w["icon"] | "";
  }
  lastUsageJson = body;
  lastFetchErr = "ok";
  return true;
}

// ---------- desenho ----------
void fillBg(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, BG);
  for (int yy = y + 3; yy < y + h; yy += 4) {
    tft.drawFastHLine(x, yy, w, SCANLINE);
  }
}

void drawTopRegion() {
  fillBg(0, 0, 240, 24);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);   // FONT2 8x16
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_year >= 120) {
    char b[8];
    snprintf(b, sizeof(b), "%02d/%02d", t->tm_mday, t->tm_mon + 1);  // dd/mm (cabe ao lado do IP)
    tft.setTextColor(WHITE, BG);
    tft.drawString(b, 6, 3);
  } else {
    tft.setTextColor(GRAY, BG);
    tft.drawString("NTP...", 6, 3);
  }
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(GRAY, BG);
  if (WiFi.getMode() & WIFI_AP) {
    tft.drawString("AP", 234, 3);
  } else {
    tft.drawString(WiFi.localIP().toString(), 234, 3);
  }
  tft.drawFastHLine(0, 23, 240, DIVIDER);
  tft.drawFastHLine(6, 23, 44, PHOSPHOR);
}

void drawWeatherIcon(const String& code, int x, int y) {
  int c = code.length() >= 2 ? atoi(code.substring(0, 2).c_str()) : 0;
  if (c == 1) {  // sol
    tft.fillCircle(x + 10, y + 9, 6, YELLOW);
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4;
      tft.drawLine(x + 10, y + 9, x + 10 + (int)(9 * cos(a)), y + 9 + (int)(9 * sin(a)), YELLOW);
    }
  } else {
    tft.fillCircle(x + 6, y + 10, 5, GRAY);
    tft.fillCircle(x + 13, y + 8, 6, GRAY);
    tft.fillCircle(x + 19, y + 11, 4, GRAY);
    tft.fillRect(x + 5, y + 10, 16, 5, GRAY);
    if (c == 9 || c == 10) {  // chuva
      tft.drawLine(x + 8, y + 16, x + 6, y + 19, CYAN);
      tft.drawLine(x + 13, y + 16, x + 11, y + 19, CYAN);
      tft.drawLine(x + 18, y + 16, x + 16, y + 19, CYAN);
    } else if (c == 11) {  // tempestade
      tft.fillTriangle(x + 9, y + 16, x + 15, y + 16, x + 12, y + 20, YELLOW);
      tft.fillTriangle(x + 14, y + 16, x + 19, y + 16, x + 17, y + 20, YELLOW);
    } else if (c == 13) {  // neve
      tft.fillCircle(x + 8, y + 17, 1, WHITE);
      tft.fillCircle(x + 13, y + 17, 1, WHITE);
      tft.fillCircle(x + 18, y + 17, 1, WHITE);
    }
  }
}

void drawWeatherRegion() {
  fillBg(0, 24, 240, 22);
  if (!wth.ok) return;
  char b[40];
  snprintf(b, sizeof(b), "%.0fC %s", wth.temp, wth.cond.c_str());
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(PHOSPHOR, BG);
  tft.drawString(">", 6, 27);
  tft.setTextColor(WHITE, BG);
  tft.drawString(b, 16, 27);
  drawWeatherIcon(wth.icon, 214, 24);
}

// dois-pontos do relógio 7-seg (piscam 500/500ms)
void drawColon(int x, int y, bool on) {
  tft.fillRect(x - 4, y, 8, 46, BG);
  tft.drawFastHLine(x - 4, y + 3, 8, SCANLINE);
  tft.drawFastHLine(x - 4, y + 43, 8, SCANLINE);
  if (on) {
    tft.fillCircle(x, y + 11, 3, PHOSPHOR);
    tft.fillCircle(x, y + 35, 3, PHOSPHOR);
  }
}

// dígitos do relógio 7-seg (redesenhados só quando o minuto muda)
void drawClockDigits() {
  fillBg(0, 46, 240, 52);
  tft.setTextDatum(TC_DATUM);
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_year < 120) {
    tft.setTextSize(4);
    tft.setTextColor(GRAY, BG);
    tft.drawString("--:--", 120, 60);
    clockColonX = 120;
    return;
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d%02d", t->tm_hour, t->tm_min);  // sem dois-pontos
  tft.setTextSize(7);              // FONT7 48px 7-segmento
  tft.setTextColor(PHOSPHOR_DIM, BG);   // sombra +1,+1
  tft.drawString(buf, 120, 47);
  tft.setTextColor(PHOSPHOR, BG);
  tft.drawString(buf, 120, 46);
  clockColonX = 120;
  drawColon(clockColonX, 52, animFrame % 2 == 0);
}

// segundos (região pequena, redraw por segundo)
void drawSeconds(int s) {
  fillBg(204, 62, 36, 24);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);              // FONT2 8x16
  tft.setTextColor(CYAN, BG);
  char buf[4];
  snprintf(buf, sizeof(buf), "%02d", s);
  tft.drawString(buf, 210, 66);
}

void drawStatusRegion() {
  fillBg(0, 96, 240, 26);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char b[40];
  uint16_t led;
  if (t->tm_year < 120) {
    led = GRAY;
    snprintf(b, sizeof(b), "NTP sincronizando");
  } else if (lastPollOk != 0 && millis() - lastPollOk < 60000UL) {
    led = GREEN;
    // hora da última atualização + intervalo (cabe em 240px com FONT2 12px)
    snprintf(b, sizeof(b), "%02d:%02d:%02d (%ds)", t->tm_hour, t->tm_min, t->tm_sec, cfgUsageInterval);
  } else if (lastPollOk != 0) {
    led = YELLOW;
    snprintf(b, sizeof(b), "desatualizado (%ds)", cfgUsageInterval);
  } else {
    led = RED;
    snprintf(b, sizeof(b), "sem servico");
  }
  tft.fillCircle(11, 109, 3, led);
  tft.setTextColor(led, BG);
  tft.drawString(b, 20, 102);
  tft.drawFastHLine(0, 122, 240, DIVIDER);
}

// metadados por estado: (palavra, cor, cor dim, passos on, passos off a 250ms)
struct StateMeta { const char* word; uint16_t color; uint16_t dim; int on; int off; };
static const StateMeta STATES[] = {
  {"SALDO",     GREEN,  GREEN_DIM, 3, 1},
  {"ESGOTADO",  RED,    RED_DIM,   1, 1},
  {"SEM CHAVE", YELLOW, YELLOW,    2, 2},
  {"ERRO",      YELLOW, YELLOW,    1, 1},
};
const StateMeta* stateMeta(const String& st) {
  if (st == "saldo") return &STATES[0];
  if (st == "sem_saldo") return &STATES[1];
  if (st == "sem_chave") return &STATES[2];
  return &STATES[3];
}

// posição vertical do card i (centraliza 1..3 cards na região 124..240)
int cardY(int i) {
  int n = provCount > 3 ? 3 : provCount;
  int gap = n >= 3 ? 1 : 4;
  int total = n * 38 + (n - 1) * gap;
  int start = 124 + (116 - total) / 2;
  if (start < 124) start = 124;
  return start + i * (38 + gap);
}

// parte estática do card (desenhada 1x; LED é redraw pequeno por tick)
void drawCardStatic(int i) {
  int y = cardY(i);
  String st = providerState(provs[i]);
  const StateMeta* m = stateMeta(st);
  float pct = provs[i].percent;
  uint16_t border = (st == "sem_saldo") ? RED : CARD_BORDER;

  tft.fillRoundRect(8, y, 224, 38, 6, CARD);
  tft.drawRoundRect(8, y, 224, 38, 6, border);
  tft.fillRect(8, y + 9, 3, 24, m->color);   // barra de acento

  // logo sobre badge com a cor da marca (contraste alto)
  const uint16_t* logo = logoFor(provs[i].id);
  if (logo) {
    tft.fillRoundRect(12, y + 1, LOGO_SIZE, LOGO_SIZE, 8, badgeFor(provs[i].id));
    tft.setSwapBytes(true);  // painel BGR: pushImage precisa dos bytes trocados
    tft.pushImage(12, y + 1, LOGO_SIZE, LOGO_SIZE, (uint16_t*)logo);
  } else {
    uint16_t bcol = badgeColor(provs[i].id);
    tft.fillRoundRect(12, y + 4, 28, 28, 7, bcol);
    tft.setTextDatum(CC_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(BLACK, bcol);
    tft.drawString(badgeLetters(provs[i].id), 26, y + 18);
  }

  // linhas de informação (FONT2 8x16) — limites primeiro, senão estado/saldo
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  String r1, r2;
  uint16_t c1 = GRAY, c2 = GRAY;
  if (provs[i].limitCount > 0) {
    // linha 1: primeiro limite; linha 2: segundo
    for (int k = 0; k < 2; k++) {
      String& r = (k == 0) ? r1 : r2;
      uint16_t& c = (k == 0) ? c1 : c2;
      if (k < provs[i].limitCount) {
        Limit& lim = provs[i].limits[k];
        String lb = lim.label;
        if (lb.length() > 2) lb = lb.substring(0, 2);
        r = lb;
        if (lim.percent >= 0.0f) {
          char b[8];
          snprintf(b, sizeof(b), " %.0f%%", lim.percent);
          r += b;
        }
        String rs = lim.reset;
        if (rs.length() > 8) rs = rs.substring(0, 8);
        if (rs.length()) { r += " "; r += rs; }
        c = lim.percent >= 0.0f ? usageColor(lim.percent) : GRAY;
      } else {
        r = "-";
      }
    }
  } else {
    bool isKey = (st == "sem_chave" || st == "erro");
    if (isKey) {
      r1 = m->word;
      c1 = m->color;
      r2 = provs[i].label;
    } else {
      r1 = provs[i].label.length() ? provs[i].label : m->word;
      c1 = (pct >= 0.0f) ? usageColor(pct) : m->color;
      if (provs[i].reset.length()) {
        r2 = provs[i].reset;
      } else if (pct >= 0.0f) {
        char b[12];
        snprintf(b, sizeof(b), "usado %.0f%%", pct);
        r2 = b;
      }
    }
  }
  int mx = 13;   // "5h 100% 14:58" = 13 chars — 156px até x=208, limpa o LED (x>=219)
  if (r1.length() > mx) r1 = r1.substring(0, mx);
  if (r2.length() > mx) r2 = r2.substring(0, mx);
  tft.setTextColor(c1, CARD);
  tft.drawString(r1, 52, y + 3);
  tft.setTextColor(c2, CARD);
  tft.drawString(r2, 52, y + 21);


  tft.drawCircle(225, y + 24, 6, 0x2945);   // anel do LED (estático)
}

// LED de estado (animado por tick — redraw pequeno, dentro do card)
void drawCardLed(int i, int frame) {
  int y = cardY(i);
  String st = providerState(provs[i]);
  const StateMeta* m = stateMeta(st);
  int period = m->on + m->off;
  bool on = (frame % period) < m->on;
  tft.fillRect(219, y + 18, 13, 13, CARD);
  tft.drawCircle(225, y + 24, 6, 0x2945);
  tft.fillCircle(225, y + 24, 3, on ? m->color : m->dim);
}

void drawWidgetRegion(int frame) {
  fillBg(0, 124, 240, 116);
  if (provCount == 0) {
    tft.setTextDatum(CC_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(GRAY, BG);
    tft.drawString("Sem provedores", 120, 180);
    return;
  }
  int n = provCount > 3 ? 3 : provCount;
  for (int i = 0; i < n; i++) {
    drawCardStatic(i);
    drawCardLed(i, frame);
  }
}

// ---------- web ----------
void handleRoot() {
  String html = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>SmartClock AI</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{background:#111;color:#eee;font-family:sans-serif;padding:16px}"
    "label{display:block;margin-top:12px}input{width:100%;padding:8px;box-sizing:border-box;"
    "background:#222;color:#eee;border:1px solid #555;border-radius:4px}"
    "button{margin-top:14px;padding:10px;background:#34c759;color:#000;border:none;"
    "border-radius:6px;width:100%;font-size:15px}"
    "h2{margin-top:26px;font-size:18px}a{color:#4d6bfe}</style></head><body>"
    "<h1>SmartClock AI</h1>"
    "<label>URL do coletor (/usage)</label><input id='usage_url'>"
    "<label>Intervalo (segundos)</label><input id='usage_interval' type='number'>"
    "<label>Fuso horario (segundos, ex. -10800)</label><input id='tz_offset' type='number'>"
    "<button onclick='save()'>Salvar</button>"
    "<button onclick=\"location='/restart'\" style='background:#ff3b30;color:#fff'>Reiniciar</button>"
    "<h2>OTA</h2><a href='/update'>Pagina de update de firmware</a>"
    "<p><a href='/usage'>ultimo JSON recebido</a> · <a href='/config'>config</a></p>"
    "<script>fetch('/config').then(r=>r.json()).then(c=>{"
    "document.getElementById('usage_url').value=c.usage_url;"
    "document.getElementById('usage_interval').value=c.usage_interval;"
    "document.getElementById('tz_offset').value=c.tz_offset;});"
    "function save(){const s=(k,v)=>fetch('/api/set?key='+k+'&value='+encodeURIComponent(v));"
    "s('usage_url',document.getElementById('usage_url').value).then(()=>"
    "s('usage_interval',document.getElementById('usage_interval').value)).then(()=>"
    "s('tz_offset',document.getElementById('tz_offset').value)).then(()=>alert('Salvo'));}"
    "</script>");
  String collectorBase = cfgUsageUrl;
  if (collectorBase.endsWith("/usage")) collectorBase = collectorBase.substring(0, collectorBase.length() - 6);
  html += "<h2>Provedores</h2><p><a href='" + collectorBase +
          "'>Configurar provedores (ativar/desativar, chaves) no coletor</a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleConfig() {
  tft.setTextSize(2);   // FONT2 — métricas reais do topo
  int dateW = tft.textWidth("07/08/2026");
  int ipW = tft.textWidth("192.168.18.67");
  String json = "{\"usage_url\":\"" + cfgUsageUrl + "\",\"usage_interval\":" + String(cfgUsageInterval) +
                ",\"tz_offset\":" + String(cfgTzOffset) +
                ",\"ssid\":\"" + WiFi.SSID() + "\",\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"ap\":" + ((WiFi.getMode() & WIFI_AP) ? "true" : "false") +
                ",\"providers\":" + provCount +
                ",\"screen\":\"" + String(tft.width()) + "x" + String(tft.height()) +
                "\",\"date_w\":" + String(dateW) + ",\"ip_w\":" + String(ipW) +
                ",\"last_err\":\"" + lastFetchErr + "\"}";
  server.send(200, "application/json", json);
}

void handleApiSet() {
  String key = server.arg("key");
  String value = server.arg("value");
  bool poll = false;
  if (key == "usage_url") { cfgUsageUrl = value; poll = true; }
  else if (key == "usage_interval") { cfgUsageInterval = value.toInt(); poll = true; }
  else if (key == "tz_offset") { cfgTzOffset = atol(value.c_str()); configTime(cfgTzOffset, 0, "pool.ntp.org"); }
  else { server.send(400, "text/plain", "unknown key"); return; }
  saveConfig();
  server.send(200, "text/plain", "ok");
  if (poll) lastPollAttempt = 0;  // força consulta imediata
}

void handleUsage() {
  server.send(200, "application/json", lastUsageJson);
}

void setup() {
  Serial.begin(115200);
  LittleFS.begin();
  loadConfig();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(BG);

  tft.setTextDatum(CC_DATUM);
  tft.setTextSize(4);
  tft.setTextColor(PHOSPHOR, BG);
  tft.drawString("SmartClock", 120, 90);
  tft.setTextSize(2);
  tft.setTextColor(WHITE, BG);
  tft.drawString("AI monitor", 120, 130);
  tft.setTextColor(GRAY, BG);
  tft.drawString("WiFi...", 120, 160);

  WiFi.mode(WIFI_STA);
  WiFi.begin();  // reusa credenciais salvas no SDK (mesmo truque do firmware original)
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) { delay(500); tries++; }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SDPRO-RECOVERY", "12345678");
  }
  configTime(cfgTzOffset, 0, "pool.ntp.org");

  httpUpdater.setup(&server, "/update");
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/api/set", handleApiSet);
  server.on("/usage", handleUsage);
  server.on("/restart", []() {
    server.send(200, "text/plain", "ok");
    ESP.restart();
  });
  server.begin();

  tft.fillScreen(BG);
  lastPollAttempt = 0;  // primeira consulta imediata
  if (cfgUsageInterval <= 0) cfgUsageInterval = 300;
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  static unsigned long lastSec = 0;
  static int lastMin = -1;
  if (now - lastSec >= 1000UL) {
    lastSec = now;
    time_t tnow = time(nullptr);
    struct tm* t = localtime(&tnow);
    if (t->tm_year < 120) {
      if (lastMin != -1) { lastMin = -1; drawClockDigits(); drawTopRegion(); }
    } else {
      int cur = t->tm_hour * 60 + t->tm_min;
      if (cur != lastMin) {
        lastMin = cur;
        drawClockDigits();
        drawTopRegion();   // data/IP — muda por minuto/dia
      }
      drawSeconds(t->tm_sec);
    }
  }

  // tick de animação: LED dos cards, dois-pontos do relógio
  static unsigned long lastAnim = 0;
  if (now - lastAnim >= ANIM_MS) {
    lastAnim = now;
    animFrame++;
    int n = provCount > 3 ? 3 : provCount;
    for (int i = 0; i < n; i++) {
      drawCardLed(i, animFrame);
    }
    drawColon(clockColonX, 52, animFrame % 2 == 0);
  }

  if (lastPollAttempt == 0 || now - lastPollAttempt >= (unsigned long)cfgUsageInterval * 1000UL) {
    lastPollAttempt = now;
    if (fetchUsage()) {
      lastPollOk = now;
      drawWidgetRegion(animFrame);
    }
    drawStatusRegion();
    drawWeatherRegion();
  }
}
