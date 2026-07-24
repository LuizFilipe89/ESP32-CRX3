/* ═══════════════════════════════════════════════════════════════════════════
 *  crx3 USB transport shim
 *  Makes the existing web UI (index.html + app.js) talk to the ESP32 over USB
 *  serial instead of Wi-Fi HTTP. It:
 *    1. connects via Web Serial (desktop) or WebUSB + CP210x driver (Android),
 *    2. speaks the firmware "API mode" framing (api <id> <VERB> <path> [b64] →
 *       @RES <id> <status> <len> <base64>), reusing the same binary payloads the
 *       old HTTP endpoints returned,
 *    3. overrides window.fetch / window.XMLHttpRequest so app.js runs unchanged,
 *    4. shows a Connect gate and runs the app's init() once connected.
 *  Loaded BEFORE app.js.
 * ═══════════════════════════════════════════════════════════════════════════ */
"use strict";

/* Tell app.js not to fire its network init() on load — we call it after connect. */
window.CRX3_DEFER_INIT = true;

const enc = new TextEncoder();
const dec = new TextDecoder();
const sleep = ms => new Promise(r => setTimeout(r, ms));

function b64enc(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return btoa(s); }
function b64dec(b64) { const s = atob(b64); const u8 = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u8[i] = s.charCodeAt(i); return u8; }

/* ─────────────────────────── CP210x WebUSB driver (Android) ───────────────
 * Ported from Jason2866/esp32tool's js/webusb-serial.js (proven working on
 * Android with real CP2102 hardware). Two bugs in the original hand-rolled
 * version this replaced: (1) all vendor control transfers used
 * recipient:"interface" — CP210x expects recipient:"device" for everything
 * except the baudrate request; (2) DTR/RTS were deasserted on open, but the
 * chip's own init sequence expects them asserted (1/1) with the mask bits set. */
class Cp210xPort {
  constructor(device) { this.device = device; this.epIn = 0; this.epOut = 0; this.ifNum = 0; }

  async open(baud) {
    if (this.device.opened) { try { await this.device.close(); } catch (e) {} }
    try { if (this.device.reset) await this.device.reset(); } catch (e) {}

    await this.device.open();
    if (!this.device.configuration || this.device.configuration.configurationValue !== 1) {
      await this.device.selectConfiguration(1);
    }
    const cfg = this.device.configuration;

    /* Pick the interface that actually has bulk in+out endpoints (prefer
     * vendor-specific class 0xFF, which is what CP210x reports). */
    let chosen = null;
    for (const iface of cfg.interfaces) {
      for (let alt = 0; alt < iface.alternates.length; alt++) {
        const a = iface.alternates[alt];
        const hasIn  = a.endpoints.some(e => e.type === "bulk" && e.direction === "in");
        const hasOut = a.endpoints.some(e => e.type === "bulk" && e.direction === "out");
        if (hasIn && hasOut) { chosen = { iface, alt, a }; break; }
      }
      if (chosen) break;
    }
    if (!chosen) throw new Error("Nenhuma interface USB com endpoints bulk encontrada");

    await this.device.claimInterface(chosen.iface.interfaceNumber);
    try { await this.device.selectAlternateInterface(chosen.iface.interfaceNumber, chosen.alt); } catch (e) {}
    this.ifNum = chosen.iface.interfaceNumber;
    this.epIn  = chosen.a.endpoints.find(e => e.type === "bulk" && e.direction === "in").endpointNumber;
    this.epOut = chosen.a.endpoints.find(e => e.type === "bulk" && e.direction === "out").endpointNumber;

    /* CP210x init sequence — exact order and recipients matter. */
    await this.device.controlTransferOut({ requestType: "vendor", recipient: "device", request: 0x00, value: 0x01, index: 0x00 }); // IFC_ENABLE
    await this.device.controlTransferOut({ requestType: "vendor", recipient: "device", request: 0x03, value: 0x0800, index: 0x00 }); // SET_LINE_CTL 8N1
    await this.device.controlTransferOut({ requestType: "vendor", recipient: "device", request: 0x07, value: 0x03 | 0x0100 | 0x0200, index: 0x00 }); // SET_MHS DTR=1 RTS=1 + masks
    const baudBuf = new ArrayBuffer(4);
    new DataView(baudBuf).setUint32(0, baud, true);
    await this.device.controlTransferOut({ requestType: "vendor", recipient: "interface", request: 0x1E, value: 0, index: 0 }, baudBuf); // IFC_SET_BAUDRATE
  }

