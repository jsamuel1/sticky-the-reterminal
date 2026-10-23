// devbench page logic. Runs in the OWNER's browser; talks to the board with Web
// Serial and reports to the agent's server with relative fetches (works under
// any reverse-proxy prefix). Probe/boot/dump are read-only; Flash writes, and
// only after an explicit confirm naming board, image set and erase mode.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.0.bundle.js";
import { md5hex } from "./md5.js";
import { boardName, softapMac, buildCfg, tinyNvs, readNvsStrings, NVS_OFFSET, NVS_SIZE, showCode, redact } from "./provision.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const session = `s${Date.now().toString(36)}`;
let port = null;          // SerialPort
let report = null;        // last assembled report
let monitor = null;       // { reader, abort }
let logBuf = "";          // text not yet shipped to /api/log
const dec = new TextDecoder();

function log(s, cls) {
  const line = document.createElement("span");
  if (cls) line.className = cls;
  line.textContent = s.endsWith("\n") ? s : s + "\n";
  logEl.append(line);
  logEl.scrollTop = logEl.scrollHeight;
  logBuf += line.textContent;
}
setInterval(flushLog, 1000);
// Last-chance flush when the pane reloads or closes; beacons can't carry headers.
addEventListener("pagehide", () => {
  if (!logBuf) return;
  navigator.sendBeacon(`api/log?s=${session}&l=${encodeURIComponent($("label").value)}`, new Blob([logBuf], { type: "text/plain" }));
  logBuf = "";
});
async function flushLog() {
  if (!logBuf || flushing) return;
  const body = logBuf; logBuf = ""; flushing = true;
  try {
    const r = await send("log", body, { query: { s: session, l: $("label").value } });
    if (!r.ok && r.status !== 413) logBuf = body + logBuf;  // 413 = rejected, don't retry forever
  } catch { logBuf = body + logBuf; } finally { flushing = false; }
}
let flushing = false;

// ---- talking to the bench server --------------------------------------------------
// Reached through the Kermes browser pane over the AWS tunnel, every POST with a
// body answers 502 "browser upstream unreachable": the tunnel edge re-sends the
// body chunked and the pane proxy forwards Transfer-Encoding into undici fetch,
// which refuses it. That is why job results and console logs stopped arriving
// on 10-18. send() tries the POST once; on that 502 (or a network error) it
// switches to GET api/x/<path>?b=<base64url piece>, which carries no body, for
// the rest of the session. Dumps still need a real POST (MiB-sized chunks).
let postBroken = false;
const X_PIECE = 6000;  // base64url chars per GET; the proxy took 12 KB URLs in testing
const b64url = (bytes) => {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
};
async function send(path, body, { query = {} } = {}) {
  const bytes = body instanceof Uint8Array ? body : new TextEncoder().encode(typeof body === "string" ? body : JSON.stringify(body));
  const qs = new URLSearchParams(query).toString();
  if (!postBroken) {
    try {
      const r = await fetch(`api/${path}${qs ? "?" + qs : ""}`, { method: "POST", headers: { "X-Devbench": "1" }, body: bytes });
      if (r.status !== 502 || !/browser upstream unreachable/.test(await r.clone().text())) return r;
    } catch { /* network error: try the GET channel */ }
    postBroken = true;
    log("POST bodies are not reaching the bench server (pane proxy over the tunnel); switching to the GET upload channel", "warn");
  }
  const enc = b64url(bytes), n = Math.max(1, Math.ceil(enc.length / X_PIECE));
  const u = `${session}-${Math.random().toString(36).slice(2, 10)}`;
  let r;
  for (let i = 0; i < n; i++) {
    const q = new URLSearchParams({ ...query, u, i, n, b: enc.slice(i * X_PIECE, (i + 1) * X_PIECE) });
    r = await fetch(`api/x/api/${path}?${q}`, { headers: { "X-Devbench": "1" } });
    if (i < n - 1 && r.status !== 202) return r;
  }
  return r;
}
async function sendJson(path, body) {
  const r = await send(path, body);
  let j = {};
  try { j = await r.json(); } catch { j = { error: `${r.status} (not JSON)` }; }
  return { ok: r.ok, status: r.status, j };
}

// ---- environment ----------------------------------------------------------
function envCheck() {
  const env = $("env");
  if (!("serial" in navigator)) {
    env.className = "bad";
    env.innerHTML = "<b>Web Serial unavailable.</b> Use desktop Chrome or Edge. Safari and Firefox do not support it" +
      (window.isSecureContext ? "." : "; this page is also not a secure context (needs https or 127.0.0.1).") +
      (window.top !== window ? " If you are in a dashboard pane, the embedding frame may be blocking the <code>serial</code> permission; open this URL in its own tab." : "");
    return false;
  }
  env.innerHTML = `<span class="ok">Web Serial available.</span> <span class="muted">session ${session} · ${window.top !== window ? "embedded" : "top-level"} · ${navigator.userAgent.match(/(Chrome|Edg)\/[\d.]+/)?.[0] ?? ""}</span>`;
  return true;
}

function setBusy(b) {
  busy = b;
  for (const id of ["probe", "boot", "mon", "rst", "forget", "dump", "flash", "plan"]) $(id).disabled = b || !port;
  $("pick").disabled = b;
  $("send").disabled = b || !report;
  $("line").disabled = !monitor;
  for (const el of document.querySelectorAll("#jobs button")) el.disabled = b || !port;
  for (const el of document.querySelectorAll("#modes button")) el.disabled = b;  // no switching mid-operation
  $("modes").hidden = !port;
  $("empty").hidden = !!port;
  if (!port) for (const p of document.querySelectorAll(".panel")) p.classList.remove("on");
  else if (!document.querySelector(".panel.on")) setMode(localStorage.devbenchMode || "console");
}
let busy = false;

// ---- modes: one task's controls at a time ------------------------------------
function setMode(m) {
  if (!document.querySelector(`.panel[data-panel="${m}"]`)) m = "console";
  for (const b of document.querySelectorAll("#modes button")) b.setAttribute("aria-selected", String(b.dataset.mode === m));
  for (const p of document.querySelectorAll(".panel")) p.classList.toggle("on", p.dataset.panel === m);
  localStorage.devbenchMode = m;
  if (m === "flash") loadSets();
}
for (const b of document.querySelectorAll("#modes button")) b.onclick = async () => {
  if (b.dataset.mode !== "console" && monitor) { await stopMonitor(); $("mon").textContent = "Open monitor (115200)"; }
  setMode(b.dataset.mode);
};

function portLabel(p) {
  const i = p.getInfo();
  const hex = (n) => (n == null ? "?" : n.toString(16).padStart(4, "0"));
  const kind = i.usbVendorId === 0x303a ? (i.usbProductId === 0x1001 ? "ESP USB-Serial/JTAG" : "Espressif USB")
    : i.usbVendorId === 0x1a86 ? "WCH CH34x" : i.usbVendorId === 0x10c4 ? "SiLabs CP210x" : "USB serial";
  return `${kind} ${hex(i.usbVendorId)}:${hex(i.usbProductId)}`;
}

