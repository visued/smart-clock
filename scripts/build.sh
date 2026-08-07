#!/usr/bin/env bash
# Compila o firmware e deixa o binário pronto para OTA (prefixo SDP).
# Uso: ./scripts/build.sh
# Depois: flash pelo web UI do relógio (http://<ip-do-relogio>/ -> Firmware Update)
set -euo pipefail

.venv/bin/pio run -d firmware
cp firmware/.pio/build/esp12e/firmware.bin firmware/SDP_SmartClockAI_V1.0.0.bin
echo "ok: firmware/SDP_SmartClockAI_V1.0.0.bin"
echo "Flash via OTA: http://<ip-do-relogio>/ -> Firmware Update (OTA)"
