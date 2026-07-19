# Plano de implementação — Upload de Captive Portal (Evil Twin) personalizado

> **Público-alvo deste documento:** outra IA/desenvolvedor que vai **executar** a
> implementação. Siga os passos na ordem. Não invente arquivos ou rotas fora do que
> está aqui. Cada passo cita o arquivo, a âncora de contexto (texto a procurar) e o
> código exato a inserir. **Não altere nada que não esteja listado neste plano.**

---

## 0. Contexto e objetivo

O firmware é **ESP-IDF puro** (target `esp32`, testado com ESP-IDF v5.3), build com
`idf.py`. A interface web e as páginas do captive portal são servidas de uma partição
**SPIFFS** (`storage`, montada em `/spiffs`), cuja imagem é gerada a partir da pasta
`data/` no build (`components/webserver/CMakeLists.txt:15`).

Hoje o captive portal do Evil Twin serve um arquivo **fixo**:
`data/devil_twin/index.html` → em runtime `/spiffs/devil_twin/index.html`, carregado por
`load_html_from_spiffs()` no handler `captive_handler` (`main/wifi/attack_eviltwin.c:288`).

**Objetivo:** permitir que o usuário faça upload, pela Web UI de gerência
(`http://192.168.4.1`), de um HTML personalizado que **substitui** esse arquivo, com:

- Teto rígido de **100 KB** (102400 bytes) validado no servidor.
- Gravação **atômica e segura** (nunca deixar o portal corrompido/pela metade).
- Botão de **restaurar o padrão de fábrica**.
- Badge de estado (`Padrão` / `Personalizado`) na UI.

**Princípio-chave — não é preciso tocar em `attack_eviltwin.c`.** O ataque já lê
`/spiffs/devil_twin/index.html`. Como o upload troca o conteúdo desse mesmo arquivo, o
Evil Twin passa a servir a página personalizada automaticamente. "Ligar o portal"
continua sendo o botão de ataque existente (`/run-attack`).

### Fluxo do usuário (visão final)

```
[Settings → card "Custom Captive Portal"]
   → escolhe arquivo .html  → JS valida tamanho no navegador (≤100 KB)
   → clica Upload → POST /devil_twin/upload (corpo = HTML cru)
   → servidor: valida Content-Length ≤100 KB  E  espaço livre no SPIFFS
   → grava em index.html.upload.tmp → confere bytes → rename() atômico
   → grava flag NVS portal_custom=1 → responde OK
   → badge vira "Personalizado" (Preview opcional abre a página ativa)
[Restore Default] → POST /devil_twin/restore-default
   → copia index.default.html → index.html → limpa flag NVS
[Atacar] → aba Attack → /run-attack (JÁ EXISTE, não muda)
   → vítima conecta → vê a página personalizada → credencial capturada
```

---

## 1. Orçamento de espaço (por que 100 KB é seguro)

- Partição `storage`: `0x70000` = **458 752 bytes** (`partitions.csv`).
- Conteúdo atual real de `data/`: **188 051 bytes** (soma dos tamanhos reais dos 45 arquivos).
- Arquivo do portal atual (`data/devil_twin/index.html`): **6 102 bytes**.

Pior caso transitório (re-upload por cima de um custom de 100 KB já instalado, com o
`.tmp` coexistindo com o ativo antigo + a cópia de fábrica):

```
outros assets (188051 − 6102) .......... 181 949
index.default.html (cópia de fábrica) ...   6 102
index.html (ativo antigo, até 100 KB) ... 102 400
index.html.upload.tmp (novo, até 100 KB)  102 400
--------------------------------------------------
pico total ............................. 392 851 bytes  (<< 458 752)
```

Sobra folga, **mas SPIFFS tem overhead por objeto e por bloco**, então o plano NÃO
confia só na conta: o handler de upload faz uma **checagem de espaço livre em runtime**
com `esp_spiffs_info()` e rejeita (HTTP 507) se não houver espaço, além do teto de 100 KB.
Isso torna o teto de 100 KB seguro em todos os cenários.

