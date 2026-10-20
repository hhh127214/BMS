// Render one PDF page to PNG through Chrome's built-in viewer (for layout spot-checks).
// usage: node _pdfshot.mjs <pdf-path> <page> <out.png>
import { spawn } from 'node:child_process';
import fs from 'node:fs';

const CHROME = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const PORT = 9337;
const pdf = process.argv[2];
const page = process.argv[3] || '1';
const out = process.argv[4] || 'C:/tmp/pdfpage.png';

const prof = 'C:\\Users\\31857\\AppData\\Local\\Temp\\cdpPdf_' + Date.now();
const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-sandbox',
  '--hide-scrollbars', '--force-device-scale-factor=2',
  `--remote-debugging-port=${PORT}`, `--user-data-dir=${prof}`, 'about:blank'],
  { stdio: ['ignore', 'ignore', 'ignore'] });

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let msgId = 0;
const pending = new Map();
function send(ws, method, params = {}, sessionId) {
  const id = ++msgId;
  const payload = { id, method, params };
  if (sessionId) payload.sessionId = sessionId;
  ws.send(JSON.stringify(payload));
  return new Promise((res, rej) => {
    pending.set(id, { res, rej });
    setTimeout(() => { if (pending.has(id)) { pending.delete(id); rej(new Error('timeout ' + method)); } }, 30000);
  });
}

let ver = null;
for (let i = 0; i < 40; i++) {
  try { ver = await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json(); break; }
  catch { await sleep(250); }
}
if (!ver) throw new Error('CDP 未就绪');

const ws = new WebSocket(ver.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) {
    const { res, rej } = pending.get(m.id);
    pending.delete(m.id);
    if (m.error) rej(new Error(JSON.stringify(m.error))); else res(m.result);
  }
};

const { targetId } = await send(ws, 'Target.createTarget', { url: 'about:blank' });
const { sessionId } = await send(ws, 'Target.attachToTarget', { targetId, flatten: true });
const S = (m, p) => send(ws, m, p, sessionId);

await S('Page.enable');
await S('Emulation.setDeviceMetricsOverride', { width: 1240, height: 1754, deviceScaleFactor: 2, mobile: false });

const url = 'file:///' + pdf.replace(/\\/g, '/') + '#page=' + page + '&zoom=page-width';
await S('Page.navigate', { url });
await sleep(4500);

const shot = await S('Page.captureScreenshot', { format: 'png', fromSurface: true });
fs.writeFileSync(out, Buffer.from(shot.data, 'base64'));
console.log('saved ' + out + ' (' + Math.round(fs.statSync(out).size / 1024) + ' KB)');

try { await send(ws, 'Browser.close'); } catch {}
ws.close();
chrome.kill();
