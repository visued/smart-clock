// SmartClock AI — firmware custom para JUZIPi SD PRO (ESP8266 ESP-12F/12E)
// Tema: PHOSPHOR CONSOLE (CRT terminal) — relógio 7-seg fósforo, scanlines,
// easter egg de saldo animado por card: SALDO (LED verde pulsando + cursor no
// medidor) / SEM SALDO (vermelho piscando) / SEM CHAVE (âmbar 1Hz).
// Carrossel: tela 1 = relógio + IA (fósforo verde); tela 2 = inversor solar
// (fósforo âmbar, dados de GET <coletor>/solar). Troca a cada N s (config "carousel").
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
#define AMBER         0xFD80     // tela solar: fósforo âmbar (CRT P3)
#define AMBER_DIM     0x8280
#define AMBER_DARK    0x28C0

#define MAX_PROVIDERS 6
#define MAX_CARDS     4          // cards visíveis na tela (4 x 36px abaixo do relógio)
#define CARD_H        36
#define CARDS_Y       93         // topo da região de cards (93..239)
#define ANIM_MS       250UL      // tick de animação

// ---------- config (persistida em /config.json) ----------
String cfgUsageUrl = "http://192.168.18.7:8787/usage";
int cfgUsageInterval = 300;      // segundos entre consultas
long cfgTzOffset = -10800L;      // Brasil UTC-3
int cfgCarousel = 15;            // segundos por tela do carrossel (0 = só a tela de IA)

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
time_t lastPollEpoch = 0;        // hora da última consulta ok (a linha de status é redesenhada ao voltar do carrossel)

struct Weather { float temp = 0.0f; String cond; String icon; bool ok = false; };
Weather wth;

String lastFetchErr = "nunca";

// ---------- estado solar (GET <coletor>/solar) ----------
#define SOLAR_SLOTS   42         // curva do dia: 05h..19h em fatias de 20 min (= coletor)
#define SOLAR_POLL_MS 30000UL
struct Solar {
  bool ok = false;               // coletor tem ao menos uma leitura do inversor
  bool online = false;           // leitura recente (inversor ligado)
  int w = 0, pct = 0, rated = 5000, peakW = 0, age = -1;
  float today = -1.0f, total = -1.0f, vac = 0.0f, hz = 0.0f, temp = 0.0f;
  float pv[2][3] = {{0}};        // [tensão V, corrente A, potência W] por MPPT
  int pvCount = 0;
  String peakT;
  int slot0 = 300, slotMin = 20;
  int8_t hist[SOLAR_SLOTS];      // % da nominal por fatia; -1 = sem dado
};
Solar sol;
String solarErr = "nunca";
unsigned long lastSolarPoll = 0;

int page = 0;                    // 0 = IA, 1 = solar
unsigned long pageSince = 0;

int animFrame = 0;
bool clockSynced = false;        // dígitos desenhados (NTP ok) — só então os dois-pontos piscam

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

