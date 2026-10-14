# ⏰ SmartClock AI — PHOSPHOR CONSOLE

Custom firmware + coletor para o **JUZIPi SD PRO** ("Smart Weather Clock", ESP8266 + ST7789 240×240),
com tema de terminal CRT e **easter egg animado de saldo** nas contas de IA.

```
┌─────────────┐  APIs de uso (quota)   ┌──────────────────┐
│ collector/  │ ─────────────────────▶ │ DeepSeek API     │
│ (Docker/PC) │                        └──────────────────┘
└──────┬──────┘
       │ GET /usage (JSON local, ex. http://192.168.18.7:8787/usage)
       ▼
┌─────────────┐  a cada N segundos (configurável) + animação 250ms
│ firmware/   │ ────────────────────────────────────────────────────┐
│ (ESP8266)   │  relógio 7-seg fósforo + clima + cards de provedor │
└─────────────┘
```

## 📸 Screenshots

| Interface web do coletor | Relógio configurado |
|---|---|
| ![Coletor](docs/collector-ui.png) | ![Relógio](assets/device.jpeg?v=2) |

*Interface do coletor (ativar/desativar provedores, chaves, limites) e o relógio rodando o firmware.*

## 🛒 Onde comprar

![Relógio SmartClock](relogio.png)