  async write(bytes) { await this.device.transferOut(this.epOut, bytes); }
  async read() {
    const r = await this.device.transferIn(this.epIn, 64);
    if (r.status === "stall") { await this.device.clearHalt("in", this.epIn); return new Uint8Array(0); }
    if (!r.data || !r.data.byteLength) return new Uint8Array(0);
    return new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
  }
  async close() { try { await this.device.releaseInterface(this.ifNum); } catch (e) {} try { await this.device.close(); } catch (e) {} }
}

/* ─────────────────────────── Web Serial driver (desktop) ────────────────── */
class WebSerialPort {
  constructor(port) { this.port = port; this.reader = null; this.writer = null; }
  async open(baud) {
    await this.port.open({ baudRate: baud, dataBits: 8, stopBits: 1, parity: "none" });
    /* Opening a COM port on Windows commonly asserts DTR, and on CP210x-based
     * ESP32 boards DTR/RTS drive the auto-reset circuit (same lines esptool
     * uses to reset into bootloader). Clear both immediately so opening the
     * port doesn't keep the board held in reset. */
    try { await this.port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch (e) {}
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
  }
  async write(bytes) { await this.writer.write(bytes); }
  async read() { const { value, done } = await this.reader.read(); return (done || !value) ? new Uint8Array(0) : value; }
  async close() {
    try { await this.reader.cancel(); } catch (e) {}
    try { this.reader.releaseLock(); } catch (e) {}
    try { this.writer.releaseLock(); } catch (e) {}
    try { await this.port.close(); } catch (e) {}
  }
}

/* ─────────────────────────── API-mode console ───────────────────────────── */
class Console {
  constructor() { this.port = null; this.rx = ""; this.connected = false; this.transport = ""; this._id = 0; this._chain = Promise.resolve(); }

  async connect(onStatus) {
    /* Android's Web Serial implementation (when present at all) does not
     * reliably talk to CP210x vendor-specific USB-serial chips — confirmed
     * against Jason2866/esp32tool, a tool proven to work on this exact board
     * on Android, which explicitly prefers WebUSB there and skips
     * navigator.serial entirely. Desktop Chrome/Edge's Web Serial handles
     * CP210x fine, so keep that path for desktop. */
    const isAndroid = /Android/i.test(navigator.userAgent);
    const hasSerial = "serial" in navigator;
    const hasUsb = "usb" in navigator;

    if (isAndroid && hasUsb) {
      const d = await navigator.usb.requestDevice({ filters: [{ vendorId: 0x10C4 }] });
      this.port = new Cp210xPort(d); this.transport = "usb";
    } else if (hasSerial) {
      /* No vendorId filter: some Android Web Serial stacks report the CP210x
       * with different descriptor details than desktop, so a strict filter
       * can hide it from the picker entirely. Let the user pick manually. */
      const p = await navigator.serial.requestPort();
      this.port = new WebSerialPort(p); this.transport = "serial";
    } else if (hasUsb) {
      const d = await navigator.usb.requestDevice({ filters: [{ vendorId: 0x10C4 }] });
      this.port = new Cp210xPort(d); this.transport = "usb";
    } else {
      throw new Error("navegador sem Web Serial nem WebUSB");
    }
    await this.port.open(115200);
    this.connected = true;
    this.rx = "";
    this._readLoop();

    /* Opening the port very likely reset the board (DTR pulse on the CP210x
     * auto-reset circuit), so the firmware is mid-boot for a couple seconds
     * and won't answer yet. Poll /ping with retries instead of a fixed sleep
     * — much more reliable than guessing a boot delay. */
    if (onStatus) onStatus("aguardando o firmware iniciar…");
    const deadline = Date.now() + 12000;
    let lastErr = null;
    while (Date.now() < deadline) {
      try {
        const res = await this._doRequest("GET", "/ping", null, 1500);
        if (res && res.status === 200) return;
      } catch (e) { lastErr = e; }
      await sleep(300);
    }
    this.connected = false;
    throw new Error("dispositivo não respondeu após reconectar (" + (lastErr ? lastErr.message : "sem resposta") + ")");
  }