// ---- port selection --------------------------------------------------------
$("pick").onclick = async () => {
  try {
    port = await navigator.serial.requestPort({});
    $("portinfo").textContent = portLabel(port);
    log(`selected ${portLabel(port)}`, "muted");
  } catch (e) {
    log(`no port selected: ${e.message}`, "warn");
  }
  setBusy(false);
};
$("forget").onclick = async () => {
  await stopMonitor();
  if (port?.forget) await port.forget();
  port = null; $("portinfo").textContent = "no port"; setBusy(false);
};navigator.serial?.addEventListener("disconnect", (e) => { if (e.target === port) log("port disconnected (USB re-enumeration or unplug)", "warn"); });

// ---- probe -----------------------------------------------------------------
const term = { clean() {}, writeLine: (s) => log(s), write: (s) => log(s) };

async function withLoader(fn) {
  const s = await openSession();
  try {
    return await fn(s.loader, s.transport);
  } finally {
    await s.close();
  }
}

// One esptool session: reset into ROM, sync, stub, baud. tracing=true records
// every SLIP frame (hex) in transport.traceLog; only used on retries because
// a full 16 MB dump would make that log enormous.
async function openSession({ tracing = false } = {}) {
  await stopMonitor();
  // 64 KiB host buffer: the 4 KiB default overflowed on multi-KiB stub reads.
  const transport = new Transport(port, tracing);
  // Stub runs at the selected baud (default 1152000); ROM sync stays at 115200.
  // On native USB-Serial/JTAG the rate is nominal, so it is harmless there.
  const baudrate = +$("baud").value || 1152000;
  const loader = new ESPLoader({ transport, baudrate, romBaudrate: 115200, terminal: term, debugLogging: false, serialOptions: { bufferSize: 65536 } });
  return { loader, transport, close: async () => { try { await transport.disconnect(); } catch {} } };
}

const step = async (name, p, ms = 8000, show = true) => {
  if (show) log(`· ${name}…`, "muted");
  let t;
  try {
    return await Promise.race([p, new Promise((_, rej) => (t = setTimeout(() => rej(new Error(`${name} timed out after ${ms} ms`)), ms)))]);
  } finally { clearTimeout(t); }
};

// Stub reads in bounded chunks via readFlashExact (below). Default 1 KiB = one
// small stub packet per READ_FLASH command. The 2026-10-18 wire trace showed
// ~470 bytes vanishing from inside a 4 KiB packet (it arrived as 3626 B), on
// the same chunk in every session; 256-byte reads (probe) never lost data.
async function readRange(loader, addr, len, { chunk = 0x400, page = 0x400, pauseMs = 0, onChunk, stopAtPtEnd = false, startAt = 0, out = new Uint8Array(len) } = {}) {
  for (let o = startAt; o < len; o += chunk) {
    const n = Math.min(chunk, len - o);
    const at = addr + o;
    let part;
    try {
      part = await step(`read 0x${at.toString(16)}+0x${n.toString(16)}`, readFlashExact(loader, at, n, Math.min(page, n), 1, pauseMs), 20000 + (n / page) * pauseMs, (o & 0xffff) === 0);
    } catch (e) {
      e.failedAt = o;  // lets runDump resume from here in a fresh session
      throw e;
    }
    out.set(part, o);
    onChunk?.(o + n, len, out.subarray(o, o + n));
    if (stopAtPtEnd) { for (let e = o; e + 32 <= o + n; e += 32) if (out[e] !== 0xaa || out[e + 1] !== 0x50) return out.subarray(0, e + 32); }
  }
  return out;
}

// esptool-js 0.7.0 readFlash() stops after the data and never reads the 16-byte
// MD5 frame the stub sends once it gets the final ack (esptool.py does:
// `digest_frame = self.read()`). That frame was left in the buffer and broke the
// NEXT read: first read always fine, second stalls or "serial noise". This reader
// consumes the frame and checks it, so every chunk is also integrity-checked.
const u32le = (v) => new Uint8Array([v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]);
// READ_FLASH args: offset, size, block (packet) size, max_in_flight, all u32 LE.
// max_in_flight is BYTES, not packets: the stub sends while
// (sent - acked) < max_in_flight (esptool-legacy-flasher-stub
// handle_flash_read), so any value <= block size means exactly one packet,
// then it waits for our ack. esptool.py passes 64 and espflash defaults to 64,
// which is also one 4 KiB packet at a time. The ack is the cumulative byte
// count as a SLIP-framed u32 LE, same as espflash's write_raw(data.len()).
// pauseMs waits before each ack, giving the link slack between pages.
async function readFlashExact(loader, addr, size, pktSize = 0x400, inflight = 1, pauseMs = 0) {
  const args = new Uint8Array(16);
  args.set(u32le(addr), 0); args.set(u32le(size), 4); args.set(u32le(pktSize), 8); args.set(u32le(inflight), 12);
  await loader.checkCommand("read flash", loader.ESP_READ_FLASH, args);
  const data = new Uint8Array(size);
  let got = 0;
  while (got < size) {
    const p = await loader.transport.read(5000);
    if (!(p instanceof Uint8Array) || !p.length) throw new Error(`read flash: empty packet at +0x${got.toString(16)}`);
    // Every packet is exactly pktSize except the last. A short one means bytes
    // were dropped between board and page; acking it would desync the stub.
    const want = Math.min(pktSize, size - got);
    if (p.length !== want) throw new Error(`read flash 0x${(addr + got).toString(16)}: packet of ${p.length} B, expected ${want} (bytes lost in transit)`);
    data.set(p, got); got += p.length;
    if (pauseMs) await new Promise((r) => setTimeout(r, pauseMs));
    await loader.transport.write(u32le(got));   // ack: total bytes received
  }
  const digest = await loader.transport.read(5000);
  if (!(digest instanceof Uint8Array) || digest.length !== 16) throw new Error(`read flash: expected 16-byte MD5 frame, got ${digest?.length ?? "none"}`);
  const want = Array.from(digest, (x) => x.toString(16).padStart(2, "0")).join("");
  const have = md5hex(data);
  if (want !== have) throw new Error(`read flash 0x${addr.toString(16)}+0x${size.toString(16)}: md5 ${have} != chip ${want}`);
  return data;
}

const blOffset = (chipName) => (/^ESP32(-S2)?$/.test(chipName) ? 0x1000 : 0x0);

