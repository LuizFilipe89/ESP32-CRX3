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

/* ─────────────────────────── CP210x WebUSB driver (Android) ─────────────── */
const CP210X = { IFC_ENABLE: 0x00, SET_LINE_CTL: 0x03, SET_MHS: 0x07, SET_BAUDRATE: 0x1E };

class Cp210xPort {
  constructor(device) { this.device = device; this.epIn = 0; this.epOut = 0; this.ifNum = 0; }
  async _ctrl(request, value, data) {
    const setup = { requestType: "vendor", recipient: "interface", request, value, index: this.ifNum };
    return this.device.controlTransferOut(setup, data || new Uint8Array(0));
  }
  async open(baud) {
    await this.device.open();
    if (this.device.configuration === null) await this.device.selectConfiguration(1);
    const cfg = this.device.configuration;
    const iface = cfg.interfaces.find(i => i.alternate.endpoints.some(e => e.type === "bulk")) || cfg.interfaces[0];
    this.ifNum = iface.interfaceNumber;
    await this.device.claimInterface(this.ifNum);
    const eps = iface.alternate.endpoints;
    this.epIn = eps.find(e => e.direction === "in" && e.type === "bulk").endpointNumber;
    this.epOut = eps.find(e => e.direction === "out" && e.type === "bulk").endpointNumber;
    await this._ctrl(CP210X.IFC_ENABLE, 0x0001);
    await this._ctrl(CP210X.SET_BAUDRATE, 0x0000, new Uint8Array([baud & 0xff, (baud >> 8) & 0xff, (baud >> 16) & 0xff, (baud >> 24) & 0xff]));
    await this._ctrl(CP210X.SET_LINE_CTL, 0x0800);   /* 8N1 */
    await this._ctrl(CP210X.SET_MHS, 0x0300);        /* DTR=0 RTS=0 */
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
    if ("serial" in navigator) {
      const p = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x10C4 }] });
      this.port = new WebSerialPort(p); this.transport = "serial";
    } else if ("usb" in navigator) {
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
      } catch (e) { break; }
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
  try {
    const res = await con.apiRequest(method, path, body, path === "/ap-list" ? 20000 : 5000);
    return new Response(res.bytes, { status: res.status || 200, headers: { "Content-Type": "application/octet-stream" } });
  } catch (e) {
    console.error("[crx3 usb] fetch " + method + " " + path + " failed:", e);
    return new Response("", { status: 504 });
  }
}

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