struct Badge { const char* id; const char* letters; uint16_t color; };
static const Badge BADGES[] = {
  {"deepseek",     "DS", DS_BLUE},
  {"ollama_cloud", "OC", OC_AMBER},
  {"claude",       "CL", 0xDBAA},
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
  cfgCarousel = doc["carousel"] | cfgCarousel;
  f.close();
}
void saveConfig() {
  File f = LittleFS.open("/config.json", "w");
  if (!f) return;
  DynamicJsonDocument doc(512);
  doc["usage_url"] = cfgUsageUrl;
  doc["usage_interval"] = cfgUsageInterval;
  doc["tz_offset"] = cfgTzOffset;
  doc["carousel"] = cfgCarousel;
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

// base do coletor (http://pc:8787) derivada da URL do /usage
String collectorBase() {
  String b = cfgUsageUrl;
  if (b.endsWith("/usage")) b = b.substring(0, b.length() - 6);
  return b;
}

// ---------- coleta solar (inversor lido pelo coletor via Modbus TCP) ----------
bool fetchSolar() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, collectorBase() + "/solar")) { solarErr = "begin falhou"; sol.ok = false; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    solarErr = "http " + String(code);
    http.end();
    sol.ok = false;              // sem coletor: carrossel fica só na tela de IA
    return false;
  }
  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, body)) { solarErr = "json invalido"; sol.ok = false; return false; }
  sol.ok = doc["ok"] | false;
  sol.online = doc["online"] | false;
  sol.age = doc["age"] | -1;
  sol.w = doc["w"] | 0;
  sol.pct = doc["pct"] | 0;
  sol.rated = doc["rated"] | 5000;
  sol.today = doc["today"] | -1.0f;
  sol.total = doc["total"] | -1.0f;
  sol.vac = doc["vac"] | 0.0f;
  sol.hz = doc["hz"] | 0.0f;
  sol.temp = doc["temp"] | 0.0f;
  sol.peakW = doc["peak_w"] | 0;
  sol.peakT = doc["peak_t"] | "";
  sol.slot0 = doc["slot0"] | 300;
  sol.slotMin = doc["slot_min"] | 20;
  if (sol.slotMin <= 0) sol.slotMin = 20;
  sol.pvCount = 0;
  for (JsonVariant p : doc["pv"].as<JsonArray>()) {
    if (sol.pvCount >= 2) break;
    for (int k = 0; k < 3; k++) sol.pv[sol.pvCount][k] = p[k] | 0.0f;
    sol.pvCount++;
  }
  int i = 0;
  for (JsonVariant v : doc["hist"].as<JsonArray>()) {
    if (i >= SOLAR_SLOTS) break;
    sol.hist[i++] = constrain(v.as<int>(), -1, 100);
  }
  for (; i < SOLAR_SLOTS; i++) sol.hist[i] = -1;
  solarErr = sol.ok ? String("ok") : String((const char*)(doc["err"] | "sem leitura"));
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
  fillBg(0, 0, 240, 22);
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
  tft.drawFastHLine(0, 21, 240, DIVIDER);
  tft.drawFastHLine(6, 21, 44, PHOSPHOR);
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
  fillBg(0, 22, 240, 20);
  if (!wth.ok) return;
  char b[40];
  snprintf(b, sizeof(b), "%.0fC %s", wth.temp, wth.cond.c_str());
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(PHOSPHOR, BG);
  tft.drawString(">", 6, 24);
  tft.setTextColor(WHITE, BG);
  tft.drawString(b, 16, 24);
  drawWeatherIcon(wth.icon, 214, 22);
}

// relógio 7-seg compacto (28px) desenhado com retângulos — região 42..73,
// libera espaço para 4 cards. Geometria espelhada em scripts/preview.py.
#define CLOCK_Y   44
#define SEG_H     28
#define SEG_W     18
#define SEG_T     3
#define SEG_INS   2
#define SEG_P     22            // passo entre dígitos (largura + 4px)
#define SEG_C     6             // espaço extra para os dois-pontos
#define CLOCK_X0  (120 - (3 * SEG_P + SEG_C + SEG_W) / 2)
#define COLON_X   (CLOCK_X0 + SEG_P + SEG_W + (SEG_P - SEG_W + SEG_C) / 2)