  async _readLoop() {
    while (this.connected) {
      try {
        const bytes = await this.port.read();
        if (bytes && bytes.length) {
          this.rx += dec.decode(bytes, { stream: true });
          if (this.rx.length > 200000) this.rx = this.rx.slice(-100000);
        }
      } catch (e) {
        /* WebUSB on Android throws transient "transfer error has occurred" /
         * "transfer was cancelled" under bus load — well documented in
         * Jason2866/esp32tool, which retries instead of giving up. Large
         * responses (e.g. /ap-list, many back-to-back 64-byte reads) are
         * exactly when this hits. Previously any exception here did `break`,
         * silently killing the read side while `connected` stayed true — the
         * socket looked alive but never received another byte, so every
         * later request just timed out. Only stop on errors that mean the
         * device is actually gone; retry everything else. */
        const msg = (e && e.message) || "";
        const fatal = msg.indexOf("device unavailable") !== -1 ||
                      msg.indexOf("device has been lost") !== -1 ||
                      msg.indexOf("device was disconnected") !== -1 ||
                      msg.indexOf("No device selected") !== -1;
        if (fatal) break;
        await sleep(10);
      }
    }
  }

  /* One request at a time (queued); returns {status, bytes}. */
  apiRequest(verb, path, bodyU8, timeout) {
    const run = () => this._doRequest(verb, path, bodyU8, timeout || 4000);
    const p = this._chain.then(run, run);
    this._chain = p.catch(() => {});
    return p;
  }

  async _doRequest(verb, path, bodyU8, timeout) {
    if (!this.connected) throw new Error("desconectado");
    const id = String(++this._id);
    let cmd = "api " + id + " " + verb + " " + path;
    if (bodyU8 && bodyU8.length) cmd += " " + b64enc(bodyU8);
    const start = this.rx.length;
    await this.port.write(enc.encode(cmd + "\r\n"));
    /* Require a trailing line break after the base64 token — long payloads
     * (e.g. ap-list) arrive over several USB/serial reads, and without this
     * anchor the regex would happily match a still-partial, mid-flight
     * base64 string (truncated \S* on whatever has landed so far), which
     * later fails to decode (atob "not correctly encoded"). Waiting for
     * \r?\n guarantees the full line — and thus the full payload — is in. */
    const re = new RegExp("@RES\\s+" + id + "\\s+(\\d+)\\s+(\\d+)\\s*(\\S*)\\r?\\n");
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      const m = this.rx.slice(start).match(re);
      if (m) {
        const status = parseInt(m[1], 10);
        const len = parseInt(m[2], 10);
        let bytes = new Uint8Array(0);
        if (len > 0 && m[3]) {
          try { bytes = b64dec(m[3]); }
          catch (e) { await sleep(25); continue; /* still mid-flight somehow; keep waiting */ }
        }
        return { status, bytes };
      }
      await sleep(25);
    }
    throw new Error("timeout: " + verb + " " + path);
  }

  async disconnect() { this.connected = false; await sleep(60); if (this.port) await this.port.close(); this.port = null; }
}

const con = new Console();
window.crx3Serial = con;   /* expose for debugging */

/* ─────────────────────────── Endpoint routing ───────────────────────────── */
const CRX3_PATHS = ["/ap-list", "/status", "/run-attack", "/reset", "/stop", "/evil-twin-status",
  "/devil_twin", "/eviltwin-log", "/save_settings", "/custom-evil-twin", "/capture.",
  "/get-log-url", "/set-log-url", "/printer", "/detector"];

function pathOf(u) { try { return new URL(u, location.href).pathname; } catch (e) { return String(u); } }
function isCrx3(url) {
  const s = String(url);
  if (s.indexOf("192.168.4.1") !== -1) return true;
  const p = pathOf(url);
  return CRX3_PATHS.some(x => p.indexOf(x) !== -1);
}

/* ─────────────────────────── fetch() shim ───────────────────────────────── */
const origFetch = window.fetch ? window.fetch.bind(window) : null;
window.fetch = function (input, opts) {
  const url = (typeof input === "string") ? input : (input && input.url) || String(input);
  if (!isCrx3(url)) return origFetch ? origFetch(input, opts) : Promise.reject(new Error("no fetch"));
  return crx3Fetch(pathOf(url), opts || {});
};

async function crx3Fetch(path, opts) {
  if (!con.connected) return new Response("", { status: 503 });
  const method = (opts.method || "GET").toUpperCase();
  let body = null;
  if (opts.body != null) {
    if (typeof opts.body === "string") body = enc.encode(opts.body);
    else if (opts.body instanceof ArrayBuffer) body = new Uint8Array(opts.body);
    else if (opts.body instanceof Uint8Array) body = opts.body;
    else body = enc.encode(String(opts.body));
  }
  /* The custom captive portal (up to 100 KB) doesn't fit on one console line
   * — split it into small chunked requests instead. app.js is untouched: it
   * still calls a single fetch('/devil_twin/upload', {body: <whole file>}). */
  if (method === "POST" && path === "/devil_twin/upload" && body) {
    return crx3ChunkedUpload(body);
  }
  try {
    const res = await con.apiRequest(method, path, body, path === "/ap-list" ? 20000 : 5000);
    return new Response(res.bytes, { status: res.status || 200, headers: { "Content-Type": "application/octet-stream" } });
  } catch (e) {
    console.error("[crx3 usb] fetch " + method + " " + path + " failed:", e);
    return new Response("", { status: 504 });
  }
}

