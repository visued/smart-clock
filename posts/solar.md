# Posts — Tela solar (inversor PHB via Modbus) no SmartClock

Conteúdo pronto para copiar e colar: versão **LinkedIn** e versão **X** (curta, ≤ 280 caracteres).
Mídia sugerida: `design/solar.gif` (animação da tela) + uma foto do relógio real na tela solar.

---

## LinkedIn

---

Meu relógio de mesa agora mostra quanto o meu telhado está gerando. Sem nuvem e sem app: ele lê direto do inversor.

Há dois meses eu transformei um relógio de R$ 80 do AliExpress em painel das minhas contas de IA. Faltava a energia solar. O app do fabricante funciona, mas eu queria bater o olho na mesa e ver a geração em tempo real.

O problema: o inversor (PHB5000D-WS) não tem API documentada. Só o app e o portal na nuvem.

A engenharia reversa:
1. Varri a rede local atrás de portas típicas de inversor. Um único aparelho respondia na porta 502: Modbus TCP, sem autenticação nenhuma.
2. O módulo de comunicação é um gateway "teimoso": ignora o ID de escravo, só aceita a função 03 e, quando você pede um endereço que não existe, simplesmente fica em silêncio, segurando a fila por ~1 s. Varrer 65 mil endereços às cegas levaria horas.
3. Então testei só os endereços onde os mapas Modbus de fabricantes costumam começar. A faixa 30000 respondeu com dados vivos.
4. Identifiquei cada campo pela física: tensão × corrente bate com a potência, o relógio interno bate com a hora atual, a eficiência CC→CA fecha em ~96%.
5. Para separar "energia de hoje" de "energia total", tirei dois snapshots com 90 s de intervalo. Os únicos contadores que subiram 0,1 kWh eram exatamente os que deviam subir gerando 4 kW.

O resultado:
- Mapa de registradores documentado, com o nível de confiança de cada campo, e um leitor em Python (só biblioteca padrão, somente leitura)
- O coletor do relógio lê o inversor a cada 30 s e monta a curva de geração do dia
- O firmware ganhou um carrossel: tela de IA em fósforo verde e tela solar em fósforo âmbar, com potência em 7 segmentos, energia de hoje e total, curva do dia e transição de "varredura de CRT"

O que aprendi:
- Muito equipamento "smart" fala Modbus na sua LAN sem senha nenhuma. Ótimo para integrar, e um bom motivo para colocar IoT numa rede separada.
- O módulo de comunicação vem com senha padrão de fábrica. Troque a sua.
- Quando a documentação não existe, a física vira documentação: V × I = W não mente.

Tudo open source:
Relógio: github.com/visued/smart-clock
Mapa Modbus do inversor + leitor Python: github.com/visued/phb5000d-ws-modbus

#EnergiaSolar #IoT #Modbus #ESP8266 #EngenhariaReversa #HomeLab #OpenSource #DIY

---

## X (Twitter)

---

Meu relógio de mesa de R$ 80 agora mostra a geração solar em tempo real ☀️

Ele lê o inversor direto pela LAN via Modbus TCP, sem nuvem. Mapeei os registradores pela física (V×I=W) e ganhei uma tela em fósforo âmbar.

github.com/visued/smart-clock

---

### X — fio opcional (respostas ao post acima)

---

2/ O inversor não tem API documentada. Achei a porta 502 aberta, mas o gateway ignora o ID de escravo e fica mudo em endereço inválido. Varrer tudo levaria horas, então testei só os endereços onde os fabricantes costumam começar. 30000 respondeu.

---

3/ Para achar "energia de hoje" x "total": 2 leituras com 90 s de intervalo. A 4 kW, 90 s = 0,1 kWh, e só esses dois contadores subiram exatamente isso.

Lição: muito IoT fala Modbus na sua LAN sem senha. Isole a rede e troque a senha padrão.
