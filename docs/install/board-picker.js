// Install page: board picker + optional Wi-Fi, in front of ESP Web Tools.
//
// Each board has its own manifest (tools/release.sh: manifest-<board>.json).
// With Wi-Fi filled in, this page writes an NVS image (namespace "tiny", key
// "cfg", networks only, built by provision.js) and hands ESP Web Tools a
// manifest that adds it at 0x9000, so the board joins Wi-Fi on first boot and
// shows its pairing code (onboarding Phase B) instead of opening its setup AP.
//
// Why no device identity here, unlike devbench and `npx tiny-tech flash`:
// tiny.technology's pairing API sends no CORS headers (checked 2026-10-24:
// OPTIONS /api/devices/pair/start answers 204 without Access-Control-Allow-Origin
// for https://cagataycali.github.io), so a page on GitHub Pages cannot read its
// answers. The board pairs itself instead. Nothing typed here leaves the browser.
import { buildCfg, tinyNvs, NVS_OFFSET, NVS_SIZE } from "./provision.js";

const BOARDS = {
  reterminal: "Seeed reTerminal Sticky (3.97\" e-ink, 32 MB)",
  fnk0104s: "Freenove FNK0104S (4.0\" LCD, 16 MB)",
  amoled18: "Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 (16 MB)",
};
const $ = (id) => document.getElementById(id);
const btn = document.querySelector("esp-web-install-button#install-sticky");
const base = new URL(".", location.href);
const manifests = {};
let blobUrls = [];

async function manifestFor(board) {
  if (!manifests[board]) {
    const r = await fetch(new URL(board === "reterminal" ? "manifest.json" : `manifest-${board}.json`, base));
    manifests[board] = r.ok ? await r.json() : null;
  }
  return manifests[board];
}

function note(text, cls = "") {
  const el = $("install-note");
  el.textContent = text;
  el.className = `install-note ${cls}`;
}

async function refresh() {
  for (const u of blobUrls) URL.revokeObjectURL(u);
  blobUrls = [];
  const board = $("install-board").value;
  const m = await manifestFor(board);
  if (!m) {
    btn.setAttribute("hidden", "");
    note(`No published build for ${BOARDS[board]} yet.`, "warn");
    return;
  }
  btn.removeAttribute("hidden");
  const ver = btn.querySelector("small");
  if (ver) ver.textContent = `v${m.version}`;
  // Absolute part URLs: a blob: manifest resolves relative paths against the page.
  const parts = m.builds[0].parts.map((p) => ({ ...p, path: new URL(p.path, base).toString() }));
  const ssid = $("install-ssid").value.trim();
  if (ssid) {
    let nvs;
    try {
      nvs = tinyNvs(buildCfg({ networks: [{ ssid, key: $("install-key").value }] }));
    } catch (e) {
      note(e.message, "warn");
      btn.setAttribute("hidden", "");
      return;
    }
    const nvsUrl = URL.createObjectURL(new Blob([nvs], { type: "application/octet-stream" }));
    blobUrls.push(nvsUrl);
    parts.push({ path: nvsUrl, offset: NVS_OFFSET });
    parts.sort((a, b) => a.offset - b.offset);
    note(`Adds a ${NVS_SIZE / 1024} KiB settings image with "${ssid}". It replaces the board's saved settings: it joins this network and shows a pairing code.`);
  } else {
    note("No Wi-Fi given: after the flash the board walks you through Wi-Fi and pairing on its own screen.");
  }
  const url = URL.createObjectURL(new Blob([JSON.stringify({ ...m, builds: [{ ...m.builds[0], parts }] })], { type: "application/json" }));
  blobUrls.push(url);
  btn.manifest = url;
  btn.setAttribute("manifest", url);
}

if (btn && $("install-board")) {
  const sel = $("install-board");
  sel.replaceChildren(...Object.entries(BOARDS).map(([v, t]) => new Option(t, v)));
  const want = new URLSearchParams(location.search).get("board");
  if (want && BOARDS[want]) sel.value = want;
  for (const id of ["install-board", "install-ssid", "install-key"]) $(id).addEventListener("input", refresh);
  refresh();
}
