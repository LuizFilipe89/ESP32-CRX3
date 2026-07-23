# Plano de implementação — Interface web completa rodando por USB serial

**Objetivo:** rodar a **mesma interface** de `data/` (index.html + app.js + style.css, com
abas, i18n en/pt-BR, popups de ajuda, slider de intensidade) hospedada na web, mas falando
com o ESP32 por **USB serial** (Web Serial no PC, WebUSB no Android) em vez de WiFi/HTTP.
Todas as funções devem continuar funcionando e o resultado deve ser **mais estável** que o
modelo WiFi anterior.

---

## 0. Por que isso é mais estável (a grande vitória)

No modelo WiFi antigo, o navegador do operador conectava no AP `crx3`. Vários ataques
(Deauth, Targeted, Evil Twin, Multi-Clone) **derrubam o AP de gerência** para usar o rádio —
e a interface **caía junto** (o próprio README diz "power cycle to stop"). Isso é a maior
fonte de instabilidade do modelo atual.

No modelo USB, o canal de controle é a **UART**, que é totalmente independente do rádio WiFi.
O operador **nunca perde a conexão**, mesmo durante Deauth/Evil Twin. Não precisa entrar no
AP, não precisa timeout de segurança, não precisa power-cycle. Essa é a justificativa central
de "mais estável".

---

## 1. Princípio de reaproveitamento

A regra é: **preservar `index.html`, `style.css`, os assets (`icons/`, `fonts/`) e ao máximo
o `app.js` sem reescrever a lógica de UI.** Só trocamos a *camada de transporte*.

O `app.js` de hoje fala HTTP de duas formas:
- `XMLHttpRequest` binário → `/ap-list`, `/run-attack`, `/status`, `/reset`
- `fetch` (texto/JSON) → `/evil-twin-status`, `/devil_twin/portal-state`, `/save_settings`,
  `/devil_twin/upload`, `/devil_twin/restore-default`, `/eviltwin-log`, `/eviltwin-log/clear`,
  `/custom-evil-twin`
- Downloads (`<a href>`) → `/capture.pcap`, `/capture.hccapx`, `/eviltwin-log`

**Estratégia escolhida (Opção A — shim de transporte):** introduzir um `transport.js` que
sobrescreve `window.fetch` e `window.XMLHttpRequest` para as URLs do crx3, redirecionando cada
chamada para um **comando serial** e devolvendo uma resposta com o **mesmo payload binário/texto**
que o HTTP devolvia. Assim o `app.js` roda praticamente **sem alteração** (só ganha um botão
"Conectar" e a inclusão do `transport.js` antes dele). Downloads viram `Blob` gerado a partir
do payload serial.

> Alternativa rejeitada (Opção B): forkar o `app.js` trocando cada chamada por comando serial.
> Mais explícito, porém duplica lógica e contraria o pedido de "reaproveitar a interface".

---

## 2. Arquitetura em 3 camadas

```
┌─────────────────────────────────────────────┐
│  Web app hospedado (GitHub Pages, HTTPS)     │
│  ├─ index.html / style.css / icons / fonts   │  ← cópia fiel de data/
│  ├─ app.js  (inalterado, lógica de UI)       │
│  └─ transport.js  (NOVO)                      │
│       ├─ shim de fetch + XMLHttpRequest       │
│       ├─ fila de requisições (1 por vez)      │
│       └─ Web Serial (PC) / WebUSB CP210x (Android)
└───────────────┬──────────────────────────────┘
                │  UART 115200 (ou 921600), framed
┌───────────────▼──────────────────────────────┐
│  Firmware — canal "API mode" (NOVO)           │
│  ├─ leitor de linha dedicado (buffer grande)  │
│  ├─ handlers que reusam os builders de payload│
│  │   já existentes no webserver.c             │
│  └─ REPL humano continua disponível           │
└───────────────────────────────────────────────┘
```

---

## 3. Protocolo serial "API mode"

