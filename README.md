# SmartClock AI

Customização do **JUZIPi SD PRO** ("Smart Weather Clock", ESP8266 + ST7789 240×240) com o tema
**PHOSPHOR CONSOLE** (terminal CRT): relógio 7-segmento verde-fósforo com dois-pontos piscando,
scanlines, e cards HUD de provedores de IA com **easter egg animado de saldo**.

```
┌─────────────┐  APIs de uso (quota)   ┌──────────────────┐
│ collector/  │ ─────────────────────▶ │ DeepSeek API     │
│ (Docker/PC) │                        └──────────────────┘
└──────┬──────┘
       │ GET /usage (JSON local, ex. http://192.168.18.7:8787/usage)
       ▼
┌─────────────┐  a cada N segundos (configurável) + animação 250ms
│ firmware/   │ ────────────────────────────────────────────────────┐
│ (ESP8266)   │  relógio 7-seg + clima + cards com easter egg       │
└─────────────┘
```

## Easter egg de saldo (por card de provedor)

| Estado | LED | Moldura | Extras |
|---|---|---|---|
| **SALDO** (<100%) | verde pulsando (900/300ms) | estática | moeda + "SALDO" verde, cursor branco percorre o medidor |
| **SEM SALDO** (≥100%) | vermelho piscando 2Hz | vermelha piscando | triângulo + "SEM SALDO", % vermelho |
| **SEM CHAVE** (sem chave) | âmbar 1Hz | estática | cadeado + "SEM CHAVE", % vira "--" |

Linha de status: LED verde = serviço OK, âmbar = desatualizado, vermelho = sem serviço.

## Provedores

| Provedor | Fonte do dado | Linha de detalhe |
|---|---|---|
| **DeepSeek** | API real `GET /user/balance` (chave admin) | saldo (ex.: "saldo 29.00 CNY") |
| **Ollama Cloud** | Manual (sem API pública de quota — [ollama/ollama#16448](https://github.com/ollama/ollama/issues/16448)) | texto livre "reset" (ex.: "reseta 10/08 00:00") |
| **Qwen, Kimi, Claude, ChatGPT...** | Manual (`used`/`budget`) — modo genérico, adicione pela web UI | texto livre "reset" |

## Como usar

### 1. Collector (PC, container Docker)

```bash
docker compose up -d --build     # http://0.0.0.0:8787/
```

- **Página web** (`http://<ip-do-pc>:8787/`): ativar/desativar provedores, colar chaves,
  budget/usado/reset, adicionar provedores manuais, botão "Testar" por provedor,
  clima (OpenWeatherMap). Salvar grava `collector/config.yaml` (backup `.bak`).
- Sem Docker: `python3 collector.py` (config em `collector/config.yaml`,
  modelo comentado em `config.yaml.example`).
- **Chave do DeepSeek rápido**: `./scripts/setup_keys.sh sk-XXXX` (aplica e reinicia).

### 2. Firmware

```bash
./scripts/build.sh     # compila e gera firmware/SDP_SmartClockAI_V1.0.0.bin
```

**Flash via OTA**: web UI do relógio (`http://<ip-do-relogio>/` → Firmware Update),
arquivo com prefixo `SDP`. O WiFi salvo é reutilizado (mesmo mecanismo do firmware
original). Config do relógio (`/`): URL do coletor, intervalo (s), fuso (ex. `-10800`),
OTA (`/update`), link para a página de provedores do coletor.

### 3. Preview / mockup antes de flashar

```bash
.venv/bin/python scripts/preview.py --ip 192.168.18.67 --time 12:34:56   # PNG com dados reais
.venv/bin/python scripts/preview.py --demo --gif --out design/animation.gif  # GIF do easter egg
.venv/bin/python scripts/preview.py --demo --out design/mockup.png        # 3 estados de exemplo
```

### 4. Restaurar firmware original

`assets/original/SDPro_V1.0.6_20260525_174828.bin` — mesmo web UI de OTA (nome já começa
com "SDP"). Recuperação por serial exige abrir o aparelho (USB do relógio é só alimentação).

## Estrutura

```
collector/    serviço Python (Docker) + página web de gestão
firmware/     PlatformIO (C++/Arduino): src/main.cpp, src/logos.h (gerado), platformio.ini
scripts/      preview.py, logo2header.py, setup_keys.sh, build.sh
assets/       logos dos provedores + binário original (restore)
design/       mockups e GIF do easter egg
```

## Logos de novos provedores

1. Salve um PNG em `assets/logos/<id>.png` (fundo transparente de preferência).
2. `./scripts/logo2header.py` → regenera `firmware/src/logos.h`.
3. `./scripts/build.sh` → flash. Sem logo, o card usa fallback de letras.

## Hardware (referência)

- ESP8266 ESP-12F/12E (4 MB) + ST7789 240×240 — MOSI=13, SCLK=14, CS=15, DC=2, RST=4, BL=5
  (pinout validado para o SD PRO por [mdrealofficial/esp8266-sdpro-smart-display](https://github.com/mdrealofficial/esp8266-sdpro-smart-display))
- Equivalente: GeekMagic SmallTV Ultra ([template ESPHome](https://devices.esphome.io/devices/geekmagic-ultra/)
  — pinout de DC/RST difere)

## Segurança

- Web UIs do relógio e do coletor **sem autenticação** (LAN). Rede doméstica confiável apenas.
- Chaves de API ficam **só no PC** (`collector/config.yaml`, fora do git), nunca no relógio.