const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const cstr = (b, o, n) => dec.decode(b.subarray(o, o + n)).replace(/\0.*$/s, "");
const PT_TYPES = { 0: "app", 1: "data" };
const PT_SUB = { app: { 0: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x12: "ota_2", 0x20: "test" },
                 data: { 0: "ota", 1: "phy", 2: "nvs", 3: "coredump", 4: "nvs_keys", 0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs" } };
const FLASH_SZ = { 0: "1MB", 1: "2MB", 2: "4MB", 3: "8MB", 4: "16MB", 5: "32MB", 6: "64MB", 7: "128MB" };
const FLASH_MODE = { 0: "qio", 1: "qout", 2: "dio", 3: "dout" };
const JEDEC_MFR = { 0xef: "Winbond", 0xc8: "GigaDevice", 0x20: "XMC/Micron", 0x68: "Boya", 0x5e: "Zbit", 0x0b: "XTX", 0x85: "Puya", 0x1c: "EON", 0xc2: "Macronix", 0xa1: "Fudan", 0x9d: "ISSI" };

function parsePartitions(b) {
  const out = [];
  for (let o = 0; o + 32 <= b.length; o += 32) {
    if (b[o] !== 0xaa || b[o + 1] !== 0x50) break;
    const type = PT_TYPES[b[o + 2]] ?? `0x${b[o + 2].toString(16)}`;
    const sub = PT_SUB[type]?.[b[o + 3]] ?? `0x${b[o + 3].toString(16)}`;
    out.push({ name: cstr(b, o + 12, 16), type, subtype: sub, offset: "0x" + u32(b, o + 4).toString(16), size: "0x" + u32(b, o + 8).toString(16), size_kb: u32(b, o + 8) / 1024 });
  }
  return out;
}

function parseAppDesc(b) {
  // esp_image_header (24) + first segment header (8) -> esp_app_desc_t at +32, magic 0xABCD5432
  if (b[0] !== 0xe9 || u32(b, 32) !== 0xabcd5432) return null;
  // esp_app_desc_t: magic@0 secure_ver@4 reserv@8 version@16 project@48 time@80 date@96 idf_ver@112 sha@144
  return { project: cstr(b, 32 + 48, 32), version: cstr(b, 32 + 16, 32), idf: cstr(b, 32 + 112, 32), time: cstr(b, 32 + 80, 16), date: cstr(b, 32 + 96, 16) };
}

$("probe").onclick = () => runProbe();
async function runProbe() {
  setBusy(true);
  log("── probe: entering ROM download mode (board will reset) ──", "muted");
  try {
    report = await withLoader(async (loader) => {
      const r = await identify(loader);
      r.errors = [];
      // Read-only peeks: bootloader header, partition table, first bytes of each app.
      // Each read is isolated: a failure is recorded and the report still goes out.
      try {
        const blAt = blOffset(loader.chip.CHIP_NAME);
        const boot = await step(`read bootloader header @0x${blAt.toString(16)}`, readFlashExact(loader, blAt, 0x20));
        r.bootloader_header = { offset: "0x" + blAt.toString(16), magic_ok: boot[0] === 0xe9, flash_mode: FLASH_MODE[boot[2]] ?? boot[2], flash_size: FLASH_SZ[boot[3] >> 4] ?? (boot[3] >> 4), flash_freq_code: boot[3] & 0xf };
      } catch (e) { r.errors.push(String(e.message ?? e)); log(String(e.message ?? e), "bad"); }
      try {
        r.partitions = parsePartitions(await readRange(loader, 0x8000, 0xc00, { chunk: 0x100, stopAtPtEnd: true }));
      } catch (e) { r.partitions = []; r.errors.push(String(e.message ?? e)); log(String(e.message ?? e), "bad"); }
      r.apps = [];
      for (const p of r.partitions.filter((p) => p.type === "app")) {
        try {
          const desc = parseAppDesc(await readRange(loader, parseInt(p.offset, 16), 0x100, { chunk: 0x100 }));
          r.apps.push({ partition: p.name, ...(desc ?? { empty_or_unknown: true }) });
        } catch (e) { r.errors.push(String(e.message ?? e)); log(String(e.message ?? e), "bad"); break; }
      }
      log(r.errors.length ? `probe finished with ${r.errors.length} error(s); resetting board` : "probe complete; resetting board to run its firmware", r.errors.length ? "warn" : "ok");
      try { await step("hard reset", loader.after("hard_reset"), 4000); } catch (e) { log(String(e.message ?? e), "warn"); }
      return r;
    });
    lastProbe = report;
    renderFacts(report);
    renderPartitionPicker();
    log(JSON.stringify(report, null, 2));
    await sendReport();  // auto-send so the agent sees every probe
    setBusy(false);
    return { ok: !report.errors?.length, report: `latest-${report.label}.json`, errors: report.errors };
  } catch (e) {
    log(`probe failed: ${e.message ?? e}`, "bad");
    log("If the board never entered download mode: hold BOOT, tap RESET, release BOOT, then probe again.", "warn");
    setBusy(false);
    return { ok: false, error: String(e.message ?? e) };
  }
}

let lastProbe = null;

async function identify(loader) {
  const chipName = await loader.main();               // detect + stub + baud
  const c = loader.chip;
  const r = { label: $("label").value, port: portLabel(port), port_info: port.getInfo(), probed_at: new Date().toISOString(), baud: loader.baudrate };
  r.chip = {
    description: chipName,
    name: c.CHIP_NAME,
    features: (await c.getChipFeatures(loader)).map((s) => s.trim()),
    crystal_mhz: await c.getCrystalFreq(loader),
    mac: await c.readMac(loader),
  };
  const id = await step("read flash id", loader.readFlashId());
  const mfr = id & 0xff, sizeId = (id >> 16) & 0xff;
  r.flash = { jedec_id: "0x" + id.toString(16).padStart(6, "0"), manufacturer: JEDEC_MFR[mfr] ?? `0x${mfr.toString(16)}`,
              device: "0x" + ((id >> 8) & 0xffff).toString(16), detected_size: loader.DETECTED_FLASH_SIZES?.[sizeId] ?? null };
  r.flash.bytes = r.flash.detected_size ? loader.flashSizeBytes(r.flash.detected_size) : null;
  return r;
}

function renderFacts(r) {
  const rows = [
    ["chip", r.chip?.description], ["features", r.chip?.features?.join(", ")], ["MAC", r.chip?.mac],
    ["flash JEDEC", `${r.flash?.jedec_id} (${r.flash?.manufacturer}) → ${r.flash?.detected_size ?? "size unknown"}`],
    ["bootloader hdr", r.bootloader_header ? `${r.bootloader_header.flash_mode} / ${r.bootloader_header.flash_size}` : ""],
    ["partitions", r.partitions?.map((p) => `${p.name} ${p.subtype} @${p.offset} ${p.size_kb}K`).join(" · ")],
    ["apps", r.apps?.map((a) => a.project ? `${a.partition}: ${a.project} ${a.version} (IDF ${a.idf})` : `${a.partition}: empty`).join(" · ")],
    ["boot log", r.boot_log ? `${r.boot_log.length} chars captured` : "—"],
  ];
  $("facts").replaceChildren(...rows.map(([k, v]) => { const tr = document.createElement("tr"); tr.innerHTML = "<td></td><td></td>"; tr.cells[0].textContent = k; tr.cells[1].textContent = v ?? ""; return tr; }));
}

// ---- console / boot log -----------------------------------------------------
async function openRaw() {
  if (!port.readable) await port.open({ baudRate: 115200, bufferSize: 65536 });
}

async function pulseReset() {
  // Classic auto-reset wiring: EN on RTS. On native USB-Serial/JTAG the chip resets
  // and the USB device re-enumerates; the reconnect handler below picks it back up.
  await port.setSignals({ dataTerminalReady: false, requestToSend: true });
  await new Promise((r) => setTimeout(r, 120));
  await port.setSignals({ dataTerminalReady: false, requestToSend: false });
}

async function startMonitor(onText) {
  await openRaw();
  const reader = port.readable.getReader();
  monitor = { reader, onText };
  setBusy(false);
  (async () => {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        const t = dec.decode(value, { stream: true });
        log(t.replace(/\r/g, ""));
        monitor?.onText?.(t);
      }
    } catch (e) {
      log(`monitor stopped: ${e.message}`, "warn");
    } finally {
      try { reader.releaseLock(); } catch {}
    }
  })();
}

