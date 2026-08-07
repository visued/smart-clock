#!/usr/bin/env python3
"""SmartClock AI Collector — busca uso/quota de provedores de IA e expõe JSON
para o relógio (firmware custom) via GET /usage.

Página web (GET /): ativar/desativar provedores, definir chaves/budget/reset.
Provedores sem adapter dedicado usam o modo manual (used/budget/reset livres),
então qualquer provedor futuro (Qwen, Kimi, Claude, ChatGPT...) pode ser
adicionado pela interface.

Uso:
  python3 collector.py [--config config.yaml] [--once]
"""
import argparse
import json
import os
import shutil
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import yaml

DEFAULT_CONFIG = {
    "listen": "0.0.0.0",
    "port": 8787,
    "cache_ttl": 60,
    "providers": {},
}


def fetch_json(url, headers=None, timeout=10):
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


# --- Adapters: retornam dict {id,name,percent,label,reset,ok} ou levantam exceção ---

def deepseek(cfg):
    key = str(cfg.get("api_key", "")).strip()
    if not key or "TROQUE" in key:
        # Fallback: sem chave, mostra provedor com 0% (tela do relógio já fica completa)
        return {
            "id": "deepseek",
            "name": cfg.get("display_name", "DeepSeek"),
            "percent": 0.0,
            "label": "sem chave de API",
            "reset": "",
            "state": "sem_chave",
            "limits": [],
            "ok": True,
        }
    data = fetch_json(
        "https://api.deepseek.com/user/balance",
        headers={"Authorization": f"Bearer {key}"},
    )
    infos = data["balance_infos"]
    # prioriza CNY (moeda nativa); senão usa a primeira entrada
    info = next((i for i in infos if i["currency"] == "CNY"), infos[0])
    total = float(info["total_balance"])
    budget = float(cfg.get("budget") or 0)
    pct = None
    if budget > 0:
        used = max(0.0, budget - total)
        pct = min(100.0, used / budget * 100.0)
    return {
        "id": "deepseek",
        "name": cfg.get("display_name", "DeepSeek"),
        "percent": pct,
        "label": f"saldo {total:.2f} {info['currency']}",
        "reset": "",
        "state": ("sem_saldo" if pct is not None and pct >= 100 else "saldo"),
        "limits": [],
        "ok": True,
    }


def manual(cfg):
    """Modo manual genérico — vale para qualquer provedor (Qwen, Kimi, Claude,
    ChatGPT...) até existir adapter com API de uso real."""
    budget = float(cfg.get("budget") or 100)
    used = float(cfg.get("used") or 0)
    pct = min(100.0, used / budget * 100.0) if budget > 0 else None
    limits = []
    for l in (cfg.get("limits") or []):
        try:
            lb = float(l.get("budget") or 0)
            lu = float(l.get("used") or 0)
            lp = min(100.0, lu / lb * 100.0) if lb > 0 else None
        except (TypeError, ValueError):
            continue
        limits.append({"label": str(l.get("label", "?")), "percent": lp,
                       "reset": str(l.get("reset", ""))})
    if limits:  # % principal = pior limite (o que o relógio destaca)
        pcts = [l["percent"] for l in limits if l["percent"] is not None]
        if pcts:
            pct = max(pct or 0.0, max(pcts))
    return {
        "id": str(cfg.get("id", "manual")),
        "name": cfg.get("display_name", "Manual"),
        "percent": pct,
        "label": f"manual · {used:g}/{budget:g}",
        "reset": str(cfg.get("reset", "")),
        "state": ("sem_saldo" if pct is not None and pct >= 100 else "saldo"),
        "limits": limits,
        "ok": True,
    }


ADAPTERS = {"deepseek": deepseek, "ollama_cloud": manual}


def openweathermap(cfg):
    """Clima atual — usado na linha de clima do relógio (não é um provider/card)."""
    key = str(cfg.get("api_key", "")).strip()
    city = str(cfg.get("city", "")).strip()
    if not key or not city or "TROQUE" in key:
        return None
    url = (
        "https://api.openweathermap.org/data/2.5/weather"
        f"?q={urllib.parse.quote(city)}&appid={key}&units=metric&lang=pt_br"
    )
    data = fetch_json(url)
    return {
        "temp": float(data["main"]["temp"]),
        "condition": data["weather"][0]["description"],
        "icon": data["weather"][0]["icon"],
        "ok": True,
    }