---

## 2. Constantes e caminhos (padronizar em todo o código)

| Nome | Valor |
|---|---|
| Teto de upload | `102400` bytes (100 KB) |
| Arquivo ativo | `/spiffs/devil_twin/index.html` |
| Arquivo temporário | `/spiffs/devil_twin/index.upload.tmp` |
| Cópia de fábrica | `/spiffs/devil_twin/index.default.html` |
| Namespace NVS | `"storage"` (o mesmo já usado em `webserver.c`) |
| Chave NVS de estado | `"portal_custom"` (u8: 0 = padrão, 1 = personalizado) |
| Rota upload | `POST /devil_twin/upload` |
| Rota restaurar | `POST /devil_twin/restore-default` |
| Rota estado | `GET /devil_twin/portal-state` |

---

## 3. Arquivo 1/4 — `data/devil_twin/index.default.html` (NOVO)

Criar a cópia de fábrica que o "Restaurar Padrão" usa. É simplesmente uma **cópia byte a
byte** do `data/devil_twin/index.html` atual, versionada no repositório.

```bash
cp data/devil_twin/index.html data/devil_twin/index.default.html
```

- Esse arquivo entra na imagem SPIFFS automaticamente (a pasta `data/` inteira é
  empacotada pelo `spiffs_create_partition_image`), então **não precisa mexer no
  CMakeLists**.
- Custo fixo: ~6 KB permanentes na partição. Ele **nunca** é sobrescrito pelo upload
  (o upload só escreve `index.upload.tmp` → `index.html`).

> **Importante:** este arquivo deve ser a cópia do `index.html` **de fábrica** (o
> original do projeto). Gere-o antes de qualquer teste de upload, para que o botão
> "Restaurar Padrão" sempre tenha um alvo válido.

---

## 4. Arquivo 2/4 — `components/webserver/webserver.c`

Três handlers novos + registro de 3 rotas. Todos no **webserver principal** (porta
padrão, `192.168.4.1`), **nunca** no `evil_server` do attack_eviltwin.

### 4.1 — Includes

Confirmar que estes já estão no topo do arquivo (a maioria já está). Garantir:

```c
#include <stdio.h>       // já presente
#include <sys/stat.h>    // já presente
#include "esp_spiffs.h"  // já presente (usado por init_spiffs)
#include "nvs.h"         // já presente
```

Adicionar, logo abaixo dos includes existentes, se ainda não houver `unlink`/`rename`
disponível (fazem parte de `<stdio.h>`/`<unistd.h>`):

```c
#include <unistd.h>
```

### 4.2 — Constantes (logo após `static const char *TAG = "webserver";`)

```c
#define PORTAL_ACTIVE_PATH  "/spiffs/devil_twin/index.html"
#define PORTAL_TMP_PATH     "/spiffs/devil_twin/index.upload.tmp"
#define PORTAL_DEFAULT_PATH "/spiffs/devil_twin/index.default.html"
#define PORTAL_MAX_BYTES    (102400)   /* 100 KB hard ceiling */
```

### 4.3 — Handler de upload (POST)

Inserir junto aos outros POST handlers, **logo após** `uri_log_post_handler`
(por volta da linha 294):

