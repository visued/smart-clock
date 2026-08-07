#!/usr/bin/env bash
# Configura a chave do DeepSeek no collector e reinicia o serviço (sem rebuild).
# Uso: ./scripts/setup_keys.sh sk-XXXX
# Depois, se quiser ajustar mais nada (budget, reset, provedores), use a página
# web do coletor: http://<ip-do-pc>:8787/
set -euo pipefail

KEY="${1:-}"
if [ -z "$KEY" ]; then
  echo "uso: $0 sk-XXXX"
  exit 1
fi

CFG="collector/config.yaml"
if [ ! -f "$CFG" ]; then
  cp collector/config.yaml.example "$CFG"
fi

# aplica a chave no bloco deepseek
python3 - "$KEY" <<'PY'
import sys
key = sys.argv[1]
path = "collector/config.yaml"
lines = open(path).read().splitlines()
out, in_ds = [], False
for ln in lines:
    if ln.startswith("deepseek:"):
        in_ds = True
    elif in_ds and ln and not ln[0].isspace():
        in_ds = False
    if in_ds and "api_key:" in ln and "sk-" in ln:
        ln = f'    api_key: "{key}"'
    out.append(ln)
open(path, "w").write("\n".join(out) + "\n")
PY

docker compose restart collector
echo "ok — chave aplicada em $CFG e serviço reiniciado."
echo "Verifique: curl -s http://127.0.0.1:8787/usage"