class Collector:
    def __init__(self, cfg, config_path=None):
        self.cfg = cfg
        self.config_path = config_path
        self.lock = threading.Lock()
        # id -> (timestamp, resultado dict | None, erro str | None)
        self.cache = {}
        self.wcache = (0.0, None, "aguardando primeira consulta")
        for pid, pcfg in cfg["providers"].items():
            if pcfg.get("enabled", True):
                self.cache[pid] = (0.0, None, "aguardando primeira consulta")

    def refresh(self, pid, pcfg):
        try:
            res = ADAPTERS.get(pid, manual)({**pcfg, "id": pid})
            with self.lock:
                self.cache[pid] = (time.time(), res, None)
        except Exception as exc:  # noqa: BLE001 — falha de provedor não derruba o serviço
            with self.lock:
                self.cache[pid] = (time.time(), None, str(exc))

    def save_config(self, providers, weather):
        def _limits(p):
            raw = p.get("limits")
            if isinstance(raw, list):
                return raw
            if isinstance(raw, str) and raw.strip():
                try:
                    return json.loads(raw)
                except json.JSONDecodeError:
                    return []
            return []
        providers = {
            pid: {
                "enabled": bool(p.get("enabled", True)),
                "display_name": str(p.get("name") or pid),
                "api_key": str(p.get("api_key", "")),
                "budget": float(p.get("budget") or 0),
                "used": float(p.get("used") or 0),
                "reset": str(p.get("reset", "")),
                "limits": _limits(p),
            }
            for pid, p in (providers or {}).items()
        }
        weather = {k: (bool(v) if k == "enabled" else str(v)) for k, v in (weather or {}).items()}
        new = {k: v for k, v in self.cfg.items() if k not in ("providers", "openweathermap")}
        new["providers"] = providers
        new["openweathermap"] = weather
        if self.config_path and os.path.exists(self.config_path):
            shutil.copy2(self.config_path, self.config_path + ".bak")
        with open(self.config_path, "w", encoding="utf-8") as fh:
            yaml.safe_dump(new, fh, sort_keys=False, allow_unicode=True)
        self.cfg["providers"] = providers
        self.cfg["openweathermap"] = weather
        with self.lock:
            self.cache = {
                pid: (0.0, None, "aguardando primeira consulta")
                for pid, p in providers.items() if p["enabled"]
            }
            self.wcache = (0.0, None, "aguardando primeira consulta")
        return True

    def snapshot(self):
        out = []
        for pid, pcfg in self.cfg["providers"].items():
            if not pcfg.get("enabled", True):
                continue
            with self.lock:
                ts, res, err = self.cache.get(pid, (0.0, None, "não iniciado"))
            if res is None or time.time() - ts > self.cfg["cache_ttl"]:
                self.refresh(pid, pcfg)
                with self.lock:
                    ts, res, err = self.cache[pid]
            if res is None:
                out.append({
                    "id": pid,
                    "name": pcfg.get("display_name", pid),
                    "percent": 0.0,
                    "label": f"erro: {err}"[:40],
                    "reset": "",
                    "state": "erro",
                    "ok": True,   # fallback: provedor + 0% mantém a tela do relógio completa
                })
                continue
            out.append(dict(res))

        # clima (linha de clima do relógio) — cache próprio de 5 min
        weather = {"ok": False}
        wcfg = self.cfg.get("openweathermap") or {}
        if wcfg.get("enabled", True):
            with self.lock:
                wts, wres, _ = self.wcache
            if wres is None or time.time() - wts > 300:
                try:
                    wres = openweathermap(wcfg)
                    with self.lock:
                        self.wcache = (time.time(), wres, None)
                except Exception as exc:  # noqa: BLE001
                    with self.lock:
                        self.wcache = (time.time(), None, str(exc))
                    wres = None
            if wres is not None:
                weather = dict(wres)

        return {
            "updated": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "interval_hint": self.cfg["cache_ttl"],
            "weather": weather,
            "providers": out,
        }


PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>SmartClock AI Coletor</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{background:#111;color:#eee;font-family:sans-serif;padding:16px;max-width:760px;margin:auto}
h1{font-size:20px}h2{font-size:16px;margin-top:26px}
.box{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:12px;margin:10px 0}
.row{display:flex;align-items:center;gap:8px;margin:6px 0;flex-wrap:wrap}
label{font-size:13px;color:#aaa;min-width:70px}
input{background:#222;color:#eee;border:1px solid #555;border-radius:4px;padding:6px 8px;flex:1;min-width:120px}
input[type=checkbox]{flex:0;min-width:auto}
button{background:#34c759;color:#000;border:none;border-radius:6px;padding:8px 14px;margin:4px 4px 0 0;cursor:pointer}
button.danger{background:#ff3b30;color:#fff}button.gray{background:#48484a;color:#fff}
.tres{font-size:12px;color:#8e8e93;margin-left:8px}
.lim{display:flex;align-items:center;gap:6px;margin:5px 0;flex-wrap:wrap}
.lim label{min-width:34px;font-size:12px}
.lim input{min-width:56px;flex:0 1 auto}
#msg{color:#34c759;font-weight:bold}
a{color:#4d6bfe}
</style></head><body>
<h1>SmartClock AI — Coletor</h1>
<p><a href="/usage">JSON para o relógio</a></p>
<h2>Provedores (cards na tela do relógio)</h2>
<p style="font-size:13px;color:#8e8e93">Sem chave = card mostra 0% (fallback). DeepSeek usa saldo real
por API. Qualquer outro provedor (Qwen, Kimi, Claude, ChatGPT...) é modo manual:
preencha <b>budget</b> (limite) e <b>usado</b> (atual), e <b>reset</b> = quando o limite reseta.</p>
<div id="provs"></div>
<button type="button" class="gray" onclick="addProv()">+ Adicionar provedor</button>
<h2>Clima (linha no topo do relógio)</h2>
<div class="box">
  <div class="row"><label>Ativo</label><input type="checkbox" id="w_enabled"></div>
  <div class="row"><label>API key</label><input type="password" id="w_key"></div>
  <div class="row"><label>Cidade</label><input id="w_city" placeholder="ex.: Sao Joaquim da Barra,BR"></div>
</div>
<button onclick="save()">Salvar tudo</button> <span id="msg"></span>
<script>
const INIT = __INIT__;
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;');}
function sec(p){
  return `<div class="box" data-pid="${esc(p.id)}">
    <div class="row"><label>id</label><input class="f-id" value="${esc(p.id)}" ${p.id==='deepseek'?'readonly':''}>
      <label>ativo</label><input type="checkbox" class="f-en" ${p.enabled?'checked':''}></div>
    <div class="row"><label>nome</label><input class="f-name" value="${esc(p.name)}"></div>
    <div class="row"><label>api key</label><input type="password" class="f-key" value="${esc(p.api_key)}"
      placeholder="${p.id==='deepseek'?'chave sk-... (saldo real)':'opcional'}"></div>
    <div class="row"><label>budget</label><input type="number" class="f-budget" value="${esc(p.budget)}">
      <label>usado</label><input type="number" class="f-used" value="${esc(p.used)}">
      <label>reset</label><input class="f-reset" value="${esc(p.reset)}" placeholder="ex.: reseta 10/08 00:00"></div>
    <div class="lims"></div>
    <div class="row"><button type="button" onclick="testProv(this)">Testar</button><span class="tres"></span>
      <button type="button" class="danger" onclick="this.closest('.box').remove()">Remover</button></div>`;
}
function limRow(l){
  l=l||{};
  return `<div class="lim"><label>limite</label><input class="l-label" size="2" maxlength="2"
    value="${esc(l.label||'')}" placeholder="H">
    <label>usado</label><input type="number" class="l-used" value="${esc(l.used??0)}">
    <label>budget</label><input type="number" class="l-budget" value="${esc(l.budget??100)}">
    <label>reset</label><input class="l-reset" value="${esc(l.reset||'')}" placeholder="ex.: em 3h">
    <button type="button" class="danger" onclick="this.closest('.lim').remove()">x</button></div>`;
}
function limBox(limits){
  const d=document.createElement('div');
  (limits||[]).forEach(l=>{const x=document.createElement('div');x.innerHTML=limRow(l);d.appendChild(x.firstChild);});
  const b=document.createElement('button');b.type='button';b.className='gray';b.textContent='+ limite (sessao/semanal)';
  b.onclick=()=>{const x=document.createElement('div');x.innerHTML=limRow({});d.insertBefore(x.firstChild,b);};
  d.appendChild(b);return d;
}
function limCollect(box){
  return [...box.querySelectorAll('.lim')].map(r=>({
    label:r.querySelector('.l-label').value,
    used:parseFloat(r.querySelector('.l-used').value||0),
    budget:parseFloat(r.querySelector('.l-budget').value||0),
    reset:r.querySelector('.l-reset').value,
  })).filter(l=>l.label);
}
function render(){const box=document.getElementById('provs');box.innerHTML='';
  INIT.providers.forEach(p=>{const d=document.createElement('div');d.innerHTML=sec(p);
    const s=d.firstChild; s.querySelector('.lims').appendChild(limBox(p.limits)); box.appendChild(s);});}
function addProv(){const d=document.createElement('div');
  d.innerHTML=sec({id:'novo_provedor',name:'Novo provedor',enabled:true,api_key:'',budget:100,used:0,reset:'',limits:[]});
  const s=d.firstChild; s.querySelector('.lims').appendChild(limBox([]));
  document.getElementById('provs').appendChild(s);}
function collect(){return [...document.querySelectorAll('#provs .box')].map(b=>({
  id:b.querySelector('.f-id').value.trim(),
  name:b.querySelector('.f-name').value,
  enabled:b.querySelector('.f-en').checked,
  api_key:b.querySelector('.f-key').value,
  budget:parseFloat(b.querySelector('.f-budget').value||0),
  used:parseFloat(b.querySelector('.f-used').value||0),
  reset:b.querySelector('.f-reset').value,
  limits:limCollect(b.querySelector('.lims')),
})).filter(p=>p.id);}
function save(){
  const providers={}; for(const p of collect()) providers[p.id]=p;
  const body={providers,openweathermap:{
    enabled:document.getElementById('w_enabled').checked,
    api_key:document.getElementById('w_key').value,
    city:document.getElementById('w_city').value}};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body)}).then(r=>r.json()).then(j=>{
      document.getElementById('msg').textContent=j.ok?'Salvo! Relógio atualiza no próximo intervalo.':'ERRO: '+j.error;
      if(j.ok) setTimeout(()=>location.reload(),800);}).catch(e=>{
      document.getElementById('msg').textContent='ERRO: '+e;});}
function testProv(btn){
  const b=btn.closest('.box');
  const body={id:b.querySelector('.f-id').value.trim(),config:{
    name:b.querySelector('.f-name').value,api_key:b.querySelector('.f-key').value,
    budget:parseFloat(b.querySelector('.f-budget').value||0),
    used:parseFloat(b.querySelector('.f-used').value||0),
    reset:b.querySelector('.f-reset').value}};
  btn.disabled=true;btn.textContent='...';
  fetch('/api/test',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body)}).then(r=>r.json()).then(j=>{
      b.querySelector('.tres').textContent=j.ok?'ok: '+(j.result.label||JSON.stringify(j.result)):'erro: '+j.error;
      btn.disabled=false;btn.textContent='Testar';});}
render();
document.getElementById('w_enabled').checked=INIT.weather.enabled;
document.getElementById('w_key').value=INIT.weather.api_key;
document.getElementById('w_city').value=INIT.weather.city;
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    server_version = "SmartClockCollector/1.0"

    def _json(self, payload, code=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except json.JSONDecodeError:
            return {}

    def do_GET(self):  # noqa: N802
        path = self.path.split("?")[0]
        if path == "/usage":
            self._json(self.server.collector.snapshot())
        elif path == "/healthz":
            self._json({"ok": True})
        elif path in ("/", ""):
            cfg = self.server.collector.cfg
            providers = [{
                "id": pid,
                "name": pcfg.get("display_name", pid),
                "enabled": bool(pcfg.get("enabled", True)),
                "api_key": pcfg.get("api_key", ""),
                "budget": pcfg.get("budget", 0),
                "used": pcfg.get("used", 0),
                "reset": pcfg.get("reset", ""),
                "limits": pcfg.get("limits") or [],
            } for pid, pcfg in cfg.get("providers", {}).items()]
            w = cfg.get("openweathermap") or {}
            init = json.dumps({
                "providers": providers,
                "weather": {
                    "enabled": bool(w.get("enabled", True)),
                    "api_key": w.get("api_key", ""),
                    "city": w.get("city", ""),
                },
            }).replace("</", "<\\/")
            html = PAGE.replace("__INIT__", init).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):  # noqa: N802
        body = self._body()
        path = self.path.split("?")[0]
        if path == "/api/config":
            try:
                self.server.collector.save_config(body.get("providers"), body.get("openweathermap"))
                self._json({"ok": True})
            except Exception as exc:  # noqa: BLE001
                self._json({"ok": False, "error": str(exc)}, 500)
        elif path == "/api/test":
            pid = str(body.get("id", ""))
            pcfg = body.get("config", {})
            try:
                res = ADAPTERS.get(pid, manual)({**pcfg, "id": pid})
                self._json({"ok": True, "result": res})
            except Exception as exc:  # noqa: BLE001
                self._json({"ok": False, "error": str(exc)})
        else:
            self._json({"error": "not found"}, 404)

    def log_message(self, fmt, *args):  # noqa: A003
        print("[collector] " + fmt % args, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.yaml")
    ap.add_argument("--once", action="store_true", help="consulta uma vez e imprime JSON")
    args = ap.parse_args()

    with open(args.config, encoding="utf-8") as fh:
        user_cfg = yaml.safe_load(fh) or {}
    cfg = {**DEFAULT_CONFIG, **user_cfg}
    cfg.setdefault("providers", {})
    if not isinstance(cfg["providers"], dict):
        raise SystemExit("config.yaml: 'providers' precisa ser um mapa (dict)")

    collector = Collector(cfg, config_path=args.config)
    if args.once:
        for pid, pcfg in cfg["providers"].items():
            if pcfg.get("enabled", True):
                collector.refresh(pid, pcfg)
        print(json.dumps(collector.snapshot(), indent=2, ensure_ascii=False))
        return

    httpd = ThreadingHTTPServer((cfg["listen"], int(cfg["port"])), Handler)
    httpd.collector = collector
    print(f"[collector] ouvindo em http://{cfg['listen']}:{cfg['port']}/usage", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