```c
/**
 * @brief Recebe um HTML de captive portal personalizado e o instala de forma atômica.
 *
 * Segurança:
 *  - Rejeita Content-Length > PORTAL_MAX_BYTES (100 KB) antes de ler o corpo.
 *  - Rejeita se o SPIFFS não tiver espaço livre suficiente (HTTP 507).
 *  - Grava num arquivo temporário; só faz rename() por cima do ativo se o total
 *    recebido bater com o Content-Length. Se algo falhar, o portal ativo antigo
 *    permanece intacto (o .tmp órfão é removido).
 */
static esp_err_t uri_portal_upload_handler(httpd_req_t *req) {
    int total = req->content_len;

    if (total <= 0 || total > PORTAL_MAX_BYTES) {
        ESP_LOGW(TAG, "Portal upload rejected: size %d (max %d)", total, PORTAL_MAX_BYTES);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "File exceeds 100 KB limit.");
        return ESP_FAIL;
    }

    /* Checagem de espaço livre real no SPIFFS (com margem de segurança). */
    size_t fs_total = 0, fs_used = 0;
    if (esp_spiffs_info("storage", &fs_total, &fs_used) == ESP_OK) {
        size_t free_bytes = (fs_total > fs_used) ? (fs_total - fs_used) : 0;
        /* Precisamos de espaço para o .tmp coexistir com o ativo atual + folga. */
        if (free_bytes < (size_t)total + 8192) {
            ESP_LOGE(TAG, "Portal upload: insufficient SPIFFS space (free=%u need=%d)",
                     (unsigned)free_bytes, total + 8192);
            httpd_resp_set_status(req, "507 Insufficient Storage");
            httpd_resp_sendstr(req, "Not enough space on device.");
            return ESP_FAIL;
        }
    }

    FILE *f = fopen(PORTAL_TMP_PATH, "w");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open temp file");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = total;
    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f);
            unlink(PORTAL_TMP_PATH);
            ESP_LOGE(TAG, "Portal upload: recv error, aborting");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            fclose(f);
            unlink(PORTAL_TMP_PATH);
            ESP_LOGE(TAG, "Portal upload: write error (disk full?), aborting");
            httpd_resp_set_status(req, "507 Insufficient Storage");
            httpd_resp_sendstr(req, "Write failed (disk full).");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    fclose(f);

    /* Só agora, com o arquivo íntegro, troca o ativo de forma atômica. */
    if (rename(PORTAL_TMP_PATH, PORTAL_ACTIVE_PATH) != 0) {
        unlink(PORTAL_TMP_PATH);
        ESP_LOGE(TAG, "Portal upload: rename() failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Install failed");
        return ESP_FAIL;
    }

    /* Marca estado = personalizado na NVS. */
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "portal_custom", 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Custom captive portal installed (%d bytes)", total);
    return httpd_resp_sendstr(req, "OK");
}
```

> **Nota sobre SPIFFS e `rename()`:** o VFS do SPIFFS no ESP-IDF suporta `rename()`
> sobrescrevendo o destino existente. Caso o destino exista, o `rename` do SPIFFS o
> substitui. Se em algum ambiente o `rename` falhar por o destino já existir, a
> alternativa é `unlink(PORTAL_ACTIVE_PATH)` imediatamente antes do `rename` — porém
> **prefira o `rename` direto** para manter a atomicidade; só adote o `unlink` prévio se
> o teste real mostrar falha de `rename` com destino existente.

### 4.4 — Handler de restaurar padrão (POST)

Inserir logo após o handler de upload:

```c
/** @brief Restaura o captive portal de fábrica copiando index.default.html sobre o ativo. */
static esp_err_t uri_portal_restore_handler(httpd_req_t *req) {
    FILE *src = fopen(PORTAL_DEFAULT_PATH, "r");
    if (src == NULL) {
        ESP_LOGE(TAG, "Restore: default portal file missing");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Default file missing");
        return ESP_FAIL;
    }
    FILE *dst = fopen(PORTAL_ACTIVE_PATH, "w");
    if (dst == NULL) {
        fclose(src);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write active file");
        return ESP_FAIL;
    }

    char buf[1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = false; break; }
    }
    fclose(src);
    fclose(dst);

    if (!ok) {
        ESP_LOGE(TAG, "Restore: copy failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Copy failed");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "portal_custom", 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Captive portal restored to factory default");
    return httpd_resp_sendstr(req, "OK");
}
```

### 4.5 — Handler de estado (GET)

Inserir junto aos GET handlers (ex.: logo após `uri_evil_twin_status_handler`):

```c
/** @brief Informa se o captive portal ativo é o padrão ou um personalizado. */
static esp_err_t uri_portal_state_handler(httpd_req_t *req) {
    uint8_t custom = 0;
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "portal_custom", &custom);   /* se não existir, custom fica 0 */
        nvs_close(nvs);
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"custom\":%s}", custom ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}
```