async function stopMonitor() {
  if (!monitor) return;
  const m = monitor; monitor = null;
  try { await m.reader.cancel(); } catch {}
  await new Promise((r) => setTimeout(r, 50));
  try { await port.close(); } catch {}
  setBusy(false);
}

// Reacquire the same board after a native-USB reset re-enumerates it.
function waitReconnect(ms) {
  return new Promise((resolve) => {
    const t = setTimeout(() => { navigator.serial.removeEventListener("connect", h); resolve(null); }, ms);
    const h = (e) => { clearTimeout(t); navigator.serial.removeEventListener("connect", h); resolve(e.target); };
    navigator.serial.addEventListener("connect", h);
  });
}

$("mon").onclick = async () => {
  if (monitor) { await stopMonitor(); $("mon").textContent = "Open monitor (115200)"; return; }
  try { await startMonitor(); $("mon").textContent = "Close monitor"; } catch (e) { log(`open failed: ${e.message}`, "bad"); }
};
$("rst").onclick = async () => { try { await openRaw(); await pulseReset(); log("reset pulsed", "muted"); } catch (e) { log(`reset failed: ${e.message}`, "bad"); } };
$("line").onkeydown = async (e) => {
  if (e.key !== "Enter" || !port?.writable) return;
  const w = port.writable.getWriter();
  await w.write(new TextEncoder().encode(e.target.value + "\n"));
  w.releaseLock(); log(`> ${e.target.value}`, "muted"); e.target.value = "";
};
$("clear").onclick = () => logEl.replaceChildren();

$("boot").onclick = () => runBoot();
async function runBoot(secsArg, { quietReport = false } = {}) {
  if (!quietReport) setBusy(true);
  const secs = Math.max(2, Math.min(120, secsArg ?? (+$("bootsecs").value || 8)));
  let text = "";
  const collect = (t) => { text += t; };
  log(`── boot capture: reset + ${secs}s ──`, "muted");
  try {
    await stopMonitor();
    await openRaw();
    const reconnect = waitReconnect(5000);
    await pulseReset();
    await port.close().catch(() => {});
    const again = await Promise.race([reconnect, new Promise((r) => setTimeout(() => r(null), 600))]);
    if (again) { port = again; log("board re-enumerated; reattached", "muted"); }
    await startMonitor(collect);
    await new Promise((r) => setTimeout(r, secs * 1000));
    await stopMonitor();
    report = report ?? { label: $("label").value, port: portLabel(port), port_info: port.getInfo() };
    report.boot_log = text;
    report.boot_log_captured_at = new Date().toISOString();
    report.boot_facts = bootFacts(text);
    renderFacts(report);
    if (report.boot_facts) log(JSON.stringify(report.boot_facts, null, 2), "ok");
    if (!quietReport) await sendReport();
    if (!text.trim()) log("No console output. The firmware may log to UART0 (header pins) instead of USB, or it may be silent.", "warn");
    if (!quietReport) setBusy(false);
    return { ok: true, chars: text.length, text, report: `latest-${report.label}.json`, boot_facts: report.boot_facts };
  } catch (e) {
    log(`boot capture failed: ${e.message}`, "bad");
    if (!quietReport) setBusy(false);
    return { ok: false, error: String(e.message ?? e) };
  }
}

function bootFacts(t) {
  const m = (re) => t.match(re)?.slice(1);
  const f = {
    psram: m(/Found (\d+)MB PSRAM device/)?.[0] ? `${m(/Found (\d+)MB PSRAM device/)[0]} MB` : null,
    psram_mode: m(/(Octal|Quad|OPI|QSPI)[^\n]*PSRAM|PSRAM[^\n]*(Octal|Quad|OPI)/i)?.filter(Boolean)[0] ?? null,
    spi_flash: m(/SPI Flash Size\s*:\s*(\S+)/)?.[0] ?? m(/detected chip:\s*(\S+)/)?.[0] ?? null,
    flash_io: m(/flash io:\s*(\S+)/)?.[0] ?? null,
    app_offset: m(/Loaded app from partition at offset (0x[0-9a-f]+)/i)?.[0] ?? null,
    console: m(/GPIO (\d+ and \d+) are used as console UART/)?.[0] ? `UART0 on GPIO ${m(/GPIO (\d+ and \d+) are used as console UART/)[0]}` : null,
    touch: m(/Touch (\S+ 0x[0-9a-f]+) found/i)?.[0] ?? null,
    app: m(/Project name:\s*(.+)/)?.[0]?.trim() ?? null,
    app_version: m(/App version:\s*(.+)/)?.[0]?.trim() ?? null,
    idf: m(/ESP-IDF:\s*(\S+)/)?.[0] ?? null,
    reset_reason: m(/rst:(0x[0-9a-f]+ \([A-Z_]+\))/i)?.[0] ?? null,
    i2c_devices: [...t.matchAll(/0x([0-9a-f]{2})\b[^\n]*(found|detected|ack)/gi)].map((x) => "0x" + x[1]),
  };
  return Object.values(f).some((v) => v && (!Array.isArray(v) || v.length)) ? f : null;
}

// ---- report ----------------------------------------------------------------
$("send").onclick = () => sendReport();
async function sendReport() {
  if (!report) return;
  report.label = $("label").value;
  report.session = session;
  try {
    const r = await send("report", report);
    const j = await r.json();
    $("sent").textContent = r.ok ? `stored ${j.stored}` : `error: ${j.error}`;
    $("sent").className = r.ok ? "ok" : "bad";
    loadReports();
  } catch (e) { $("sent").textContent = `send failed: ${e.message}`; $("sent").className = "bad"; }
}

async function loadReports() {
  try {
    const list = await (await fetch("api/reports")).json();
    $("reports").textContent = list.length ? list.map((x) => x.name).join("  ·  ") : "none yet";
  } catch { $("reports").textContent = "server unreachable"; }
}

if (envCheck()) {
  navigator.serial.getPorts().then((ps) => { if (ps.length === 1) { port = ps[0]; $("portinfo").textContent = portLabel(port) + " (remembered)"; setBusy(false); } });
}
setBusy(false);
loadReports();
loadSets();
pollJobs();
setInterval(pollJobs, 3000);

// ---- dump --------------------------------------------------------------------
const progress = (el, done, total, t0) => {
  const pct = total ? (100 * done) / total : 0;
  const secs = (performance.now() - t0) / 1000;
  const rate = secs > 0 ? done / 1024 / secs : 0;
  $(el).textContent = `${pct.toFixed(1)}%  ${(done / 1024).toFixed(0)}/${(total / 1024).toFixed(0)} KiB  ${rate.toFixed(0)} KiB/s`;
};