// bits: a b c d e f g
static const uint8_t SEG_MASK[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void draw7seg(int x, int y, int digit, uint16_t color) {
  uint8_t m = SEG_MASK[digit % 10];
  int half = SEG_H / 2;
  int span = SEG_W - 2 * SEG_INS;
  if (m & 0x01) tft.fillRect(x + SEG_INS, y, span, SEG_T, color);                          // a
  if (m & 0x02) tft.fillRect(x + SEG_W - SEG_T, y + SEG_INS, SEG_T, half - SEG_INS, color); // b
  if (m & 0x04) tft.fillRect(x + SEG_W - SEG_T, y + half, SEG_T, half - SEG_INS, color);    // c
  if (m & 0x08) tft.fillRect(x + SEG_INS, y + SEG_H - SEG_T, span, SEG_T, color);           // d
  if (m & 0x10) tft.fillRect(x, y + half, SEG_T, half - SEG_INS, color);                    // e
  if (m & 0x20) tft.fillRect(x, y + SEG_INS, SEG_T, half - SEG_INS, color);                 // f
  if (m & 0x40) tft.fillRect(x + SEG_INS, y + half - SEG_T / 2, span, SEG_T, color);        // g
}

// dois-pontos do relógio (piscam 500/500ms); fillBg na fase da região 42
void drawColon(bool on) {
  if (!clockSynced) return;
  fillBg(COLON_X - 3, 42, 7, 32);
  if (on) {
    tft.fillCircle(COLON_X, CLOCK_Y + 9, 2, PHOSPHOR);
    tft.fillCircle(COLON_X, CLOCK_Y + 19, 2, PHOSPHOR);
  }
}

// dígitos do relógio (redesenhados só quando o minuto muda)
void drawClockDigits() {
  fillBg(0, 42, 240, 32);
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_year < 120) {
    clockSynced = false;
    tft.setTextDatum(TC_DATUM);
    tft.setTextSize(3);
    tft.setTextColor(GRAY, BG);
    tft.drawString("--:--", 120, 47);
    return;
  }
  clockSynced = true;
  int dg[4] = {t->tm_hour / 10, t->tm_hour % 10, t->tm_min / 10, t->tm_min % 10};
  for (int i = 0; i < 4; i++) {
    int x = CLOCK_X0 + i * SEG_P + (i >= 2 ? SEG_C : 0);
    draw7seg(x + 1, CLOCK_Y + 1, dg[i], PHOSPHOR_DIM);  // sombra +1,+1
    draw7seg(x, CLOCK_Y, dg[i], PHOSPHOR);
  }
  drawColon(animFrame % 2 == 0);
}

// segundos (região pequena, redraw por segundo)
void drawSeconds(int s) {
  fillBg(204, 42, 36, 32);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);              // FONT2 8x16
  tft.setTextColor(CYAN, BG);
  char buf[4];
  snprintf(buf, sizeof(buf), "%02d", s);
  tft.drawString(buf, 210, 50);
}