### 4.6 — Declarar as 3 rotas na tabela de URIs

Junto aos `static httpd_uri_t uri_...` GET (por volta da linha 366, após `uri_evil_status`):

```c
static httpd_uri_t uri_portal_state = { .uri = "/devil_twin/portal-state", .method = HTTP_GET, .handler = uri_portal_state_handler };
```

Junto aos `static httpd_uri_t uri_...` POST (por volta da linha 386, após `uri_save_settings`):

```c
static httpd_uri_t uri_portal_upload  = { .uri = "/devil_twin/upload",          .method = HTTP_POST, .handler = uri_portal_upload_handler };
static httpd_uri_t uri_portal_restore = { .uri = "/devil_twin/restore-default", .method = HTTP_POST, .handler = uri_portal_restore_handler };
```

> **ATENÇÃO À ORDEM DE REGISTRO.** Existe uma rota curinga GET `/devil_twin/*`
> (`uri_dtwin`, `webserver.c:371`). As rotas novas `/devil_twin/portal-state` (GET),
> `/devil_twin/upload` (POST) e `/devil_twin/restore-default` (POST) precisam ser
> registradas **ANTES** de `uri_dtwin` em `webserver_run()`, senão o curinga captura o
> GET `/devil_twin/portal-state` e tenta servi-lo como arquivo. (Os POST não colidem com
> o curinga GET por serem método diferente, mas registre todos antes por consistência.)

### 4.7 — Registrar os handlers em `webserver_run()`

Localize o bloco de `httpd_register_uri_handler(server, &uri_...)`. Registre as novas
rotas **antes** da linha `httpd_register_uri_handler(server, &uri_dtwin);`:

```c
    httpd_register_uri_handler(server, &uri_portal_state);   /* GET  — antes do curinga */
    httpd_register_uri_handler(server, &uri_portal_upload);  /* POST */
    httpd_register_uri_handler(server, &uri_portal_restore); /* POST */

    httpd_register_uri_handler(server, &uri_dtwin);          /* curinga /devil_twin/* (já existe) */
```

### 4.8 — Aumentar o limite de handlers

Em `webserver_run()`, a config tem `config.max_uri_handlers = 30;` (`webserver.c:409`).
Contamos +3 rotas novas. Confirme quantos handlers estão registrados hoje e garanta que
`max_uri_handlers` seja **≥ (total atual + 3)**. Se hoje há folga (30 já cobre), pode
deixar; se estiver justo, aumente para `33`. **Verifique contando os
`httpd_register_uri_handler` no arquivo.**

---

## 5. Arquivo 3/4 — `data/index.html`

Adicionar um card novo na aba **Settings**. A `<section id="tab-settings">` começa em
`data/index.html:421` e termina em `</section>` (linha ~455, logo antes de
`<!-- About -->`). Inserir o bloco abaixo **dentro** dessa section, logo após o card
"Update Credentials" (após o `</div>` que fecha aquele `.card`, e antes do `</section>`):

```html
    <!-- Custom Captive Portal -->
    <div class="info-box info-warn" style="margin-top:20px;">
      <div class="info-label">
        <img src="/icons/tips.png" class="info-label-icon" alt="">
        Custom Captive Portal
      </div>
      Upload your own HTML page (max 100 KB) to replace the Evil Twin captive portal.
      The file must be self-contained (inline CSS/JS, no external files). It keeps the
      form field name <code>password</code> and posts to <code>/submit</code> so
      captured credentials are still logged.
    </div>

    <div class="card">
      <div class="card-header">
        <h3>Portal Page</h3>
        <span id="portal-state-badge" class="badge">…</span>
      </div>
      <div class="card-body">
        <div class="form-row">
          <label class="form-label" for="portal-file">HTML file (.html, ≤100 KB)</label>
          <input type="file" id="portal-file" accept=".html,text/html">
        </div>
        <div style="margin-top:20px; display:flex; gap:8px; flex-wrap:wrap;">
          <button onclick="uploadPortal()" class="btn-primary">
            <img src="/icons/rocket.png" style="width:18px;height:18px;filter:brightness(10);" alt="">
            Upload
          </button>
          <a id="portal-preview-link" class="btn-secondary"
             style="text-decoration:none;display:inline-flex;align-items:center;gap:6px;"
             href="/devil_twin/index.html" target="_blank" rel="noopener">
            <img src="/icons/preview.png" class="btn-icon" alt="">
            Preview
          </a>
          <button onclick="restorePortal()" class="btn-secondary">
            <img src="/icons/restart.png" class="btn-icon" alt="">
            Restore Default
          </button>
        </div>
      </div>
    </div>
```