Requisitos: framing robusto, binário seguro, uma requisição por vez, tolerante a logs de boot.

- **Entrada em API mode:** o web app envia uma linha mágica `\n@API1\n`. O firmware troca do
  REPL para o leitor de API e **silencia o ESP_LOG na UART** (`esp_log_set_vprintf` para um sink
  nulo/marcado) para não poluir o canal. Sair com `@REPL`.
- **Requisição:** `@REQ <id> <verb> <arg-base64?>\n`
  - ex.: `@REQ 7 GET /ap-list`
  - ex.: `@REQ 8 POST /run-attack aGV4...`  (corpo em base64)
- **Resposta:** `@RES <id> <status> <len> <base64>\n`
  - `status` = 200/400/500; `len` = bytes do payload decodificado; base64 = corpo.
- **Payloads grandes** (pcap, upload de portal 100 KB): **chunking** —
  `@RESC <id> <seq> <base64>` … `@RESE <id> <status> <total>`; e no upload
  `@REQC <id> <seq> <base64>` … `@REQE <id>`.
- **Base64** evita bytes de controle que quebrariam o leitor de linha.
- O shim **ignora qualquer linha que não comece com `@RES`/`@RESC`/`@RESE`** (logs de boot,
  ruído) → tolerância a lixo no canal.
- **Fila no JS:** só um `@REQ` em voo por vez; o polling de `/status` (1x/seg) entra na fila e
  nunca colide com um `/run-attack`.

---

## 4. Mudanças no firmware

1. **Novo módulo `main/console/serial_api.c`** (ao lado do `serial_console.c`):
   - Leitor de linha próprio na UART0 (não usa linenoise; buffer ~8 KB) para aceitar linhas
     longas e base64.
   - Dispatcher `@REQ verb path` → chama a função de payload correspondente.
   - Encoder/decoder base64 + framing `@RES`.
   - Silenciar/rotear ESP_LOG enquanto em API mode.
2. **Refatorar `webserver.c` para expor os *builders* de payload** como funções reutilizáveis
   (hoje a lógica está dentro dos handlers HTTP). Ex.: `build_ap_list(uint8_t **buf, size_t*)`,
   `build_status(...)`, `get_pcap(...)`, `get_hccapx(...)`, `evil_twin_status_json(...)`,
   `portal_state_json(...)`, `read_eviltwin_log(...)`. Os handlers HTTP e o API mode chamam a
   MESMA função → zero duplicação e paridade garantida.
3. **`/run-attack`:** o API mode recebe os 22 bytes (base64), monta `attack_request_t` e posta
   `WEBSERVER_EVENT_ATTACK_REQUEST` — idêntico ao que o `serial_console.c` já faz. Reuso total.
4. **Manter o rádio + SPIFFS + servidor do portal cativo** para o **Evil Twin** (ver §7 e
   CRÍTICO). Ou seja: o `CONFIG_CRX3_START_WEB_INTERFACE` passa a controlar só a UI do operador,
   mas o **httpd do portal da vítima** (rotas `/`, `/devil_twin/*`, captura de senha) precisa
   subir quando o Evil Twin roda.
5. **Settings + reboot:** comando que grava SSID/senha no NVS e reinicia (reusa
   `save_settings` já existente).
6. **Upload de portal custom:** receber em chunks base64 e gravar em SPIFFS (reusa a rota atual).
7. **Baud:** subir para **921600** (`CONFIG_ESP_CONSOLE_UART_BAUDRATE`) para acelerar pcap e
   upload (100 KB caem de ~9 s para ~1,3 s). O REPL humano continua funcionando no mesmo baud.

---

## 5. Mudanças no web app (mínimas)

1. **Copiar** `data/` (index.html, app.js, style.css, icons/, fonts/) para a pasta hospedada
   `usb/` (ou `docs/app/`). Caminhos absolutos `/style.css`, `/app.js`, `/icons/*`, `/fonts/*`
   viram relativos à base do Pages.
