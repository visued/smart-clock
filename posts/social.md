# Posts para compartilhar

Conteúdo pronto para copiar e colar. Cada post tem uma versão **LinkedIn** e uma versão **X**.
Ajuste o nome, a empresa e os links antes de publicar se quiser.

---

## LinkedIn (versão completa)

---

Comprei um relógio de R$ 80 no AliExpress e transformei ele no painel das minhas contas de IA.

O problema: eu assinava APIs de modelos (DeepSeek, Ollama Cloud), e o saldo e os limites eram um mistério. Descobrir que o limite semanal estourou nunca era "no momento" — era sempre olhando dashboard depois da cota já ter acabado.

A ideia: aquele mini relógio de mesa que mostra hora e clima, com um espacinho vazio na tela... e se eu usasse esse espaço para mostrar o uso real das minhas contas?

O desafio:
- O firmware original é fechado (só binários no GitHub do fabricante — nada de código-fonte)
- Hardware: ESP8266 + tela ST7789 240x240 — mesma família do GeekMagic SmallTV, que tem template ESPHome, então o pinout real foi descoberto na prática (inclusive o backlight que é ativo-baixo, o que me custou uma tela "apagada" no primeiro flash)

A solução:
- Firmware custom em C++ (PlatformIO/TFT_eSPI) com tema PHOSPHOR CONSOLE: relógio 7-segmento verde-fósforo com dois-pontos piscando, scanlines de CRT e clima real (OpenWeatherMap)
- Um coletor em Python rodando em Docker no PC, que consulta a API do DeepSeek (saldo real) e expõe um JSON na rede local
- O relógio puxa esse JSON a cada N minutos e desenha um card por provedor: logo, saldo, limites de sessão/semanal com o "reseta em X"
- Easter egg: um LED por card que pulsa verde quando tem saldo, pisca vermelho quando esgotou e fica âmbar quando falta configurar a chave

O que aprendi no caminho:
- OTA (upload do firmware pela rede) é o superpoder de quem hackeia ESP8266 — flash sem abrir o aparelho
- A Ollama ainda não tem API pública de quota (tem uma issue aberta pedindo isso) — por isso os limites dela são configurados manualmente
- Firmware custom com web UI próprio dá um controle que nenhum "modo de fábrica" oferece

Tudo open source: github.com/visued/smart-clock

#ESP8266 #IoT #SideProject #DIY #OpenSource #LLM #HomeLab #Embedded

---

## LinkedIn (versão curta)

---

Comprei um relógio chinês barato e o transformei em um painel de status das minhas contas de IA: firmware custom (ESP8266 + ST7789), tema de terminal CRT, clima real e um card por provedor com saldo e limites (sessão/semanal) — com LED que pulsa verde quando tem saldo e pisca vermelho quando acabou.

Coletor em Python/Docker consulta a API do DeepSeek e o relógio desenha tudo na tela via rede local. Flash 100% via OTA, sem abrir o aparelho.

Open source: github.com/visued/smart-clock

#ESP8266 #DIY #OpenSource #AI

---

## X (Twitter)

---

Comprei um relógio de R$ 80 no AliExpress e hackeei o firmware para virar um painel de status das minhas contas de IA.

ESP8266 + tela 240x240, firmware custom em C++ (tema terminal CRT), coletor em Docker consultando a API do DeepSeek.

Saldo, limites de sessão/semanal e um LED que pulsa quando tem saldo e pisca vermelho quando estourou.

Tudo open source: github.com/visued/smart-clock

#ESP8266 #DIY #OpenSource #AI

---

## X (versão minúscula, para caber com folga)

---

Relógio de R$ 80 do AliExpress hackeado: firmware custom em C++ (ESP8266) virou painel das minhas contas de IA. Saldo real via API do DeepSeek, limites sessão/semanal, LED vermelho quando a cota acaba. Open source: github.com/visued/smart-clock #ESP8266 #DIY #AI