const CRX3_UPLOAD_CHUNK = 600;   /* raw bytes/chunk — keeps each console line well under max_cmdline_length */
async function crx3ChunkedUpload(bytes) {
  try {
    const startRes = await con.apiRequest("POST", "/devil_twin/upload/start", enc.encode(String(bytes.length)), 5000);
    if (startRes.status !== 200) return new Response(startRes.bytes, { status: startRes.status });
    for (let off = 0; off < bytes.length; off += CRX3_UPLOAD_CHUNK) {
      const chunk = bytes.subarray(off, off + CRX3_UPLOAD_CHUNK);
      const r = await con.apiRequest("POST", "/devil_twin/upload/chunk", chunk, 5000);
      if (r.status !== 200) return new Response(r.bytes, { status: r.status });
    }
    const finRes = await con.apiRequest("POST", "/devil_twin/upload/finish", null, 8000);
    return new Response(finRes.bytes, { status: finRes.status || 200 });
  } catch (e) {
    console.error("[crx3 usb] chunked upload failed:", e);
    return new Response("", { status: 504 });
  }
}

/* ─────────────────────────── Blob downloads ──────────────────────────────
 * Over WiFi these were plain <a href download> links to a real HTTP server.
 * Over USB there's no HTTP server to navigate to, so fetch the bytes through
 * the same serial API and trigger the download via a Blob object URL. */