void drawStatusRegion() {
  fillBg(0, 74, 240, 18);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char b[40];
  uint16_t led;
  if (t->tm_year < 120) {
    led = GRAY;
    snprintf(b, sizeof(b), "NTP sincronizando");
  } else if (lastPollOk != 0 && lastPollOk == lastPollAttempt) {
    led = GREEN;
    // hora da última atualização + intervalo (cabe em 240px com FONT2 12px)
    struct tm* p = localtime(&lastPollEpoch);
    snprintf(b, sizeof(b), "%02d:%02d:%02d (%ds)", p->tm_hour, p->tm_min, p->tm_sec, cfgUsageInterval);
  } else if (lastPollOk != 0) {
    led = YELLOW;
    snprintf(b, sizeof(b), "desatualizado (%ds)", cfgUsageInterval);
  } else {
    led = RED;
    snprintf(b, sizeof(b), "sem servico");
  }
  tft.fillCircle(11, 83, 3, led);
  tft.setTextColor(led, BG);
  tft.drawString(b, 20, 76);
  tft.drawFastHLine(0, 92, 240, DIVIDER);
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

int cardCount() { return provCount > MAX_CARDS ? MAX_CARDS : provCount; }

// posição vertical do card i (centraliza 1..4 cards na região 93..239)
int cardY(int i) {
  int n = cardCount();
  int gap = n >= MAX_CARDS ? 1 : 4;
  int total = n * CARD_H + (n - 1) * gap;
  int start = CARDS_Y + (240 - CARDS_Y - total) / 2;
  if (start < CARDS_Y) start = CARDS_Y;
  return start + i * (CARD_H + gap);
}

// parte estática do card (desenhada 1x; LED é redraw pequeno por tick)
void drawCardStatic(int i) {
  int y = cardY(i);
  String st = providerState(provs[i]);
  const StateMeta* m = stateMeta(st);
  float pct = provs[i].percent;
  uint16_t border = (st == "sem_saldo") ? RED : CARD_BORDER;

  tft.fillRoundRect(8, y, 224, CARD_H, 6, CARD);
  tft.drawRoundRect(8, y, 224, CARD_H, 6, border);
  tft.fillRect(8, y + 8, 3, 20, m->color);   // barra de acento

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
  tft.drawString(r1, 52, y + 2);
  tft.setTextColor(c2, CARD);
  tft.drawString(r2, 52, y + 19);


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
  fillBg(0, CARDS_Y, 240, 240 - CARDS_Y);
  if (provCount == 0) {
    tft.setTextDatum(CC_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(GRAY, BG);
    tft.drawString("Sem provedores", 120, 165);
    return;
  }
  int n = cardCount();
  for (int i = 0; i < n; i++) {
    drawCardStatic(i);
    drawCardLed(i, frame);
  }
}

// ---------- tela 2: SOLAR (fósforo âmbar) — geometria espelhada em scripts/preview.py ----------
#define SOLAR_SEGS    25         // barra de potência: 25 segmentos de 7px + 2px (x 8..232)
#define CHART_X0      15         // curva: 42 barras de 4px + 1px (x 15..224)
#define CHART_BASE    203
#define CHART_H       28

static const StateMeta SOLAR_STATES[] = {
  {"GERANDO", GREEN, GREEN_DIM, 3, 1},
  {"OCIOSO",  GRAY,  GRAY,      1, 1},
  {"NOITE",   GRAY,  GRAY,      1, 1},
  {"OFFLINE", RED,   RED_DIM,   1, 1},
};
int solarLedX = 0;               // posição do LED (depende da largura da palavra de estado)

bool solarGenerating() { return sol.online && sol.w > 0; }

const StateMeta* solarMeta() {
  if (solarGenerating()) return &SOLAR_STATES[0];
  if (sol.online) return &SOLAR_STATES[1];
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_hour >= 18 || t->tm_hour < 6) return &SOLAR_STATES[2];
  return &SOLAR_STATES[3];
}

int solarCurSlot() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  int m = t->tm_hour * 60 + t->tm_min - sol.slot0;
  return m < 0 ? -1 : m / sol.slotMin;
}

int solarLit() { return sol.online ? (sol.pct * SOLAR_SEGS + 50) / 100 : 0; }

// dígito 7-seg grande (H=44 W=26 T=5) + '.' e '-'
void draw7segBig(int x, int y, char ch, uint16_t color) {
  if (ch == '.') { tft.fillRect(x + 1, y + 38, 6, 6, color); return; }
  if (ch == '-') { tft.fillRect(x + 3, y + 20, 20, 5, color); return; }
  const int W = 26, H = 44, T = 5, I = 3, half = H / 2, span = W - 2 * I;
  uint8_t m = SEG_MASK[(ch - '0') % 10];
  if (m & 0x01) tft.fillRect(x + I, y, span, T, color);
  if (m & 0x02) tft.fillRect(x + W - T, y + I, T, half - I, color);
  if (m & 0x04) tft.fillRect(x + W - T, y + half, T, half - I, color);
  if (m & 0x08) tft.fillRect(x + I, y + H - T, span, T, color);
  if (m & 0x10) tft.fillRect(x, y + half, T, half - I, color);
  if (m & 0x20) tft.fillRect(x, y + I, T, half - I, color);
  if (m & 0x40) tft.fillRect(x + I, y + half - T / 2, span, T, color);
}

// sol com raios que "giram" (frame) ou lua quando o inversor não gera
void drawSun(int x, int y, int frame) {
  fillBg(x, y, 26, 20);
  if (!solarGenerating()) {
    tft.fillCircle(x + 10, y + 9, 7, GRAY);
    tft.fillCircle(x + 14, y + 6, 6, BG);
    return;
  }
  tft.fillCircle(x + 10, y + 9, 5, AMBER);
  float off = (frame % 4 >= 2) ? PI / 8 : 0.0f;
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4 + off;
    int x1 = x + 10 + (int)lroundf(7 * cosf(a)), y1 = y + 9 + (int)lroundf(7 * sinf(a));
    int x2 = x + 10 + (int)lroundf(10 * cosf(a)), y2 = y + 9 + (int)lroundf(10 * sinf(a));
    tft.drawLine(x1, y1, x2, y2, AMBER);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, AMBER);
  }
}