2. **Adicionar `transport.js`** (carregado ANTES do `app.js`):
   - Camada de transporte dual: Web Serial (PC) com fallback WebUSB+CP210x (Android) — já
     implementada e testada em `usb/index.html`, reaproveitável.
   - Shim de `window.fetch` e `window.XMLHttpRequest`: intercepta URLs contendo os paths do crx3
     (independente de host `192.168.4.1`), roda o `@REQ` correspondente e devolve
     `Response`/objeto XHR com o corpo (ArrayBuffer p/ binário, text/json p/ o resto).
   - Downloads (`/capture.*`) viram `Blob` + `URL.createObjectURL` disparando o download.
3. **Botão "Conectar"** no topo (gesto de usuário exigido pelas APIs). Enquanto desconectado, a
   UI fica desabilitada.
4. **`app.js` praticamente intacto** — só remover o auto-`init()` no `window.onload` para rodar
   **após** a conexão serial, em vez de na carga da página.

---

## 6. Mapeamento endpoint → comando serial

| Função na UI | HTTP antigo | Comando API | Payload |
|---|---|---|---|
| Escanear redes | `GET /ap-list` | `@REQ GET /ap-list` | 40 bytes/AP (bin) |
| Lançar ataque | `POST /run-attack` | `@REQ POST /run-attack <b64 22B>` | ok/erro |
| Polling status | `GET /status` | `@REQ GET /status` | status bin |
| Parar/limpar | `HEAD /reset` | `@REQ POST /reset` | ok |
| Handshake .pcap | `GET /capture.pcap` | `@REQ GET /capture.pcap` | bin (chunk) |
| Handshake .hccapx | `GET /capture.hccapx` | `@REQ GET /capture.hccapx` | bin |
| Evil Twin status | `GET /evil-twin-status` | `@REQ GET /evil-twin-status` | JSON |
| Estado do portal | `GET /devil_twin/portal-state` | idem | JSON |
| Log de credenciais | `GET /eviltwin-log` | idem | texto |
| Limpar log | `POST /eviltwin-log/clear` | idem | ok |
| Rogue AP custom | `POST /custom-evil-twin` | `@REQ POST /custom-evil-twin <b64>` | ok |
| Upload portal | `POST /devil_twin/upload` | chunked | ok |
| Restaurar portal | `POST /devil_twin/restore-default` | idem | ok |
| Salvar settings | `POST /save_settings` | `@REQ POST /save_settings <b64>` | ok+reboot |

---

## 7. Estabilidade — decisões

- **Fila serial** (1 req/vez) + **id de correlação** → sem respostas trocadas.
- **Timeout + retry** por requisição no shim; `scan` com timeout maior (~9 s).
- **Framing base64 + prefixo `@RES`** → imune a logs e a leituras parciais.
- **ESP_LOG silenciado** em API mode → canal limpo.
- **Reconexão automática:** se o cabo cair (evento `disconnect`), a UI volta pro estado
  "desconectado" sem travar; reconectar restaura tudo.
- **Watchdog de link:** um `@REQ GET /ping` leve a cada X s detecta canal morto.

---

## 8. Fases de implementação

- **Fase 1 — Firmware API mode (base):** `serial_api.c`, framing, `@API1`, base64, `GET /ap-list`
  e `GET /status` e `POST /run-attack` reusando builders. Sobe baud p/ 921600.
  *Verificação:* script serial manda `@REQ GET /ap-list` e confere os 40 bytes/AP.
- **Fase 2 — Shim de transporte:** `transport.js` com Web Serial/WebUSB + override de fetch/XHR.
  Copiar `data/` p/ `usb/`. Fazer Scan + Deauth + polling de status funcionarem na UI real.
  *Verificação:* abrir a UI hospedada, escanear, lançar deauth, ver status atualizar sem cair.
- **Fase 3 — Handshake/PMKID/beacon/ghost + downloads:** pcap/hccapx via chunk → Blob.
  *Verificação:* capturar handshake e baixar o .hccapx pelo navegador.