Quer montar o seu? Compre o relógio **JUZIPi SD PRO** ("Smart Weather Clock") aqui:
[Comprar na AliExpress](https://s.click.aliexpress.com/e/_msaW5j1)

## ✨ O que a tela mostra

- **Relógio 7-segmento** verde-fósforo com sombra e dois-pontos piscando, scanlines de CRT
- **Clima atual** (OpenWeatherMap) com ícone procedural
- **Cards de provedores de IA** com logo da marca + linhas de limite:
  ```
  [🐋]  saldo 4.34 USD    ← % global (pior limite) no canto
        usado 91%         ← limite/saldo por linha, cor por uso
  ```
- **Limites múltiplos por provedor** (ex.: `H 45% em 3h` / `W 60% 2d`) — como o painel da Ollama
- **Easter egg de saldo**: LED por card que pulsa (verde = saldo), pisca (vermelho = esgotado) ou âmbar (sem chave)

### 🔁 Carrossel — tela 2: Solar (fósforo âmbar)

O relógio alterna entre a tela de IA e a tela do **inversor solar PHB5000D-WS** a cada N segundos
(`carousel` na web UI do relógio; padrão 15 s, `0` = só a tela de IA). A transição é uma
"varredura de CRT".

![Tela solar](design/solar.gif)

- **Potência agora** em 7-seg âmbar + barra de 25 segmentos (% da nominal, ponta pulsando)
- **Hoje** (kWh) e **Total** (MWh)
- **Curva do dia** 05h–19h em barras de 20 min, com pico e a barra atual piscando
- **PV1/PV2** (V, A, kW), tensão/frequência da rede e temperatura do inversor
- Estado: `GERANDO` (LED verde), `OCIOSO`, `NOITE` (lua) ou `OFFLINE` (com "há quanto tempo")

Os dados vêm do **coletor**, que lê o inversor localmente via **Modbus TCP** (somente leitura) e
expõe `GET /solar`. Se o coletor não tiver leitura do inversor, o carrossel fica só na tela de IA.
Engenharia reversa e mapa de registradores: [visued/phb5000d-ws-modbus](https://github.com/visued/phb5000d-ws-modbus).

## 📦 Estrutura

```
collector/    Serviço Python (Docker) + página web de gestão (http://<pc>:8787/)
firmware/     PlatformIO (C++/Arduino): src/main.cpp, src/logos.h (gerado), platformio.ini
scripts/      preview.py (mockup PNG/GIF da tela), logo2header.py, setup_keys.sh, build.sh
assets/       logos dos provedores + binário original (restore)
docs/         screenshots
design/       mockups e GIF do easter egg
```

## 🚀 Como usar

### 1. Coletor (no PC)

```bash
docker compose up -d --build     # http://0.0.0.0:8787/
```

Abre `http://<ip-do-pc>:8787/` — ative/desative provedores, cole chaves, configure
budget/limites/reset, teste a chave e o clima. Salvar grava `collector/config.yaml`
(backup `.bak`). Sem Docker: `python3 collector.py` (config em `collector/config.yaml`,
modelo comentado em `config.yaml.example`).

Chave do DeepSeek rápido: `./scripts/setup_keys.sh sk-XXXX`

### 2. Firmware

```bash
./scripts/build.sh    # compila e gera firmware/SDP_SmartClockAI_V1.0.0.bin
```

**Flash via OTA**: web UI do relógio (`http://<ip-do-relogio>/` → Firmware Update), arquivo
com prefixo `SDP`. O WiFi salvo é reutilizado. Config do relógio: URL do coletor, intervalo
(s), fuso horário (ex. `-10800` Brasil), OTA e link para o coletor.

### 3. Preview antes de flashar

```bash
.venv/bin/python scripts/preview.py --ip 192.168.18.67 --time 12:34:56        # PNG com dados reais
.venv/bin/python scripts/preview.py --demo --gif --out design/animation.gif   # GIF do easter egg
.venv/bin/python scripts/preview.py --page solar --solar-url http://127.0.0.1:8787/solar --out design/solar.png
.venv/bin/python scripts/preview.py --page solar --demo --gif --frames 8 --out design/solar.gif
```

### 4. Restaurar firmware original

`assets/original/SDPro_V1.0.6_20260525_174828.bin` — mesmo web UI de OTA.

## 🔌 Provedores

| Provedor | Dados | Reset |
|---|---|---|
| **DeepSeek** | API real `GET /user/balance` (chave admin, conta pré-paga) | sem reset (saldo) |
| **Ollama Cloud** | Quota real via `GET ollama.com/api/usage` (não documentado — [ollama/ollama#12532](https://github.com/ollama/ollama/issues/12532)) + plano via `/api/me` + fallback manual | reset computado (sessão: borda de 5h; semanal: segunda 00:00 UTC) |
| **Z.ai GLM** | Quota real via endpoint de monitoramento (não-oficial — pode mudar) + fallback manual | reset por limite |
| **Claude (Pro/Max)** | Quota real via `GET api.anthropic.com/api/oauth/usage` (não documentado — o mesmo do `/usage` do Claude Code) com o token OAuth do login do Claude Code (`~/.claude/.credentials.json`, montado read-only no Docker) + fallback manual | reset por limite (sessão 5h / semanal) |
| **Qwen, Kimi, ChatGPT...** | Manual (`used`/`budget` + limites) | texto livre por limite |
| **Inversor solar PHB** (tela 2) | Modbus TCP local, função 03, a cada 30 s (`phb_solar` no `config.yaml`) → `GET /solar`; curva do dia em memória (zera se o coletor reiniciar) | — |

Cada provedor aceita **limites extras** (sessão/semanal/mensal): rótulo, usado, budget e reset.

## 🎨 Logos de novos provedores

1. Salve o PNG em `assets/logos/<id>.png` (transparente de preferência)
2. Ajuste `LOGO_MODE`/`BADGE_COLORS` em `scripts/logo2header.py`
3. `./scripts/logo2header.py` → `./scripts/build.sh` → flash

## 📱 Modelos compatíveis

| Modelo | Placa | Status |
|---|---|---|
| **JUZIPi SD PRO** | ESP8266 ESP-12F/12E + ST7789 240×240 | ✅ testado (este projeto) |
| **GeekMagic SmallTV** | ESP8266 (mesma placa) | ✅ mesmo pinout |
| **GeekMagic SmallTV Ultra** | ESP8266 | ✅ mesmo pinout |
| **GeekMagic SmallTV Pro** | ESP32 | ⚠️ requer outro env de build |
| **NMMiner NM-TV-154** | ESP32-C2 | ⚠️ requer outro env de build |

Verifique a placa (ESP8266 vs ESP32) **antes** de flashar — o firmware e o pinout abaixo são
para a família ESP8266. Na dúvida, abra o aparelho e confira o chip.

## 🖥️ Hardware (referência)

- ESP8266 ESP-12F/12E (4 MB) + ST7789 240×240 — MOSI=13, SCLK=14, CS=15, DC=0, RST=2, BL=5 (backlight ativo-baixo)
- Pinout da família SmallTV/Ultra (template [ESPHome](https://devices.esphome.io/devices/geekmagic-ultra/))

## 🔒 Segurança

- Web UIs **sem autenticação** (LAN) — rede doméstica confiável
- Chaves de API ficam **só no PC** (`collector/config.yaml`, fora do git), nunca no relógio