// topo: data | pontos do carrossel | hora (a tela solar não tem o relógio grande)
void drawSolarHeader() {
  fillBg(0, 0, 240, 22);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char b[8];
  bool synced = t->tm_year >= 120;
  if (synced) snprintf(b, sizeof(b), "%02d/%02d", t->tm_mday, t->tm_mon + 1);
  else snprintf(b, sizeof(b), "NTP...");
  tft.setTextColor(synced ? WHITE : GRAY, BG);
  tft.drawString(b, 6, 3);
  tft.drawCircle(114, 11, 3, GRAY);      // tela 1 (IA)
  tft.fillCircle(126, 11, 3, AMBER);     // tela 2 (solar, ativa)
  if (synced) {
    snprintf(b, sizeof(b), "%02d:%02d", t->tm_hour, t->tm_min);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(CYAN, BG);
    tft.drawString(b, 234, 3);
  }
  tft.drawFastHLine(0, 21, 240, DIVIDER);
  tft.drawFastHLine(6, 21, 44, AMBER);
}

void drawSolarAnim(int frame);

// título: > SOLAR ....... (LED) ESTADO  sol/lua
void drawSolarTitle() {
  fillBg(0, 22, 240, 20);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(AMBER, BG);
  tft.drawString(">", 6, 24);
  tft.setTextColor(WHITE, BG);
  tft.drawString("SOLAR", 18, 24);
  const StateMeta* m = solarMeta();
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(m->color, BG);
  int w = tft.drawString(m->word, 206, 24);
  solarLedX = 206 - w - 8;
  drawSun(212, 22, animFrame);
  drawSolarAnim(animFrame);
}

// herói: potência atual em 7-seg âmbar com sombra
void drawSolarHero() {
  fillBg(0, 42, 240, 52);
  char num[8];
  const char* unit = "kW";
  int w = sol.online ? sol.w : 0;
  if (!sol.ok) snprintf(num, sizeof(num), "-.--");
  else if (w >= 1000) snprintf(num, sizeof(num), "%.2f", w / 1000.0f);
  else { snprintf(num, sizeof(num), "%d", w < 0 ? 0 : w); unit = "W"; }
  const int P = 32, DP = 10;
  int width = -(P - 26) + 6 + 24;
  for (char* c = num; *c; c++) width += (*c == '.') ? DP : P;
  int x = 120 - width / 2;
  uint16_t col = sol.online ? AMBER : AMBER_DIM;
  for (char* c = num; *c; c++) {
    draw7segBig(x + 1, 47, *c, AMBER_DARK);   // sombra +1,+1
    draw7segBig(x, 46, *c, col);
    x += (*c == '.') ? DP : P;
  }
  x -= P - 26;
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(GRAY, BG);
  tft.drawString("AGORA", x + 6, 48);
  tft.setTextSize(2);
  tft.setTextColor(col, BG);
  tft.drawString(unit, x + 6, 74);
}

void drawGaugeSeg(int i, uint16_t c) { tft.fillRect(8 + i * 9, 97, 7, 10, c); }

// barra de potência (% da nominal) + rótulos
void drawSolarGauge() {
  fillBg(0, 94, 240, 26);
  int lit = solarLit();
  for (int i = 0; i < SOLAR_SEGS; i++) drawGaugeSeg(i, i < lit ? AMBER : AMBER_DARK);
  char b[24];
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(AMBER, BG);
  snprintf(b, sizeof(b), "%d%%", sol.online ? sol.pct : 0);
  tft.drawString(b, 8, 110);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(GRAY, BG);
  snprintf(b, sizeof(b), "nominal %.1fkW", sol.rated / 1000.0f);
  tft.drawString(b, 232, 110);
}