function renderPartitionPicker() {
  const sel = $("dumpwhat");
  const keep = sel.value;
  sel.replaceChildren(new Option("whole flash", "all"), ...(lastProbe?.partitions ?? []).map((p) => new Option(`${p.name} (${p.subtype}, ${p.size_kb}K @${p.offset})`, `partition:${p.name}`)), new Option("custom range…", "range"));
  if ([...sel.options].some((o) => o.value === keep)) sel.value = keep;
}
$("dumpwhat").onchange = () => { $("dumprange").hidden = $("dumpwhat").value !== "range"; };
$("dump").onclick = () => runDump($("dumpwhat").value === "range" ? $("dumprange").value.trim() : $("dumpwhat").value);

// what: "all" | "partition:<name>" | "<offset>:<length>" (hex or decimal)
async function runDump(what) {
  setBusy(true);
  const t0 = performance.now();
  log(`── dump ${what} ──`, "muted");
  let session = null;
  try {
    session = await openSession();
    let loader = session.loader;
    const id = await identify(loader);
    let addr, len, part = null;
    if (what === "all") { addr = 0; len = id.flash.bytes; }
    else if (what.startsWith("partition:")) {
      const pname = what.slice(10);
      const pt = parsePartitions(await readRange(loader, 0x8000, 0xc00, { chunk: 0x100, stopAtPtEnd: true }));
      part = pt.find((p) => p.name === pname);
      if (!part) throw new Error(`no partition "${pname}" (have: ${pt.map((p) => p.name).join(", ")})`);
      addr = parseInt(part.offset, 16); len = parseInt(part.size, 16);
    } else {
      const [a, l] = what.split(":").map((s) => Number(s));
      if (!Number.isInteger(a) || !Number.isInteger(l) || l <= 0) throw new Error(`bad range "${what}" (want offset:length, e.g. 0x8000:0x1000)`);
      addr = a; len = l;
    }
    if (!len) throw new Error("flash size unknown; dump a range instead");
    if (id.flash.bytes && addr + len > id.flash.bytes) throw new Error(`range ends at 0x${(addr + len).toString(16)}, past ${id.flash.detected_size} flash`);
    const name = `${$("label").value}-${id.chip.mac.replace(/:/g, "")}-${what === "all" ? "full" : part ? part.name : `0x${addr.toString(16)}`}-${new Date().toISOString().replace(/[-:]/g, "").slice(0, 15)}`;
    // Upload in 1 MiB pieces as reads complete, in order.
    let sent = 0, pending = [], pendingLen = 0, upChain = Promise.resolve();
    const flushUp = async (final = false) => {
      if (!pendingLen || (!final && pendingLen < 1 << 20)) return;
      const chunks = pending, n = pendingLen; pending = []; pendingLen = 0;
      const body = new Uint8Array(n); let o = 0;
      for (const c of chunks) { body.set(c, o); o += c.length; }
      const r = await fetch(`api/dump/${name}?offset=${sent}`, { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body });
      if (!r.ok) throw new Error(`upload at ${sent} failed: ${r.status} ${(await r.text()).slice(0, 200)}`);
      sent += n;
    };
    const data = new Uint8Array(len);
    const onChunk = (done, total, chunk) => {
      progress("dumpprog", done, total, t0);
      pending.push(chunk.slice()); pendingLen += chunk.length;
      upChain = upChain.then(() => flushUp());
    };
    // A failed chunk costs a fresh session (reset, sync, stub) and a resume at
    // that chunk; earlier chunks are kept. Give up only after 3 failures on the
    // SAME chunk, so a long dump with scattered losses still completes.
    const page = +$("dumppage").value || 256;
    const pauseMs = Math.max(0, Math.min(100, +$("dumppause").value || 0));
    // One command covers 16 pages: fewer command round trips; still one page unacked.
    const rd = { onChunk, out: data, page, pauseMs, chunk: page * 16 };
    log(`pages of ${page} B, ${pauseMs} ms pause per page, ${page * 16} B per command`, "muted");
    let from = 0, retries = 0, sameChunk = 0, lastFail = -1;
    for (;;) {
      try {
        await readRange(loader, addr, len, { ...rd, startAt: from });
        break;
      } catch (e) {
        if (e.failedAt == null) throw e;
        sameChunk = e.failedAt === lastFail ? sameChunk + 1 : 1;
        lastFail = e.failedAt;
        if (sameChunk > 3) throw new Error(`chunk +0x${e.failedAt.toString(16)} failed 3 times: ${e.message ?? e}`);
        retries++;
        from = e.failedAt;
        log(`chunk at +0x${from.toString(16)} failed (${e.message ?? e}); new session, resuming (${retries} so far)`, "warn");
        await session.close();
        session = await openSession();
        loader = session.loader;
        await loader.main();
      }
    }
    await upChain;
    await flushUp(true);
    log("· verifying with on-chip MD5…", "muted");
    const local = md5hex(data);
    const chipMd5 = await step("flash md5", loader.flashMd5sum(addr, len), 120000);
    const meta = { label: $("label").value, mac: id.chip.mac, chip: id.chip.description, flash: id.flash, what, partition: part, offset: "0x" + addr.toString(16), size: len, md5: local, chip_md5: chipMd5, verified_on_chip: local === chipMd5, retries, page, pause_ms: pauseMs, seconds: +((performance.now() - t0) / 1000).toFixed(1), baud: loader.baudrate };
    const done = await (await send(`dump/${name}/done`, meta)).json();
    if (local !== chipMd5) log(`MD5 MISMATCH: read ${local}, chip ${chipMd5}. The dump is corrupt; retry at a lower baud.`, "bad");
    else if (!done.verified) log(`server copy mismatch: ${JSON.stringify(done.server)}`, "bad");
    else log(`dump verified (${len} bytes, md5 ${local}${retries ? `, ${retries} resumed chunk(s)` : ""}) → dumps/${name}.bin`, "ok");
    try { await loader.after("hard_reset"); } catch {}
    setBusy(false);
    return { ok: local === chipMd5 && done.verified, file: `dumps/${name}.bin`, md5: local, size: len, retries, seconds: meta.seconds };
  } catch (e) {
    log(`dump failed: ${e.message ?? e}`, "bad");
    if (session?.transport.traceLog) await shipTrace(session.transport.traceLog, "dump-failed");
    setBusy(false);
    return { ok: false, error: String(e.message ?? e) };
  } finally {
    await session?.close();
  }
}

// Wire traces go to the log endpoint under their own session id so the agent
// can read reports/<tag>.log; capped to the last 256 KiB.
async function shipTrace(text, tag) {
  const body = text.length > 262144 ? text.slice(-262144) : text;
  try { await send("log", body, { query: { s: `trace-${tag}`.slice(0, 48), l: $("label").value } }); } catch {}
  log(`wire trace (${(body.length / 1024).toFixed(0)} KiB) sent to the agent`, "muted");
}

