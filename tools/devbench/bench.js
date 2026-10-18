// devbench page logic. Runs in the OWNER's browser; talks to the board with Web
// Serial and reports to the agent's server with relative fetches (works under
// any reverse-proxy prefix). Never writes flash.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.0.bundle.js";

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
  for (const id of ["probe", "boot", "mon", "rst", "forget"]) $(id).disabled = b || !port;
  $("pick").disabled = b;
  $("send").disabled = b || !report;
  $("line").disabled = !monitor;
}

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
};
navigator.serial?.addEventListener("disconnect", (e) => { if (e.target === port) log("port disconnected (USB re-enumeration or unplug)", "warn"); });

// ---- probe -----------------------------------------------------------------
const term = { clean() {}, writeLine: (s) => log(s), write: (s) => log(s) };

async function withLoader(fn) {
  await stopMonitor();
  const transport = new Transport(port, false);
  // Stub runs at the selected baud for flash reads (default 1152000); ROM sync stays
  // at 115200. On native USB-Serial/JTAG the rate is nominal, so it is harmless there.
  const baudrate = +$("baud").value || 1152000;
  const loader = new ESPLoader({ transport, baudrate, romBaudrate: 115200, terminal: term, debugLogging: false });
  try {
    return await fn(loader, transport);
  } finally {
    try { await transport.disconnect(); } catch {}
  }
}

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

$("probe").onclick = async () => {
  setBusy(true);
  log("── probe: entering ROM download mode (board will reset) ──", "muted");
  try {
    report = await withLoader(async (loader) => {
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
      const step = async (name, p, ms = 8000) => {
        log(`· ${name}…`, "muted");
        let t;
        try {
          return await Promise.race([p, new Promise((_, rej) => (t = setTimeout(() => rej(new Error(`${name} timed out after ${ms} ms`)), ms)))]);
        } finally { clearTimeout(t); }
      };
      r.errors = [];
      const id = await step("read flash id", loader.readFlashId());
      const mfr = id & 0xff, sizeId = (id >> 16) & 0xff;
      r.flash = { jedec_id: "0x" + id.toString(16).padStart(6, "0"), manufacturer: JEDEC_MFR[mfr] ?? `0x${mfr.toString(16)}`,
                  device: "0x" + ((id >> 8) & 0xffff).toString(16), detected_size: loader.DETECTED_FLASH_SIZES?.[sizeId] ?? null };
      // Read-only peeks: bootloader header, partition table, first bytes of each app.
      // Each read is isolated: a failure is recorded and the report still goes out.
      try {
        // Second-stage bootloader offset: 0x1000 on ESP32/ESP32-S2, 0x0 on S3/C-series.
        const blAt = /^ESP32(-S2)?$/.test(c.CHIP_NAME) ? 0x1000 : 0x0;
        const boot = await step(`read bootloader header @0x${blAt.toString(16)}`, loader.readFlash(blAt, 0x20));
        r.bootloader_header = { offset: "0x" + blAt.toString(16), magic_ok: boot[0] === 0xe9, flash_mode: FLASH_MODE[boot[2]] ?? boot[2], flash_size: FLASH_SZ[boot[3] >> 4] ?? (boot[3] >> 4), flash_freq_code: boot[3] & 0xf };
      } catch (e) { r.errors.push(String(e.message ?? e)); log(String(e.message ?? e), "bad"); }
      // Large stub reads stalled on both boards at 0xc00 (timeout on one, "serial
      // noise" on the other) while the 0x20 header read worked. Read in small
      // chunks, retry each once, and stop at the table's end marker.
      const readChunked = async (addr, len, chunk = 0x100) => {
        const out = new Uint8Array(len);
        for (let o = 0; o < len; o += chunk) {
          const n = Math.min(chunk, len - o);
          let part;
          try { part = await step(`read 0x${(addr + o).toString(16)}+0x${n.toString(16)}`, loader.readFlash(addr + o, n), 4000); }
          catch (e) { log(`retry: ${e.message ?? e}`, "warn"); part = await step(`re-read 0x${(addr + o).toString(16)}`, loader.readFlash(addr + o, n), 4000); }
          out.set(part.subarray(0, n), o);
          // End of partition table: an entry slot that is not 0xAA50 (MD5 0xEBEB or erased 0xFF).
          if (addr === 0x8000) { for (let e = o; e + 32 <= o + n; e += 32) if (out[e] !== 0xaa || out[e + 1] !== 0x50) return out.subarray(0, e + 32); }
        }
        return out;
      };
      try {
        r.partitions = parsePartitions(await readChunked(0x8000, 0xc00));
      } catch (e) { r.partitions = []; r.errors.push(String(e.message ?? e)); log(String(e.message ?? e), "bad"); }
      r.apps = [];
      for (const p of r.partitions.filter((p) => p.type === "app")) {
        try {
          const desc = parseAppDesc(await readChunked(parseInt(p.offset, 16), 0x100));
          r.apps.push({ partition: p.name, ...(desc ?? { empty_or_unknown: true }) });
        } catch (e) { r.errors.push(String(e.message ?? e)); log(String(e.message ?? e), "bad"); break; }
      }
      log(r.errors.length ? `probe finished with ${r.errors.length} error(s); resetting board` : "probe complete; resetting board to run its firmware", r.errors.length ? "warn" : "ok");
      try { await step("hard reset", loader.after("hard_reset"), 4000); } catch (e) { log(String(e.message ?? e), "warn"); }
      return r;
    });
    renderFacts(report);
    log(JSON.stringify(report, null, 2));
    await sendReport();  // auto-send so the agent sees every probe
  } catch (e) {
    log(`probe failed: ${e.message ?? e}`, "bad");
    log("If the board never entered download mode: hold BOOT, tap RESET, release BOOT, then probe again.", "warn");
  }
  setBusy(false);
};

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

$("boot").onclick = async () => {
  setBusy(true);
  const secs = Math.max(2, Math.min(60, +$("bootsecs").value || 8));
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
  } catch (e) {
    log(`boot capture failed: ${e.message}`, "bad");
  }
  setBusy(false);
};

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