void drawSolarTile(int x, const char* label, const char* val, const char* unit, uint16_t accent) {
  tft.fillRoundRect(x, 121, 108, 35, 6, CARD);
  tft.drawRoundRect(x, 121, 108, 35, 6, CARD_BORDER);
  tft.fillRect(x, 129, 3, 19, accent);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(GRAY, CARD);
  tft.drawString(label, x + 10, 126);
  tft.setTextSize(2);
  tft.setTextColor(WHITE, CARD);
  int vw = tft.drawString(val, x + 10, 136);
  tft.setTextSize(1);
  tft.setTextColor(accent, CARD);
  tft.drawString(unit, x + 14 + vw, 143);
}

// tiles: energia de hoje / total acumulado
void drawSolarTiles() {
  fillBg(0, 120, 240, 38);
  char b[12];
  if (sol.today >= 0.0f) snprintf(b, sizeof(b), "%.1f", sol.today);
  else snprintf(b, sizeof(b), "--");
  drawSolarTile(8, "HOJE", b, "kWh", AMBER);
  if (sol.total < 0.0f) {
    drawSolarTile(124, "TOTAL", "--", "kWh", CYAN);
  } else if (sol.total >= 1000.0f) {
    snprintf(b, sizeof(b), "%.2f", sol.total / 1000.0f);
    drawSolarTile(124, "TOTAL", b, "MWh", CYAN);
  } else {
    snprintf(b, sizeof(b), "%.0f", sol.total);
    drawSolarTile(124, "TOTAL", b, "kWh", CYAN);
  }
}

// uma barra da curva (limpa a coluna inteira antes)
void drawChartBar(int i, uint16_t fill, uint16_t cap) {
  int bx = CHART_X0 + i * 5;
  tft.fillRect(bx, CHART_BASE - CHART_H + 1, 4, CHART_H, CARD);
  int v = sol.hist[i];
  if (v < 0) return;
  int bh = max(1, (v * CHART_H + 50) / 100);
  tft.fillRect(bx, CHART_BASE - bh + 1, 4, bh, fill);
  tft.fillRect(bx, CHART_BASE - bh + 1, 4, bh >= 2 ? 2 : 1, cap);   // "brilho" no topo
}

// curva de geração do dia (05h..19h)
void drawSolarChart() {
  fillBg(0, 158, 240, 61);
  tft.fillRoundRect(8, 159, 224, 58, 6, CARD);
  tft.drawRoundRect(8, 159, 224, 58, 6, CARD_BORDER);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(GRAY, CARD);
  tft.drawString("CURVA HOJE", 15, 164);
  if (sol.peakW > 0) {
    char b[24];
    snprintf(b, sizeof(b), "pico %.1fkW %s", sol.peakW / 1000.0f, sol.peakT.c_str());
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(AMBER_DIM, CARD);
    tft.drawString(b, 225, 164);
  }
  tft.drawFastHLine(CHART_X0, CHART_BASE + 1, SOLAR_SLOTS * 5, DIVIDER);
  int cur = solarCurSlot();
  for (int i = 0; i < SOLAR_SLOTS; i++) {
    bool isCur = (i == cur && sol.online);
    drawChartBar(i, isCur ? YELLOW : AMBER_DIM, isCur ? YELLOW : AMBER);
  }
  tft.setTextColor(GRAY, CARD);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("5h", CHART_X0, 207);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("12h", CHART_X0 + (720 - sol.slot0) / sol.slotMin * 5, 207);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("19h", CHART_X0 + SOLAR_SLOTS * 5, 207);
}