- **Fase 4 — Evil Twin completo:** subir httpd do portal da vítima quando o ataque roda;
  status, log de credenciais, upload/restore de portal custom, rogue AP custom.
  *Verificação:* rodar Evil Twin, capturar senha de um device de teste, ver no log.
- **Fase 5 — Settings + i18n + polimento:** salvar credenciais/reboot; garantir que i18n,
  popups e slider seguem intactos; reconexão e watchdog.
- **Fase 6 — Deploy:** publicar em GitHub Pages (branch `main`) e validar no Android real.

---

## 9. Testes de verificação

- Cada fase tem um teste serial automatizável (PowerShell abrindo a COM e trocando frames) +
  um teste manual na UI.
- Teste de estresse: rodar Deauth 10 min e confirmar que o canal de status **nunca cai**
  (o ponto-chave de estabilidade vs. o modelo WiFi).

---

## 10. Problemas conhecidos

### Solucionáveis (com trabalho)
- **Throughput a 115200** para pcap/portal 100 KB (~9 s) → resolvido subindo p/ 921600.
- **Linha longa/binário no console** → resolvido com leitor dedicado + base64 (não usa linenoise).
- **Logs poluindo o canal** → resolvido silenciando ESP_LOG em API mode.
- **Refatorar webserver.c** para expor builders → trabalhoso, porém direto.

### (CRÍTICO)

- **(CRÍTICO) iPhone/iPad é impossível.** Nem Web Serial nem WebUSB existem no iOS/Safari.
  Nenhum navegador no iPhone acessa USB serial. Se algum dia precisar de iPhone, a única saída é
  o modelo WiFi antigo ou um app nativo — **não há solução** dentro deste plano. (Você usa
  Android, então não te bloqueia, mas fica registrado.)

- **(CRÍTICO) Evil Twin depende inerentemente de WiFi para a vítima.** O controle do operador
  vai por USB, mas a **vítima só se conecta por WiFi** e o portal cativo **exige um servidor HTTP
  no ESP32** servindo a página. Ou seja: é impossível um Evil Twin "100% sem WiFi" — o rádio e um
  httpd mínimo **têm que** subir durante o ataque. É solucionável mantendo esse httpd do portal,
  mas contraria a ideia de "desligar toda a parte web": a camada web do **lado da vítima** é
  parte indivisível do ataque. Além disso, o **"preview" do portal** que hoje é um link HTTP não
  tem equivalente natural por USB (teria que buscar o HTML por serial e renderizar num Blob).

- **(CRÍTICO / condicional ao aparelho) WebUSB + CP210x no Android pode falhar.** Em alguns
  aparelhos Android o driver de kernel "reivindica" o CP210x e o `claimInterface()` do WebUSB é
  negado — sem fallback, porque o Web Serial ainda não é estável no Chrome Android. Se o **seu**
  aparelho específico travar o CP210x, a interface hospedada por USB **não abrirá nesse device**,
  e não há como contornar pelo navegador (precisaria de app nativo com permissão USB, ou um
  aparelho diferente). Isso não dá pra garantir do nosso lado; só dá pra testar no aparelho real.
  *(Mitigação parcial: se a placa fosse um ESP32-S2/S3 com USB nativo — classe CDC-ACM — o
  WebUSB/Web Serial funcionaria de forma muito mais confiável no Android. O seu DevKit v1 usa
  CP210x, que é o caso mais problemático.)*

- **(CRÍTICO / de projeto) `data/index.html` e `app.js` assumem HTTP em vários pontos** (URLs
  `http://192.168.4.1`, downloads via `<a href>`, `responseType='arraybuffer'`). O shim de
  fetch/XHR cobre a maioria, mas **downloads e algumas suposições de rede podem exigir pequenos
  ajustes pontuais no app.js** — ou seja, "app.js 100% intacto" pode não se sustentar em 2–3
  pontos. Não é bloqueio, mas derruba a promessa de "zero alteração" no reuso.
