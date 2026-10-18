// devbench page logic. Runs in the OWNER's browser; talks to the board with Web
// Serial and reports to the agent's server with relative fetches (works under
// any reverse-proxy prefix). Probe/boot/dump are read-only; Flash writes, and
// only after an explicit confirm naming board, image set and erase mode.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.0.bundle.js";
import { md5hex } from "./md5.js";

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
  if (!logBuf) return;
  const body = logBuf; logBuf = "";
  try {
    const r = await fetch("api/log", { method: "POST", headers: { "X-Devbench-Session": session, "X-Devbench-Label": $("label").value, "Content-Type": "text/plain" }, body });
    if (!r.ok && r.status !== 413) logBuf = body + logBuf;  // 413 = rejected, don't retry forever
  } catch { logBuf = body + logBuf; }
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
  for (const id of ["probe", "boot", "mon", "rst", "forget", "dump", "flash"]) $(id).disabled = b || !port;
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
  await stopMonitor();
  // 64 KiB host buffer: the 4 KiB default overflowed on multi-KiB stub reads
  // (the 0xc00 partition-table read stalled on both S3 boards).
  const transport = new Transport(port, false);
  // Stub runs at the selected baud for flash reads (default 1152000); ROM sync stays
  // at 115200. On native USB-Serial/JTAG the rate is nominal, so it is harmless there.
  const baudrate = +$("baud").value || 1152000;
  const loader = new ESPLoader({ transport, baudrate, romBaudrate: 115200, terminal: term, debugLogging: false, serialOptions: { bufferSize: 65536 } });
  try {
    return await fn(loader, transport);
  } finally {
    try { await transport.disconnect(); } catch {}
  }
}

const step = async (name, p, ms = 8000) => {
  log(`· ${name}…`, "muted");
  let t;
  try {
    return await Promise.race([p, new Promise((_, rej) => (t = setTimeout(() => rej(new Error(`${name} timed out after ${ms} ms`)), ms)))]);
  } finally { clearTimeout(t); }
};