// rodapé: strings PV + rede, ou há quanto tempo o inversor está offline
void drawSolarFooter() {
  fillBg(0, 219, 240, 21);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  char b[40];
  if (!sol.online) {
    if (!sol.ok) snprintf(b, sizeof(b), "sem leitura: %s", solarErr.c_str());
    else if (sol.age < 3600) snprintf(b, sizeof(b), "inversor offline ha %dmin", sol.age / 60);
    else snprintf(b, sizeof(b), "inversor offline ha %dh%02d", sol.age / 3600, sol.age % 3600 / 60);
    b[38] = '\0';
    tft.setTextColor(GRAY, BG);
    tft.drawString(b, 8, 226);
    return;
  }
  for (int k = 0; k < sol.pvCount; k++) {
    int y = 221 + k * 10;
    snprintf(b, sizeof(b), "PV%d", k + 1);
    tft.setTextColor(AMBER, BG);
    int w = tft.drawString(b, 8, y);
    snprintf(b, sizeof(b), "%.0fV %.1fA %.2fkW", sol.pv[k][0], sol.pv[k][1], sol.pv[k][2] / 1000.0f);
    tft.setTextColor(WHITE, BG);
    tft.drawString(b, 8 + w + 6, y);
  }
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(GRAY, BG);
  snprintf(b, sizeof(b), "%.0fV %.1fHz", sol.vac, sol.hz);
  tft.drawString(b, 232, 221);
  snprintf(b, sizeof(b), "inv %.0fC", sol.temp);
  tft.drawString(b, 232, 231);
}

// animação (tick 250 ms): LED de estado, sol girando, ponta da barra e barra atual piscando
void drawSolarAnim(int frame) {
  const StateMeta* m = solarMeta();
  bool on = (frame % (m->on + m->off)) < m->on;
  if (solarLedX > 0) tft.fillCircle(solarLedX, 32, 3, on ? m->color : m->dim);
  if (frame % 2 == 0) drawSun(212, 22, frame);
  if (!sol.online) return;
  uint16_t c = (frame % 2 == 0) ? YELLOW : AMBER;
  int lit = solarLit();
  if (lit > 0) drawGaugeSeg(lit - 1, c);
  int cur = solarCurSlot();
  if (cur >= 0 && cur < SOLAR_SLOTS) drawChartBar(cur, c, c);
}

void drawSolarData() {
  drawSolarTitle();
  drawSolarHero();
  drawSolarGauge();
  drawSolarTiles();
  drawSolarChart();
  drawSolarFooter();
}

// ---------- carrossel ----------
void drawAiPage() {
  drawTopRegion();
  drawWeatherRegion();
  drawClockDigits();
  drawStatusRegion();
  drawWidgetRegion(animFrame);
}

// transição "varredura de CRT": linha de fósforo desce limpando a tela
void crtWipe(uint16_t lineColor) {
  for (int y = 0; y < 240; y += 16) {
    fillBg(0, y, 240, 16);
    if (y + 16 < 240) tft.drawFastHLine(0, y + 16, 240, lineColor);
    delay(8);
  }
}