// ---- flash (+ provisioning) ---------------------------------------------------------
// With "provision" ticked this is `npx tiny-tech flash --body sticky-the-reterminal`
// in the browser: read the MAC over the ROM loader, name the board as its
// firmware does, get a device credential (this host's tiny session, else a
// pairing code the owner approves), build the NVS image the firmware reads and
// write bootloader + table + otadata + app + NVS in ONE session, then watch the
// first boot. The device token lives only in this page's memory and the NVS
// image; it is never logged, reported or kept by the server.
let sets = [];
async function loadSets() {
  try {
    sets = await (await fetch("api/sets")).json();
    const sel = $("flashset"), keep = sel.value;
    sel.replaceChildren(...(sets.length ? sets.map((s) => new Option(s.error ? `⚠ ${s.error}` : `${s.name} — ${s.boards?.join("/") || "any board"} — ${s.flash_settings?.flash_size || "?"} — ${s.parts.length} parts${s.version ? " — " + s.version : ""}`, s.name)) : [new Option("no image sets (build firmware or add images/<set>/manifest.json)", "")]));
    if ([...sel.options].some((o) => o.value === keep)) sel.value = keep;
    renderSet();
  } catch { $("flashparts").textContent = "server unreachable"; }
}
function renderSet() {
  const s = sets.find((x) => x.name === $("flashset").value);
  $("flashparts").textContent = s ? (s.note ? s.note + "\n" : "") + s.parts.map((p) => `0x${p.offset.toString(16).padStart(6, "0")}  ${p.file}  ${(p.size / 1024).toFixed(0)} KiB  md5 ${p.md5}`).join("\n") +
    `\nflash ${s.flash_settings?.flash_mode || "keep"} / ${s.flash_settings?.flash_size || "keep"} / ${s.flash_settings?.flash_freq || "keep"}   source: ${s.source}` : "";
}
$("flashset").onchange = renderSet;
$("reloadsets").onclick = loadSets;
$("provision").onchange = () => { $("provbox").hidden = !$("provision").checked; };
const provOpts = () => ({ provision: $("provision").checked, adopt: $("adopt").checked, ssid: $("ssid").value.trim(), key: $("wifikey").value });
$("flash").onclick = () => runFlash($("flashset").value, $("eraseall").checked, provOpts());
$("plan").onclick = () => runFlash($("flashset").value, $("eraseall").checked, { ...provOpts(), dryRun: true });

// The label in the bench's picker → the TINY_BOARD a set declares.
const BOARD_OF_LABEL = { "fnk0104s": "fnk0104s", "amoled18-v2": "amoled18", "reterminal-sticky": "reterminal" };
const SIZE_BYTES = (s) => (/^(\d+)MB$/.exec(s || "") ? +RegExp.$1 << 20 : null);

async function tinyApi(path, body) {
  const { ok, status, j } = await sendJson(`tiny/${path}`, body ?? {});
  if (!ok && !j.status) throw new Error(j.error || `tiny/${path}: ${status}`);
  return j;
}

function showPairing(code, url, ttl) {
  const box = $("pairbox");
  box.hidden = false;
  $("paircode").textContent = showCode(code);
  const a = $("pairlink"); a.href = url; a.textContent = url;
  // qrcode-generator 1.4.4 (MIT, vendor/); loaded on demand so a broken vendor
  // file never stops a plain flash.
  import("./vendor/qrcode-generator-1.4.4.js").then(({ default: qrcode }) => {
    const qr = qrcode(0, "M"); qr.addData(url); qr.make();
    $("pairqr").innerHTML = qr.createSvgTag({ cellSize: 4, margin: 2, scalable: false, alt: `QR code for ${url}` });
  }).catch(() => { $("pairqr").textContent = "(QR unavailable; use the link)"; });
  $("pairstate").textContent = `waiting for approval… (code valid ${Math.round(ttl / 60)} min)`;
}

// The credential, tiny-tech acquireChildIdentity: session (duplicate guard /
// adopt / re-key a stale row) or headless pairing. Returns {device_id, device_token, name, api, via, adopted}.
async function acquireCredential(name, adopt) {
  const st = await tinyApi("status");
  if (st.session) {
    log(`credential: this host's tiny session (@${st.login}) → ${st.api}/api/devices`, "muted");
    const r = await tinyApi("enroll", { name, adopt });
    if (!r.device_token) throw new Error(r.error || "enroll returned no token");
    log(`✓ ${r.adopted ? "re-keyed existing row" : "enrolled"} ${r.name} (${String(r.device_id).slice(0, 8)}…) with the session`, "ok");
    return r;
  }
  log(`credential: no tiny session on the bench host → pairing code via ${st.api}`, "muted");
  const s = await tinyApi("pair/start", { name });
  showPairing(s.code, s.url, s.ttl_s);
  log(`pair this board: code ${showCode(s.code)} — approve at ${s.url}`, "warn");
  const deadline = Date.now() + s.ttl_s * 1000;
  try {
    while (Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 3000));
      if (pairCancelled) throw new Error("pairing cancelled");
      const p = await tinyApi("pair/poll", { code: s.code });
      if (p.status === "approved" && p.device_token) {
        $("pairstate").textContent = `approved — enrolled as ${p.name}`;
        log(`✓ approved — enrolled as ${p.name} (${String(p.device_id).slice(0, 8)}…)`, "ok");
        return p;
      }
      if (p.status === "denied") throw new Error("the owner denied this device");
      if (p.status === "expired") throw new Error("pairing code expired — start again");
      if (p.status === "claimed") throw new Error("this code was already claimed by another process");
      if (p.status === "vanished") throw new Error("pairing code vanished (bench server restarted, or registry reset) — start again");
      $("pairstate").textContent = `waiting for approval… ${Math.max(0, Math.round((deadline - Date.now()) / 1000))} s left`;
    }
    throw new Error("pairing timed out — start again");
  } finally { if (!$("pairstate").textContent.startsWith("approved")) $("pairstate").textContent += "  (stopped)"; }
}
let pairCancelled = false;
$("paircancel").onclick = () => { pairCancelled = true; };

function bootVerdict(text, name) {
  const lines = text.split("\n").filter((l) => /tiny_config|tiny_provision|provision|wifi|got ip|heartbeat|hostname|panic|abort|Guru|node loop/i.test(l));
  return {
    lines: lines.slice(0, 40).map((l) => l.trim().slice(0, 200)),
    got_ip: /got ip/i.test(text),
    hostname: new RegExp(`hostname ${name}`).test(text),
    heartbeat: /heartbeat[^\n]*(ok|200)/i.test(text),
    crashed: /Guru Meditation|panic|abort\(\)/.test(text),
    waiting_for_identity: /waiting for device_id\+token/.test(text),
  };
}