Todas as classes (`info-box info-warn`, `card`, `card-header`, `badge`, `form-row`,
`form-label`, `btn-primary`, `btn-secondary`, `btn-icon`) **já existem** em
`data/style.css` — não crie CSS novo. Os ícones citados (`tips.png`, `rocket.png`,
`preview.png`, `restart.png`) já existem em `data/icons/`.

> Para carregar o badge de estado ao abrir a aba, veja o passo 6.2 (chamar
> `loadPortalState()` dentro de `switchTab`, no ramo `settings`, ou uma vez no load).

---

## 6. Arquivo 4/4 — `data/app.js`

Adicionar as funções novas. Reaproveitam `showDialog()` / `showError()` já existentes
(`app.js:783`, `:795`).

### 6.1 — Funções de portal (inserir perto de `saveSettings`, ~linha 888)

```js
/* ── Custom Captive Portal ───────────────────────── */
var PORTAL_MAX_BYTES = 102400; /* 100 KB — deve casar com PORTAL_MAX_BYTES no firmware */

function loadPortalState() {
    var badge = document.getElementById("portal-state-badge");
    if (!badge) return;
    fetch('/devil_twin/portal-state')
    .then(function (r) { return r.json(); })
    .then(function (d) {
        badge.textContent = d.custom ? "Custom" : "Default";
    })
    .catch(function () { badge.textContent = "?"; });
}

function uploadPortal() {
    var input = document.getElementById("portal-file");
    if (!input || !input.files || input.files.length === 0) {
        showDialog("Choose an .html file first.");
        return;
    }
    var file = input.files[0];
    if (file.size === 0) { showDialog("File is empty."); return; }
    if (file.size > PORTAL_MAX_BYTES) {
        showDialog("File is " + Math.round(file.size / 1024) + " KB — the limit is 100 KB.");
        return;
    }
    if (!confirm("Replace the Evil Twin captive portal with this file?")) return;

    var reader = new FileReader();
    reader.onload = function () {
        fetch('/devil_twin/upload', {
            method: 'POST',
            headers: { 'Content-Type': 'text/html' },
            body: reader.result
        })
        .then(function (r) {
            if (r.ok) {
                showDialog("Custom portal installed.");
                loadPortalState();
            } else {
                return r.text().then(function (t) {
                    showDialog("Upload failed: " + (t || r.status));
                });
            }
        })
        .catch(function (e) { showError("Network error: " + e); });
    };
    reader.onerror = function () { showError("Could not read the file."); };
    reader.readAsText(file);
}

function restorePortal() {
    if (!confirm("Restore the factory-default captive portal? Your custom page will be replaced.")) return;
    fetch('/devil_twin/restore-default', { method: 'POST' })
    .then(function (r) {
        if (r.ok) { showDialog("Default portal restored."); loadPortalState(); }
        else showDialog("Restore failed.");
    })
    .catch(function (e) { showError("Network error: " + e); });
}
```

### 6.2 — Carregar o estado ao entrar em Settings

Localize `function switchTab(name)` (`app.js:100`). Sem alterar a lógica genérica,
adicione ao final da função (ou onde já houver tratamento por aba) um disparo lazy:

```js
    if (name === "settings") loadPortalState();
```

Se `switchTab` não tiver um ponto óbvio para isso, alternativa mínima: chamar
`loadPortalState()` uma vez no fim do arquivo (no bloco de inicialização), protegido pelo
`if (!badge) return;` que já existe dentro da função.