void showPage(int p) {
  page = p;
  pageSince = millis();
  crtWipe(p == 1 ? AMBER : PHOSPHOR);
  if (p == 1) {
    drawSolarHeader();
    drawSolarData();
  } else {
    drawAiPage();
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
    "<label>Carrossel: segundos por tela (0 = so a tela de IA)</label><input id='carousel' type='number'>"
    "<button onclick='save()'>Salvar</button>"
    "<button onclick=\"location='/restart'\" style='background:#ff3b30;color:#fff'>Reiniciar</button>"
    "<h2>OTA</h2><a href='/update'>Pagina de update de firmware</a>"
    "<p><a href='/usage'>ultimo JSON recebido</a> · <a href='/config'>config</a></p>"
    "<script>fetch('/config').then(r=>r.json()).then(c=>{"
    "document.getElementById('usage_url').value=c.usage_url;"
    "document.getElementById('usage_interval').value=c.usage_interval;"
    "document.getElementById('tz_offset').value=c.tz_offset;"
    "document.getElementById('carousel').value=c.carousel;});"
    "function save(){const s=(k,v)=>fetch('/api/set?key='+k+'&value='+encodeURIComponent(v));"
    "s('usage_url',document.getElementById('usage_url').value).then(()=>"
    "s('usage_interval',document.getElementById('usage_interval').value)).then(()=>"
    "s('tz_offset',document.getElementById('tz_offset').value)).then(()=>"
    "s('carousel',document.getElementById('carousel').value)).then(()=>alert('Salvo'));}"
    "</script>");
  html += "<h2>Provedores</h2><p><a href='" + collectorBase() +
          "'>Configurar provedores (ativar/desativar, chaves) no coletor</a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleConfig() {
  tft.setTextSize(2);   // FONT2 — métricas reais do topo
  int dateW = tft.textWidth("07/08/2026");
  int ipW = tft.textWidth("192.168.18.67");
  String json = "{\"usage_url\":\"" + cfgUsageUrl + "\",\"usage_interval\":" + String(cfgUsageInterval) +
                ",\"tz_offset\":" + String(cfgTzOffset) +
                ",\"carousel\":" + String(cfgCarousel) + ",\"page\":" + String(page) +
                ",\"solar_err\":\"" + solarErr + "\"" +
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
  if (key == "usage_url") { cfgUsageUrl = value; poll = true; lastSolarPoll = 0; }
  else if (key == "usage_interval") { cfgUsageInterval = value.toInt(); poll = true; }
  else if (key == "tz_offset") { cfgTzOffset = atol(value.c_str()); configTime(cfgTzOffset, 0, "pool.ntp.org"); }
  else if (key == "carousel") {
    cfgCarousel = max(0L, value.toInt());
    if (cfgCarousel == 0 && page != 0) showPage(0);
    lastSolarPoll = 0;
  }
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
  pageSince = millis();
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
      if (lastMin != -1) {
        lastMin = -1;
        if (page == 0) { drawClockDigits(); drawTopRegion(); } else drawSolarHeader();
      }
    } else {
      int cur = t->tm_hour * 60 + t->tm_min;
      if (cur != lastMin) {
        lastMin = cur;
        if (page == 0) {
          drawClockDigits();
          drawTopRegion();   // data/IP — muda por minuto/dia
        } else {
          drawSolarHeader();
          drawSolarChart();  // barra "agora" anda a cada 20 min
        }
      }
      if (page == 0) drawSeconds(t->tm_sec);
    }
  }

  // tick de animação: LED dos cards, dois-pontos do relógio
  static unsigned long lastAnim = 0;
  if (now - lastAnim >= ANIM_MS) {
    lastAnim = now;
    animFrame++;
    if (page == 0) {
      int n = cardCount();
      for (int i = 0; i < n; i++) {
        drawCardLed(i, animFrame);
      }
      drawColon(animFrame % 2 == 0);
    } else {
      drawSolarAnim(animFrame);
    }
  }

  if (lastPollAttempt == 0 || now - lastPollAttempt >= (unsigned long)cfgUsageInterval * 1000UL) {
    lastPollAttempt = now;
    bool ok = fetchUsage();
    if (ok) { lastPollOk = now; lastPollEpoch = time(nullptr); }
    if (page == 0) {   // na tela solar os dados ficam guardados; redesenho completo ao voltar
      if (ok) drawWidgetRegion(animFrame);
      drawStatusRegion();
      drawWeatherRegion();
    }
  }

  // solar: consulta o coletor a cada 30 s (só com carrossel ligado)
  if (cfgCarousel > 0 && (lastSolarPoll == 0 || now - lastSolarPoll >= SOLAR_POLL_MS)) {
    lastSolarPoll = now;
    fetchSolar();
    if (page == 1) {
      if (sol.ok) drawSolarData();
      else showPage(0);   // coletor sem dados do inversor: volta para a tela de IA
    }
  }

  // carrossel: alterna IA <-> solar (solar só entra se o coletor tiver leitura do inversor)
  if (cfgCarousel > 0 && now - pageSince >= (unsigned long)cfgCarousel * 1000UL) {
    int next = (page == 0 && sol.ok) ? 1 : 0;
    if (next != page) showPage(next);
    else pageSince = now;
  }
}