async function runFlash(setName, eraseAll, { provision = false, adopt = false, ssid = "", key = "", dryRun = false, preconfirmed = false } = {}) {
  await loadSets();
  const s = sets.find((x) => x.name === setName);
  if (!s || s.error || !s.parts.length) { log(`no usable image set "${setName}"`, "bad"); return { ok: false, error: "no such set" }; }
  const label = $("label").value;
  const board = BOARD_OF_LABEL[label];
  if (s.boards?.length && !s.boards.includes(label) && !s.boards.includes(board)) {
    // Provisioning writes a credential: a board/set mismatch is a refusal, not a question.
    if (provision || dryRun) { log(`refused: set "${s.name}" is for ${s.boards.join(", ")}, bench label is "${label}"`, "bad"); return { ok: false, error: "board label mismatch" }; }
    if (!confirm(`Image set "${s.name}" is declared for ${s.boards.join(", ")}, but the board label is "${label}". Flash anyway?`)) return { ok: false, error: "label mismatch declined" };
  }
  if (provision && !ssid) { log("provision needs the Wi-Fi network the board should join (SSID)", "bad"); return { ok: false, error: "no ssid" }; }
  if (provision && s.parts.some((p) => p.offset < NVS_OFFSET + NVS_SIZE && p.offset + p.size > NVS_OFFSET)) { log(`set "${s.name}" already writes the NVS range 0x9000+0x6000`, "bad"); return { ok: false, error: "set overlaps nvs" }; }
  setBusy(true);
  pairCancelled = false;
  $("pairbox").hidden = true;
  const t0 = performance.now();
  log(`── ${dryRun ? "plan" : "flash"} ${s.name}${provision ? " + provision" : ""}${eraseAll ? " (erase all)" : ""} ──`, "muted");
  let token = null;
  try {
    // Fetch and verify every part BEFORE touching the board.
    const files = [];
    for (const p of s.parts) {
      const r = await fetch(`api/sets/${encodeURIComponent(s.name)}/${encodeURIComponent(p.file)}`);
      if (!r.ok) throw new Error(`fetch ${p.file}: ${r.status}`);
      const data = new Uint8Array(await r.arrayBuffer());
      const got = md5hex(data);
      if (data.length !== p.size || got !== p.md5) throw new Error(`${p.file}: downloaded ${data.length} B md5 ${got}, server said ${p.size} B md5 ${p.md5}`);
      files.push({ data, address: p.offset, name: p.file });
    }
    log(`fetched ${files.length} part(s), md5 verified`, "ok");
    const fs = s.flash_settings || {};
    // Checks every write must pass, run in each ROM session.
    const check = (id) => {
      if (s.chip && s.chip.toUpperCase() !== id.chip.name.toUpperCase()) throw new Error(`image set is for ${s.chip}, board is ${id.chip.name}`);
      const want = SIZE_BYTES(fs.flash_size);
      if (want && id.flash.bytes && want !== id.flash.bytes) throw new Error(`image set is built for ${fs.flash_size} flash, this board has ${id.flash.detected_size}`);
      const end = Math.max(...files.map((f) => f.address + f.data.length), provision ? NVS_OFFSET + NVS_SIZE : 0);
      if (id.flash.bytes && end > id.flash.bytes) throw new Error(`image ends at 0x${end.toString(16)}, past ${id.flash.detected_size} flash`);
      const bl = files.find((f) => f.address === blOffset(id.chip.name));
      if (!bl && files.some((f) => f.address < 0x8000)) throw new Error(`a part sits below 0x8000 but not at this chip's bootloader offset 0x${blOffset(id.chip.name).toString(16)}`);
    };
    // Session 1: identify + plan. Closed again before any credential exists, so
    // a pairing approval that takes minutes never leaves a ROM session idle.
    const { id, name, plan } = await withLoader(async (loader) => {
      const id = await identify(loader);
      check(id);
      const name = boardName(id.chip.mac);
      log(`board ${id.chip.mac} (softAP ${softapMac(id.chip.mac)}) → ${name}`, "ok");
      const plan = {
        set: s.name, boards: s.boards, flash: `${fs.flash_mode || "keep"} / ${fs.flash_size || "keep"} / ${fs.flash_freq || "keep"}`, erase_all: eraseAll,
        board: { mac: id.chip.mac, softap_mac: softapMac(id.chip.mac), name, chip: id.chip.description, flash: id.flash.detected_size },
        parts: [...files.map((f) => `0x${f.address.toString(16)} ${f.name} ${f.data.length} B`), ...(provision ? [`0x${NVS_OFFSET.toString(16)} nvs.bin ${NVS_SIZE} B (tiny/cfg: identity + 1 network)`] : [])],
      };
      if (provision) {
        const st = await tinyApi("status");
        plan.credential = st.session ? `host session @${st.login} → POST ${st.api}/api/devices (duplicate guard${adopt ? ", adopt if online" : ""})` : `no session on the bench host → pairing code via ${st.api}/api/devices/pair/*`;
        if (st.session) {
          const look = await tinyApi("lookup", { name });
          plan.existing_rows = look.rows;
          const live = look.rows?.find((r) => r.online === true);
          if (live && !adopt) throw new Error(`"${name}" is ONLINE as ${String(live.id).slice(0, 8)}… — this board (or its twin) is heartbeating. Tick "adopt" to re-key that row.`);
        }
        plan.wifi = ssid;
      }
      log(JSON.stringify(plan, null, 2));
      try { await loader.after("hard_reset"); } catch {}
      return { id, name, plan };
    });
    if (dryRun) {
      log("dry run — nothing written to the board, nothing enrolled", "ok");
      await send("report", { label, kind: "flash-plan", session, ok: true, plan });
      setBusy(false);
      return { ok: true, dry_run: true, plan };
    }
    const summary = `Write ${plan.parts.length} part(s) from "${s.name}" to the board labelled "${label}" (${name}, ${id.chip.mac})` +
      (eraseAll ? ", ERASING THE WHOLE CHIP FIRST (Wi-Fi, pairing token, settings all lost)" : provision ? " (its NVS is REPLACED: identity + this Wi-Fi only)" : " (regions outside these parts are kept)") +
      (provision ? `\n\nIt will be enrolled as ${name}: ${plan.credential}.` : "") +
      `\n\n${plan.parts.join("\n")}`;
    if (!preconfirmed && !confirm(summary)) { log("flash cancelled", "muted"); setBusy(false); return { ok: false, error: "cancelled by owner" }; }
    const fileArray = files.map(({ data, address }) => ({ data, address }));
    let cred = null;
    if (provision) {
      // The token exists once: get it only after the owner confirmed and every
      // check passed, so a refusal never leaves an enrolled row with no board.
      cred = await acquireCredential(name, adopt);
      token = cred.device_token;
      const cfg = buildCfg({ device_id: cred.device_id, token, api: cred.api, name, networks: [{ ssid, key }] });
      const nvs = tinyNvs(cfg);
      const back = JSON.parse(readNvsStrings(nvs)["tiny/cfg"]);
      if (back.device_id !== cfg.device_id || back.token !== token) throw new Error("NVS image did not read back");
      fileArray.push({ data: nvs, address: NVS_OFFSET });
      files.push({ data: nvs, address: NVS_OFFSET, name: "nvs.bin" });
      log(`NVS image built: ${NVS_SIZE} B at 0x${NVS_OFFSET.toString(16)} (device ${String(cfg.device_id).slice(0, 8)}…, Wi-Fi "${ssid}")`, "ok");
    }
    fileArray.sort((a, b) => a.address - b.address);
    // Session 2: the ONE write. Same board? (A swapped cable would enrol the wrong chip.)
    const res = await withLoader(async (loader) => {
      const id2 = await identify(loader);
      if (id2.chip.mac !== id.chip.mac) throw new Error(`board changed: planned ${id.chip.mac}, now ${id2.chip.mac}. Not writing ${name}'s identity to it.${cred ? ` Re-run with "adopt" to re-key ${name}.` : ""}`);
      check(id2);
      await loader.writeFlash({
        fileArray,
        flashMode: fs.flash_mode || "keep", flashFreq: fs.flash_freq || "keep", flashSize: fs.flash_size || "keep",
        eraseAll, compress: true,
        reportProgress: (i, written, total) => { $("flashprog").textContent = `part ${i + 1}/${fileArray.length}: ${((100 * written) / total).toFixed(0)}%`; },
        calculateMD5Hash: (img) => md5hex(img),   // esptool-js re-hashes on chip after each part
      });
      log(`flash written and verified on chip in ${((performance.now() - t0) / 1000).toFixed(1)} s; resetting`, "ok");
      try { await loader.after("hard_reset"); } catch {}
      return { ok: true, set: s.name, name, mac: id.chip.mac, erase_all: eraseAll,
               parts: files.map((f) => ({ file: f.name, offset: "0x" + f.address.toString(16), md5: f.name === "nvs.bin" ? "(holds the token; not reported)" : md5hex(f.data) })),
               provisioned: !!cred, device_id: cred?.device_id, via: cred?.via, adopted: cred?.adopted, wifi: provision ? ssid : undefined,
               seconds: +((performance.now() - t0) / 1000).toFixed(1) };
    });
    token = null;  // last reference outside the closures above
    if (res.ok && res.provisioned) {
      log("── first boot (40 s): looking for tiny_config / wifi / got ip / heartbeat ──", "muted");
      const boot = await runBoot(40, { quietReport: true });
      res.boot = bootVerdict(boot.text || "", res.name);
      log(JSON.stringify(res.boot, null, 2), res.boot.crashed ? "bad" : res.boot.got_ip ? "ok" : "warn");
      if (res.boot.got_ip) log(`${res.name} joined Wi-Fi. tiny.technology/devices should show it online within a minute.`, "ok");
      else log("No 'got ip' in 40 s: check the SSID/key, or that the board logs to this USB port.", "warn");
    }
    await send("report", redact({ label, kind: dryRun ? "flash-plan" : "flash", session, ...res }));
    setBusy(false);
    return redact(res);
  } catch (e) {
    token = null;
    log(`${dryRun ? "plan" : "flash"} failed: ${e.message ?? e}`, "bad");
    if (!dryRun && !/cancelled|refused|ONLINE|mismatch|built for|denied|expired|claimed|vanished|timed out/.test(String(e.message))) log("A failed write leaves the board unbootable until a flash succeeds. The ROM loader is in mask ROM and cannot be damaged: retry, lower the baud, or hold BOOT while plugging in.", "warn");
    setBusy(false);
    return { ok: false, error: String(e.message ?? e) };
  }
}