async function crx3Download(path, filename) {
  if (!con.connected) { alert("não conectado"); return; }
  try {
    const res = await con.apiRequest("GET", path, null, 20000);
    if (res.status !== 200) { alert("Falha ao baixar (" + res.status + ")"); return; }
    const blob = new Blob([res.bytes], { type: "application/octet-stream" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url; a.download = filename || "download";
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 30000);
  } catch (e) {
    alert("Falha ao baixar: " + e.message);
  }
}
window.crx3Download = crx3Download;

async function crx3Preview(path) {
  if (!con.connected) { alert("não conectado"); return; }
  try {
    const res = await con.apiRequest("GET", path, null, 8000);
    if (res.status !== 200) { alert("Falha ao carregar preview (" + res.status + ")"); return; }
    const blob = new Blob([res.bytes], { type: "text/html" });
    const url = URL.createObjectURL(blob);
    window.open(url, "_blank");
  } catch (e) {
    alert("Falha: " + e.message);
  }
}
window.crx3Preview = crx3Preview;

/* ─────────────────────────── XMLHttpRequest shim ────────────────────────── */
const OrigXHR = window.XMLHttpRequest;

function Crx3XHR() {
  this.responseType = ""; this.timeout = 0; this.status = 0; this.response = null; this.responseText = "";
  this.readyState = 0; this.onload = null; this.onerror = null; this.ontimeout = null; this.onreadystatechange = null;
  this._m = ""; this._u = ""; this._real = null;
}
Crx3XHR.prototype.open = function (method, url) {
  this._m = (method || "GET").toUpperCase(); this._u = url;
  if (!isCrx3(url)) { this._real = new OrigXHR(); this._real.open(method, url); }
};
Crx3XHR.prototype.setRequestHeader = function (k, v) { if (this._real) this._real.setRequestHeader(k, v); };
Crx3XHR.prototype.getAllResponseHeaders = function () { return this._real ? this._real.getAllResponseHeaders() : ""; };
Crx3XHR.prototype.abort = function () { if (this._real) this._real.abort(); };
Crx3XHR.prototype.addEventListener = function (type, fn) {
  if (type === "load") this.onload = fn; else if (type === "error") this.onerror = fn; else if (type === "timeout") this.ontimeout = fn;
  if (this._real) this._real.addEventListener(type, fn);
};
Crx3XHR.prototype.send = function (body) {
  if (this._real) {
    const self = this, r = this._real;
    r.responseType = this.responseType; r.timeout = this.timeout;
    r.onload = function () { self.status = r.status; self.response = r.response; self.responseText = r.responseText; self.readyState = 4; if (self.onreadystatechange) self.onreadystatechange(); if (self.onload) self.onload(); };
    r.onerror = function () { if (self.onerror) self.onerror(); };
    r.ontimeout = function () { if (self.ontimeout) self.ontimeout(); };
    r.send(body); return;
  }
  doCrx3XHR(this, body);
};

async function doCrx3XHR(xhr, body) {
  const path = pathOf(xhr._u);
  try {
    if (!con.connected) throw new Error("desconectado");
    let res;
    if (xhr._m === "GET" && path === "/ap-list") res = await con.apiRequest("GET", "/ap-list", null, 20000);
    else if (xhr._m === "GET" && path === "/status") res = await con.apiRequest("GET", "/status", null, 4000);
    else if (xhr._m === "POST" && path === "/run-attack") {
      const u8 = body instanceof ArrayBuffer ? new Uint8Array(body) : (body instanceof Uint8Array ? body : enc.encode(String(body || "")));
      res = await con.apiRequest("POST", "/run-attack", u8, 4000);
    } else if (path === "/reset") res = await con.apiRequest("POST", "/reset", null, 3000);
    else res = { status: 404, bytes: new Uint8Array(0) };

    xhr.status = res.status || 200;
    if (xhr.responseType === "arraybuffer") xhr.response = res.bytes.buffer;
    else { xhr.responseText = dec.decode(res.bytes); xhr.response = xhr.responseText; }
    xhr.readyState = 4;
    if (xhr.onreadystatechange) xhr.onreadystatechange();
    if (xhr.onload) xhr.onload();
  } catch (e) {
    console.error("[crx3 usb] xhr " + xhr._m + " " + path + " failed:", e);
    xhr.status = 0; xhr.readyState = 4;
    if (xhr.onerror) xhr.onerror();
  }
}
window.XMLHttpRequest = Crx3XHR;

/* ─────────────────────────── Connect gate UI ────────────────────────────── */
function buildConnectUI() {
  if (document.getElementById("usb-connect-overlay")) return;
  const ov = document.createElement("div");
  ov.id = "usb-connect-overlay";
  ov.style.cssText = "position:fixed;inset:0;z-index:99999;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,.9);backdrop-filter:blur(4px);";
  ov.innerHTML =
    '<div style="text-align:center;font-family:ui-monospace,\'Cascadia Code\',Consolas,monospace;color:#35F58B;padding:24px;max-width:440px;">' +
    '<div style="font-size:22px;font-weight:700;letter-spacing:1px;margin-bottom:6px;">crx3 · USB</div>' +
    '<div style="color:#7FBF97;font-size:13px;margin-bottom:22px;">Conecte a placa no cabo USB e toque para começar.</div>' +
    '<button id="usb-connect-btn" style="font-family:inherit;font-size:15px;background:#35F58B;color:#00230F;border:1px solid #35F58B;padding:12px 22px;font-weight:700;cursor:pointer;">Conectar ao ESP32 (USB)</button>' +
    '<div id="usb-connect-msg" style="color:#FFB4AB;font-size:12px;margin-top:16px;white-space:pre-wrap;min-height:1.2em;"></div>' +
    '<div style="color:#7FBF97;font-size:11px;margin-top:18px;line-height:1.5;">PC: Chrome/Edge (Web Serial) · Android: Chrome via OTG (WebUSB) · iPhone não suporta.<br>Feche qualquer monitor serial usando a porta antes.</div>' +
    '</div>';
  document.body.appendChild(ov);
  document.getElementById("usb-connect-btn").onclick = doConnect;
  if (!("serial" in navigator) && !("usb" in navigator)) {
    document.getElementById("usb-connect-msg").textContent = "Este navegador não suporta Web Serial nem WebUSB. Use Chrome ou Edge.";
  }
}

async function doConnect() {
  const btn = document.getElementById("usb-connect-btn"), msg = document.getElementById("usb-connect-msg");
  btn.disabled = true; msg.textContent = "conectando…";
  try {
    await con.connect(function (s) { msg.textContent = s; });
    const ov = document.getElementById("usb-connect-overlay");
    if (ov) ov.remove();
    if (typeof window.init === "function") window.init();
  } catch (e) {
    console.error("[crx3 usb] connect failed:", e);
    btn.disabled = false;
    msg.textContent = "Falha: " + e.message + "\nDesconecte e reconecte o cabo, então tente de novo.";
  }
}

function onUnplug() {
  if (con.connected) { con.connected = false; con.port = null; }
  buildConnectUI();
  const m = document.getElementById("usb-connect-msg");
  if (m) m.textContent = "cabo removido — reconecte.";
}
if (navigator.usb) navigator.usb.addEventListener("disconnect", onUnplug);
if (navigator.serial) navigator.serial.addEventListener("disconnect", onUnplug);

/* DOM is already parsed (this script sits at the end of <body>). */
buildConnectUI();