// Stub reads in bounded chunks. Each chunk uses readFlashExact (below), which
// follows esptool.py's protocol including the trailing per-read MD5 frame.
async function readRange(loader, addr, len, { chunk = 0x10000, onChunk, stopAtPtEnd = false } = {}) {
  const out = new Uint8Array(len);
  for (let o = 0; o < len; o += chunk) {
    const n = Math.min(chunk, len - o);
    const at = addr + o;
    const part = await step(`read 0x${at.toString(16)}+0x${n.toString(16)}`, readFlashExact(loader, at, n), 20000);
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
async function readFlashExact(loader, addr, size, pktSize = 0x1000, inflight = 64) {
  const args = new Uint8Array(16);
  args.set(u32le(addr), 0); args.set(u32le(size), 4); args.set(u32le(pktSize), 8); args.set(u32le(inflight), 12);
  await loader.checkCommand("read flash", loader.ESP_READ_FLASH, args);
  const data = new Uint8Array(size);
  let got = 0;
  while (got < size) {
    const p = await loader.transport.read(5000);
    if (!(p instanceof Uint8Array) || !p.length) throw new Error(`read flash: empty packet at +0x${got.toString(16)}`);
    if (got + p.length > size) throw new Error(`read flash: got ${got + p.length} bytes, asked ${size}`);
    data.set(p, got); got += p.length;
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
async function runBoot(secsArg) {
  setBusy(true);
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
    await sendReport();
    if (!text.trim()) log("No console output. The firmware may log to UART0 (header pins) instead of USB, or it may be silent.", "warn");
    setBusy(false);
    return { ok: true, chars: text.length, report: `latest-${report.label}.json`, boot_facts: report.boot_facts };
  } catch (e) {
    log(`boot capture failed: ${e.message}`, "bad");
    setBusy(false);
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
    const r = await fetch("api/report", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(report) });
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
  try {
    const res = await withLoader(async (loader) => {
      const id = await identify(loader);
      let addr, len, part = null;
      if (what === "all") { addr = 0; len = id.flash.bytes; }
      else if (what.startsWith("partition:")) {
        const name = what.slice(10);
        const pt = parsePartitions(await readRange(loader, 0x8000, 0xc00, { chunk: 0x100, stopAtPtEnd: true }));
        part = pt.find((p) => p.name === name);
        if (!part) throw new Error(`no partition "${name}" (have: ${pt.map((p) => p.name).join(", ")})`);
        addr = parseInt(part.offset, 16); len = parseInt(part.size, 16);
      } else {
        const [a, l] = what.split(":").map((s) => Number(s));
        if (!Number.isInteger(a) || !Number.isInteger(l) || l <= 0) throw new Error(`bad range "${what}" (want offset:length, e.g. 0x8000:0x1000)`);
        addr = a; len = l;
      }
      if (!len) throw new Error("flash size unknown; dump a range instead");
      if (id.flash.bytes && addr + len > id.flash.bytes) throw new Error(`range ends at 0x${(addr + len).toString(16)}, past ${id.flash.detected_size} flash`);
      const name = `${$("label").value}-${id.chip.mac.replace(/:/g, "")}-${what === "all" ? "full" : part ? part.name : `0x${addr.toString(16)}`}-${new Date().toISOString().replace(/[-:]/g, "").slice(0, 15)}`;
      // Stream to the server in 1 MiB uploads as reads complete, so a long dump
      // survives a pane reload up to the last uploaded MiB.
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
      const data = await readRange(loader, addr, len, {
        onChunk: (done, total, chunk) => {
          progress("dumpprog", done, total, t0);
          pending.push(chunk.slice()); pendingLen += chunk.length;
          upChain = upChain.then(() => flushUp());   // serialised uploads, in order
        },
      });
      await upChain;
      await flushUp(true);
      // Cross-check: stub hashes the flash range itself; must equal what we read.
      log("· verifying with on-chip MD5…", "muted");
      const local = md5hex(data);
      const chipMd5 = await step("flash md5", loader.flashMd5sum(addr, len), 120000);
      const meta = { label: $("label").value, mac: id.chip.mac, chip: id.chip.description, flash: id.flash, what, partition: part, offset: "0x" + addr.toString(16), size: len, md5: local, chip_md5: chipMd5, verified_on_chip: local === chipMd5, seconds: +((performance.now() - t0) / 1000).toFixed(1), baud: loader.baudrate };
      const done = await (await fetch(`api/dump/${name}/done`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(meta) })).json();
      if (local !== chipMd5) log(`MD5 MISMATCH: read ${local}, chip ${chipMd5}. The dump is corrupt; retry at a lower baud.`, "bad");
      else if (!done.verified) log(`server copy mismatch: ${JSON.stringify(done.server)}`, "bad");
      else log(`dump verified (${len} bytes, md5 ${local}) → dumps/${name}.bin`, "ok");
      try { await loader.after("hard_reset"); } catch {}
      return { ok: local === chipMd5 && done.verified, file: `dumps/${name}.bin`, md5: local, size: len, seconds: meta.seconds };
    });
    setBusy(false);
    return res;
  } catch (e) {
    log(`dump failed: ${e.message ?? e}`, "bad");
    setBusy(false);
    return { ok: false, error: String(e.message ?? e) };
  }
}

// ---- flash -------------------------------------------------------------------
let sets = [];
async function loadSets() {
  try {
    sets = await (await fetch("api/sets")).json();
    const sel = $("flashset"), keep = sel.value;
    sel.replaceChildren(...(sets.length ? sets.map((s) => new Option(s.error ? `⚠ ${s.error}` : `${s.name} — ${s.chip || "?"} — ${s.parts.length} parts${s.version ? " — " + s.version : ""}`, s.name)) : [new Option("no image sets (build firmware or add images/<set>/manifest.json)", "")]));
    if ([...sel.options].some((o) => o.value === keep)) sel.value = keep;
    renderSet();
  } catch { $("flashparts").textContent = "server unreachable"; }
}
function renderSet() {
  const s = sets.find((x) => x.name === $("flashset").value);
  $("flashparts").textContent = s ? (s.note ? s.note + "\n" : "") + s.parts.map((p) => `0x${p.offset.toString(16).padStart(6, "0")}  ${p.file}  ${(p.size / 1024).toFixed(0)} KiB  md5 ${p.md5}`).join("\n") + `\nsource: ${s.source}` : "";
}
$("flashset").onchange = renderSet;
$("reloadsets").onclick = loadSets;
$("flash").onclick = () => runFlash($("flashset").value, $("eraseall").checked);

async function runFlash(setName, eraseAll, { preconfirmed = false } = {}) {
  await loadSets();
  const s = sets.find((x) => x.name === setName);
  if (!s || s.error || !s.parts.length) { log(`no usable image set "${setName}"`, "bad"); return { ok: false, error: "no such set" }; }
  const label = $("label").value;
  if (s.boards?.length && !s.boards.includes(label)) {
    if (!confirm(`Image set "${s.name}" is declared for ${s.boards.join(", ")}, but the board label is "${label}". Flash anyway?`)) return { ok: false, error: "label mismatch declined" };
  }
  const summary = `Write ${s.parts.length} part(s) from "${s.name}" to the board labelled "${label}"` +
    (eraseAll ? ", ERASING THE WHOLE CHIP FIRST (NVS: Wi-Fi, pairing token, settings all lost)" : " (regions outside these parts are kept)") +
    `?\n\n${s.parts.map((p) => `0x${p.offset.toString(16)}  ${p.file}  ${(p.size / 1024).toFixed(0)} KiB`).join("\n")}`;
  if (!preconfirmed && !confirm(summary)) { log("flash cancelled", "muted"); return { ok: false, error: "cancelled by owner" }; }
  setBusy(true);
  const t0 = performance.now();
  log(`── flash ${s.name}${eraseAll ? " (erase all)" : ""} ──`, "muted");
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
    const res = await withLoader(async (loader) => {
      const id = await identify(loader);
      if (s.chip && s.chip.toUpperCase() !== id.chip.name.toUpperCase()) throw new Error(`image set is for ${s.chip}, board is ${id.chip.name}`);
      const end = Math.max(...files.map((f) => f.address + f.data.length));
      if (id.flash.bytes && end > id.flash.bytes) throw new Error(`image ends at 0x${end.toString(16)}, past ${id.flash.detected_size} flash`);
      const bl = files.find((f) => f.address === blOffset(id.chip.name));
      if (!bl && files.some((f) => f.address < 0x8000)) throw new Error(`a part sits below 0x8000 but not at this chip's bootloader offset 0x${blOffset(id.chip.name).toString(16)}`);
      const fs = s.flash_settings || {};
      await loader.writeFlash({
        fileArray: files.map(({ data, address }) => ({ data, address })),
        flashMode: fs.flash_mode || "keep", flashFreq: fs.flash_freq || "keep", flashSize: fs.flash_size || "keep",
        eraseAll, compress: true,
        reportProgress: (i, written, total) => { $("flashprog").textContent = `${files[i].name}: ${((100 * written) / total).toFixed(0)}%`; },
        calculateMD5Hash: (img) => md5hex(img),   // esptool-js re-hashes on chip after each part
      });
      log(`flash written and verified on chip in ${((performance.now() - t0) / 1000).toFixed(1)} s; resetting`, "ok");
      try { await loader.after("hard_reset"); } catch {}
      return { ok: true, set: s.name, parts: files.map((f) => ({ file: f.name, offset: "0x" + f.address.toString(16), md5: md5hex(f.data) })), erase_all: eraseAll, mac: id.chip.mac, seconds: +((performance.now() - t0) / 1000).toFixed(1) };
    });
    await fetch("api/report", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ label, kind: "flash", session, ...res }) });
    setBusy(false);
    return res;
  } catch (e) {
    log(`flash failed: ${e.message ?? e}`, "bad");
    log("A failed write leaves the board unbootable until a flash succeeds. The ROM loader is in mask ROM and cannot be damaged: retry, lower the baud, or hold BOOT while plugging in.", "warn");
    setBusy(false);
    return { ok: false, error: String(e.message ?? e) };
  }
}