---

## 7. Ordem de execução recomendada

1. **Passo 3** — criar `data/devil_twin/index.default.html` (cópia do `index.html`).
2. **Passo 4** — editar `components/webserver/webserver.c` (constantes, 3 handlers, 3
   rotas, registro na ordem correta, `max_uri_handlers`).
3. **Passo 5** — editar `data/index.html` (card na aba Settings).
4. **Passo 6** — editar `data/app.js` (funções + disparo no switchTab).
5. **Build:** `idf.py build` (target `esp32`).
6. **Flash (inclui a imagem SPIFFS `storage.bin`, obrigatória):**
   `idf.py -p COMx flash monitor`.

---

## 8. Checklist de verificação (rodar de fato, não só ler)

Conecte no AP de gerência (`http://192.168.4.1`) e valide:

- [ ] Aba **Settings** mostra o card "Custom Captive Portal" e o badge carrega
      (`Default` num dispositivo recém-gravado).
- [ ] **Preview** abre `/devil_twin/index.html` (a página de fábrica).
- [ ] Upload de um HTML pequeno (< 5 KB) → resposta OK, badge vira **Custom**,
      Preview passa a mostrar a nova página.
- [ ] Upload de arquivo **> 100 KB** → bloqueado no navegador (dialog), sem POST.
- [ ] Forçar um POST > 100 KB por fora (ex.: `curl --data-binary @big.html`) →
      servidor responde **413** e o portal ativo **não muda**.
- [ ] Simular queda no meio do upload (cancelar) → o `index.html` ativo antigo continua
      válido; nenhum arquivo pela metade é servido; `index.upload.tmp` não sobra (ou é
      inofensivo).
- [ ] **Restore Default** → badge volta a **Default**, Preview mostra a página de fábrica.
- [ ] **Reboot** o ESP32 → o estado (Custom/Default) persiste (flag NVS) e o
      `index.html` correto é servido.
- [ ] Iniciar o **Evil Twin** (aba Attack → `/run-attack`) com um portal Custom instalado
      → a vítima vê a página personalizada; enviar credencial no campo `password` →
      aparece no log do captive portal (aba Log) como já acontecia. **Confirma que
      `attack_eviltwin.c` não precisou de nenhuma mudança.**

---

## 9. Regras de segurança para quem executa

- **Não** registrar as rotas de upload/restore no `evil_server` do
  `attack_eviltwin.c`. Elas são administrativas e vivem só no webserver principal.
- **Não** remover nem sobrescrever `index.default.html` em runtime — é a rede de
  segurança do "Restaurar Padrão".
- **Não** reduzir o teto de 100 KB nem remover a checagem `esp_spiffs_info()`; as duas
  camadas juntas é que garantem que a partição nunca estoure.
- Manter a **gravação atômica** (`.tmp` → `rename`). Nunca escrever direto em
  `index.html` durante o recebimento.
- **Não** alterar nada fora dos 4 arquivos listados. Se o build acusar
  `max_uri_handlers` estourado, ajuste apenas esse número.
- O conteúdo enviado pelo usuário é servido **cru** para a vítima (é essa a intenção do
  recurso). Não há sanitização de HTML — isto é um projeto de segurança ofensiva para
  **uso autorizado**; deixe claro na UI (já coberto pelo texto do card) que o arquivo é
  responsabilidade do operador.

---

## 10. Resumo dos arquivos tocados

| Arquivo | Ação |
|---|---|
| `data/devil_twin/index.default.html` | **NOVO** — cópia de fábrica do portal |
| `components/webserver/webserver.c` | 4 constantes, 3 handlers, 3 rotas, registro, `max_uri_handlers` |
| `data/index.html` | 1 card novo na aba Settings |
| `data/app.js` | 3 funções + 1 linha no `switchTab` |

Nenhum outro arquivo deve ser modificado. Em particular, **`main/wifi/attack_eviltwin.c`
permanece intacto** — é o ponto central do design.
