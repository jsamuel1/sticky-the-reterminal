// provision.js — the pieces of `npx tiny-tech flash --body sticky-the-reterminal`
// that run in a browser: the board's own name, the NVS image the firmware reads
// at boot, and the pairing helpers. Shared by tools/devbench (served next to
// bench.js) and docs/install (a copy; tools/devbench/selftest.py checks they
// are byte-identical). No dependencies, no network, no DOM.
//
// The NVS image is written here rather than by esp_idf_nvs_partition_gen so the
// public install page, which has no server, can build it. It follows that tool's
// layout exactly (format version 2, the default): selftest.py compares this
// writer's output byte for byte against the generator in espressif/idf:v5.4.

// ---- name ---------------------------------------------------------------------
// Firmware tiny_provision.cpp make_ssid(): FNV-1a32 over the 6 SOFTAP MAC bytes,
// low 16 bits. esptool (and esptool-js readMac) report the BASE (STA) MAC; ESP-IDF
// derives softAP as base with mac[5] += 1 (mac_addr.c, no carry into mac[4]).
// Vector (owner's FNK0104S): base b8:1f:3f:c5:05:2c → softAP …:2d → tiny-eba6.
// Hashing the base MAC gives tiny-ed39, a name the board never broadcasts.
export function parseMac(mac) {
  const b = String(mac).trim().toLowerCase().split(/[:-]/).map((x) => parseInt(x, 16));
  if (b.length !== 6 || b.some((x) => !(x >= 0 && x <= 255))) throw new Error(`bad MAC "${mac}"`);
  return b;
}
export function softapMac(baseMac) {
  const b = parseMac(baseMac);
  b[5] = (b[5] + 1) & 0xff;
  return b.map((x) => x.toString(16).padStart(2, "0")).join(":");
}
export function fnv1a32(bytes) {
  let h = 0x811c9dc5;
  for (const x of bytes) { h ^= x; h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
}
export function boardName(baseMac) {
  return `tiny-${(fnv1a32(parseMac(softapMac(baseMac))) & 0xffff).toString(16).padStart(4, "0")}`;
}

// ---- cfg blob -------------------------------------------------------------------
// tiny_config.cpp: namespace "tiny", key "cfg", one JSON string. Field limits are
// tiny_config_t's char arrays minus the NUL; an over-long value would be
// truncated by copy_str on the board, so refuse it here instead.
export const NVS_OFFSET = 0x9000, NVS_SIZE = 0x6000;  // partitions.csv and partitions_16mb.csv
export const DEFAULT_API = "https://tiny.technology";
const LIMITS = { device_id: 47, token: 79, api: 95, name: 47, ssid: 32, key: 64, networks: 8 };
const utf8 = (s) => new TextEncoder().encode(s);

export function buildCfg({ device_id, token, api = DEFAULT_API, name, networks = [] }) {
  const cfg = {};
  const put = (k, v) => {
    if (v == null || v === "") return;
    if (typeof v !== "string") throw new Error(`${k} must be a string`);
    if (utf8(v).length > LIMITS[k]) throw new Error(`${k} is ${utf8(v).length} bytes; the board keeps at most ${LIMITS[k]}`);
    cfg[k] = v;
  };
  put("device_id", device_id); put("token", token); put("api", api); put("name", name);
  if (!Array.isArray(networks) || networks.length > LIMITS.networks) throw new Error(`networks: at most ${LIMITS.networks}`);
  cfg.networks = networks.map(({ ssid, key = "" }) => {
    if (!ssid || utf8(ssid).length > LIMITS.ssid) throw new Error("Wi-Fi SSID must be 1..32 bytes");
    if (utf8(key).length > LIMITS.key) throw new Error("Wi-Fi key must be at most 64 bytes");
    if (key && utf8(key).length < 8) throw new Error("WPA2 keys are at least 8 characters (leave it empty for an open network)");
    return { ssid, key };
  });
  if (!cfg.networks.length) delete cfg.networks;
  return cfg;
}
// Same separators as provision_new.sh (json.dumps(cfg, separators=(",",":"))).
export const cfgJson = (cfg) => JSON.stringify(cfg);

// ---- NVS partition image ------------------------------------------------------------
// Page = 32-byte header, 32-byte entry-state bitmap, 126 entries of 32 bytes.
// Entry: ns(1) type(1) span(1) chunkIndex(1) crc32(4) key(16) data(8).
const PAGE = 4096, ENTRIES = 126, ACTIVE = 0xfffffffe, FULL = 0xfffffffc, VERSION2 = 0xfe;
const T_U8 = 0x01, T_SZ = 0x21, MAX_SZ = 4000;  // V2 string limit (incl. NUL)

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
// zlib.crc32(data, 0xFFFFFFFF), as the generator calls it.
export function crc32(data, seed = 0xffffffff) {
  let c = ~seed >>> 0;
  for (const x of data) c = CRC_TABLE[(c ^ x) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}
const putU32 = (b, o, v) => { b[o] = v & 255; b[o + 1] = (v >>> 8) & 255; b[o + 2] = (v >>> 16) & 255; b[o + 3] = (v >>> 24) & 255; };

class Page {
  constructor(num) {
    this.buf = new Uint8Array(PAGE).fill(0xff);
    this.n = 0;
    putU32(this.buf, 0, ACTIVE);
    putU32(this.buf, 4, num);
    this.buf[8] = VERSION2;
    putU32(this.buf, 28, crc32(this.buf.subarray(4, 28)));
  }
  room() { return ENTRIES - this.n; }
  write(bytes, count) {
    this.buf.set(bytes, 64 + 32 * this.n);
    for (let i = 0; i < count; i++, this.n++) this.buf[32 + (this.n * 2 >> 3)] &= ~(1 << ((this.n * 2) & 7));
  }
}
function header(ns, type, span, key) {
  const e = new Uint8Array(32).fill(0xff);
  e[0] = ns; e[1] = type; e[2] = span; e[3] = 0xff;
  const k = utf8(key);
  if (k.length > 15) throw new Error(`NVS key "${key}" over 15 bytes`);
  e.fill(0, 8, 24); e.set(k, 8);
  return e;
}
const sealHeader = (e) => { const c = new Uint8Array(28); c.set(e.subarray(0, 4), 0); c.set(e.subarray(8, 32), 4); putU32(e, 4, crc32(c)); return e; };

// entries, in order like the generator's CSV rows:
//   {namespace: "tiny"}  {key: "cfg", string: "..."}  {key: "x", u8: 1}
// Layout matches esp_idf_nvs_partition_gen: the last page is the erased spare;
// every page before it gets a header with its sequence number and is marked
// FULL (the generator's closing create_new_page() loop flips each page to FULL
// before it checks the remaining size, including the last one it creates).
export function nvsImage(entries, size = NVS_SIZE) {
  if (size % PAGE || size < 3 * PAGE) throw new Error("NVS size must be a multiple of 4096 and at least 0x3000");
  const usable = size / PAGE - 1;
  const pages = [new Page(0)];
  const cur = () => pages[pages.length - 1];
  const fresh = () => {
    if (pages.length >= usable) throw new Error("NVS image full");
    pages.push(new Page(pages.length));
    return cur();
  };
  const nsIndex = new Map();
  let ns = 0;
  for (const e of entries) {
    if (e.namespace != null) {
      if (nsIndex.has(e.namespace)) { ns = nsIndex.get(e.namespace); continue; }
      ns = nsIndex.size + 1;
      nsIndex.set(e.namespace, ns);
      const h = header(0, T_U8, 1, e.namespace);
      h[24] = ns;
      (cur().room() < 1 ? fresh() : cur()).write(sealHeader(h), 1);
    } else if (!ns) {
      throw new Error("write a namespace entry first");
    } else if (e.u8 != null) {
      const h = header(ns, T_U8, 1, e.key);
      h[24] = e.u8 & 0xff;
      (cur().room() < 1 ? fresh() : cur()).write(sealHeader(h), 1);
    } else if (e.string != null) {
      const data = utf8(e.string + "\0");
      if (data.length > MAX_SZ) throw new Error(`NVS string "${e.key}" is ${data.length} bytes; limit ${MAX_SZ}`);
      const dataEntries = (data.length + 31) >> 5;
      const page = cur().n + dataEntries + 1 >= ENTRIES ? fresh() : cur();  // generator: >= max_entries
      const h = header(ns, T_SZ, dataEntries + 1, e.key);
      h[24] = data.length & 255; h[25] = data.length >> 8;
      putU32(h, 28, crc32(data));
      page.write(sealHeader(h), 1);
      page.write(data, dataEntries);
    } else {
      throw new Error("entry needs namespace, u8 or string");
    }
  }
  while (pages.length < usable) pages.push(new Page(pages.length));
  const out = new Uint8Array(size).fill(0xff);
  for (const [i, p] of pages.entries()) { putU32(p.buf, 0, FULL); out.set(p.buf, i * PAGE); }
  return out;
}

// The one image tiny_config.cpp reads at boot.
export function tinyNvs(cfg) {
  return nvsImage([{ namespace: "tiny" }, { key: "cfg", string: cfgJson(cfg) }]);
}

// ---- reading it back (selftest, and the plan's "what will the board see") --------------
export function readNvsStrings(img) {
  const out = {}, names = {};
  for (let o = 0; o + PAGE <= img.length; o += PAGE) {
    const state = (img[o] | img[o + 1] << 8 | img[o + 2] << 16 | img[o + 3] << 24) >>> 0;
    if (state !== ACTIVE && state !== FULL) continue;
    for (let i = 0; i < ENTRIES;) {
      const st = (img[o + 32 + (i * 2 >> 3)] >> ((i * 2) & 7)) & 3;
      if (st !== 2) { i++; continue; }  // 2 = written, 3 = empty, 0 = erased
      const e = img.subarray(o + 64 + 32 * i, o + 96 + 32 * i);
      const key = new TextDecoder().decode(e.subarray(8, 24)).replace(/\0.*$/s, "");
      if (e[1] === T_U8 && e[0] === 0) names[e[24]] = key;
      if (e[1] === T_SZ) {
        const len = e[24] | e[25] << 8;
        const data = img.subarray(o + 96 + 32 * i, o + 96 + 32 * i + len);
        if (crc32(data) !== ((e[28] | e[29] << 8 | e[30] << 16 | e[31] << 24) >>> 0)) throw new Error(`crc mismatch on ${key}`);
        out[`${names[e[0]] ?? e[0]}/${key}`] = new TextDecoder().decode(data.subarray(0, len - 1));
      }
      i += Math.max(1, e[2]);
    }
  }
  return out;
}

// ---- pairing helpers (the tiny-tech headless contract) ---------------------------------
// pair/start {name, platform, kind, capabilities, secretHash} → {code, ttl_s}
// pair/poll  {code, secret} → {status:"approved", device_id, device_token, name}
//            | denied | expired | claimed | 404 "unknown code"
// Firmware verbs: firmware/main/tiny_node.cpp s_caps[] verbatim, in order. A static page
// cannot read the source, so this is a copy — tools/devbench/selftest.py fails the day it
// drifts from provision_lib.firmware_caps(firmware/).
export const STICKY_CAPABILITIES = ["render_ui", "status", "sensors", "say", "ask", "play", "voice", "screenshot",
  "miccheck", "page", "rotate", "glance", "tap", "swipe", "scroll", "lock", "unlock", "sleep", "ota", "messages", "config", "agent", "sd"];
export function stickyClaim(name, via) {
  const caps = ["body:sticky-the-reterminal", ...(via ? [`via:${String(via).slice(0, 8)}`] : []), ...STICKY_CAPABILITIES];
  return { name, platform: "esp32s3", kind: "daemon", capabilities: [...new Set(caps.map((c) => c.toLowerCase().slice(0, 32)))].slice(0, 32) };
}
export const pairUrl = (api, code) => `${String(api).replace(/\/$/, "")}/devices/pair?code=${encodeURIComponent(code)}`;
export const showCode = (code) => `${code.slice(0, 3)} ${code.slice(3)}`;
// Anything that may hold a token, shown or shipped anywhere.
export function redact(v) {
  return JSON.parse(JSON.stringify(v ?? null, (k, x) => (/^(token|device_token|secret|key|wifi_key)$/i.test(k) && typeof x === "string" && x ? `<redacted ${x.length} chars>` : x)));
}