// ---- agent-queued jobs -----------------------------------------------------------
// The agent drops jobs/*.json via tools/devbench/job.py. Each one shows as a
// banner; nothing runs until the owner clicks Run (flash also confirms). A job's
// result goes back through send(), so Run and Decline both work behind the
// tunnel, and a failed post shows on the banner instead of vanishing.
const seenJobs = new Set();
async function postResult(j, res, div) {
  try {
    const r = await send(`jobs/${j.id}/result`, redact(res ?? { ok: false }));
    if (!r.ok) throw new Error(`${r.status} ${(await r.text()).slice(0, 160)}`);
    log(`job ${j.id}: result recorded`, "muted");
  } catch (e) {
    const msg = `could not record the result of job ${j.id}: ${e.message ?? e}`;
    log(msg, "bad");
    if (div) { const err = document.createElement("div"); err.className = "bad small"; err.textContent = msg; div.append(err); }
    return false;
  }
  pollJobs();
  return true;
}
async function pollJobs() {
  let jobs = [];
  try { jobs = await (await fetch("api/jobs")).json(); } catch { return; }
  const box = $("jobs");
  box.hidden = !jobs.length;
  box.replaceChildren(...jobs.map((j) => {
    const div = document.createElement("div");
    div.className = "job";
    const what = j.kind === "flash" ? `${j.dry_run ? "PLAN (dry run) for " : ""}flash set "${j.set}"${j.provision ? ` + PROVISION${j.adopt ? " (adopt)" : ""}` : ""}${j.erase_all ? " + ERASE ALL" : ""}` : j.kind === "dump" ? `dump ${j.what}` : j.kind === "boot" ? `boot log ${j.secs}s` : j.kind;
    div.innerHTML = `<b></b> <span></span> <span class="muted"></span> `;
    div.children[0].textContent = `Agent asks: ${what}`;
    div.children[1].textContent = `for board "${j.label}"`;
    div.children[2].textContent = j.note ? `— ${j.note}` : "";
    const run = document.createElement("button"); run.textContent = "Run"; run.className = j.kind === "flash" && !j.dry_run ? "danger" : "primary";
    const skip = document.createElement("button"); skip.textContent = "Decline";
    run.disabled = busy || !port;
    run.onclick = async () => {
      if (j.label && $("label").value !== j.label) {
        if (!confirm(`This job is for "${j.label}" but the bench is labelled "${$("label").value}". Switch the label to "${j.label}" and run it on the connected board?`)) return;
        $("label").value = j.label;
      }
      let res;
      setMode({ probe: "identify", boot: "identify", dump: "dump", flash: "flash" }[j.kind] || "console");
      if (j.kind === "flash" && j.provision) {
        // Wi-Fi never rides in a job file: the owner types it on the Flash tab.
        $("provision").checked = true; $("provbox").hidden = false; $("adopt").checked = !!j.adopt;
        if (!$("ssid").value.trim()) { log("this job provisions the board: type the Wi-Fi SSID (and key) on the Flash tab, then press Run again", "warn"); $("ssid").focus(); return; }
      }
      if (j.kind === "probe") res = await runProbe();
      else if (j.kind === "boot") { res = await runBoot(j.secs); delete res.text; }
      else if (j.kind === "dump") res = await runDump(j.what || "all");
      else if (j.kind === "flash") res = await runFlash(j.set, !!j.erase_all, { ...(j.provision ? provOpts() : {}), provision: !!j.provision, adopt: !!j.adopt, dryRun: !!j.dry_run });
      else res = { ok: false, error: `unknown job kind ${j.kind}` };
      if (res?.error === "cancelled by owner") return;  // stays queued
      await postResult(j, res, div);
    };
    skip.onclick = async () => { skip.disabled = true; if (!(await postResult(j, { ok: false, declined: true }, div))) skip.disabled = false; };
    div.append(run, " ", skip);
    if (!seenJobs.has(j.id)) { seenJobs.add(j.id); log(`agent queued job ${j.id}: ${what} for "${j.label}"`, "warn"); }
    return div;
  }));
}