// ---- agent-queued jobs -----------------------------------------------------------
// The agent drops jobs/*.json via tools/devbench/job.py. Each one shows as a
// banner; nothing runs until the owner clicks Run (flash also confirms).
const seenJobs = new Set();
async function pollJobs() {
  let jobs = [];
  try { jobs = await (await fetch("api/jobs")).json(); } catch { return; }
  const box = $("jobs");
  box.hidden = !jobs.length;
  box.replaceChildren(...jobs.map((j) => {
    const div = document.createElement("div");
    div.className = "job";
    const what = j.kind === "flash" ? `flash set "${j.set}"${j.erase_all ? " + ERASE ALL" : ""}` : j.kind === "dump" ? `dump ${j.what}` : j.kind === "boot" ? `boot log ${j.secs}s` : j.kind;
    div.innerHTML = `<b></b> <span></span> <span class="muted"></span> `;
    div.children[0].textContent = `Agent asks: ${what}`;
    div.children[1].textContent = `for board "${j.label}"`;
    div.children[2].textContent = j.note ? `— ${j.note}` : "";
    const run = document.createElement("button"); run.textContent = "Run"; run.className = j.kind === "flash" ? "danger" : "primary";
    const skip = document.createElement("button"); skip.textContent = "Decline";
    run.disabled = busy || !port;
    run.onclick = async () => {
      if (j.label && $("label").value !== j.label) {
        if (!confirm(`This job is for "${j.label}" but the bench is labelled "${$("label").value}". Switch the label to "${j.label}" and run it on the connected board?`)) return;
        $("label").value = j.label;
      }
      let res;
      setMode({ probe: "identify", boot: "identify", dump: "dump", flash: "flash" }[j.kind] || "console");
      if (j.kind === "probe") res = await runProbe();
      else if (j.kind === "boot") res = await runBoot(j.secs);
      else if (j.kind === "dump") res = await runDump(j.what || "all");
      else if (j.kind === "flash") res = await runFlash(j.set, !!j.erase_all);
      else res = { ok: false, error: `unknown job kind ${j.kind}` };
      if (res?.error === "cancelled by owner") return;  // stays queued
      await fetch(`api/jobs/${j.id}/result`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(res ?? { ok: false }) });
      pollJobs();
    };
    skip.onclick = async () => { await fetch(`api/jobs/${j.id}/result`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ok: false, declined: true }) }); pollJobs(); };
    div.append(run, " ", skip);
    if (!seenJobs.has(j.id)) { seenJobs.add(j.id); log(`agent queued job ${j.id}: ${what} for "${j.label}"`, "warn"); }
    return div;
  }));
}
