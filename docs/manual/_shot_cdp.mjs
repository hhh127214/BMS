import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const CHROME = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const PORT = 9334;
const BASE = process.env.SHOT_BASE || 'http://127.0.0.1:8765';
const OUTDIR = 'D:\\wb(cn)\\BMS\\docs\\manual\\shots';
const TOKEN = process.argv[2] || '';
const USER = JSON.stringify({ user_id: 1, username: 'admin', role: 'admin', display_name: '系统管理员' });

// [hash, 文件名, 是否整页, 滚动到页尾, 导航后执行的 JS（用于把页面摆到正常态）]
// ★ 演示截图必须呈现系统正常工作的样子：默认选中项若恰好是停用/未落盘的那台，
//   页面上就会有红字，看着像故障 —— 那是"默认选中了谁"，不是"系统坏了"。
const PICK_DEVICE = "var s=document.querySelectorAll('select')[0];"
  + "if(s&&s.value!=='DEV-LOAD-01'){s.value='DEV-LOAD-01';"
  + "s.dispatchEvent(new Event('change',{bubbles:true}));}";
const PICK_PART = "var g=document.querySelectorAll('svg g');"
  + "for(var i=0;i<g.length;i++){var t=(g[i].textContent||'');"
  + "if(t.indexOf('无实时源')<0&&t.trim()){"
  + "g[i].dispatchEvent(new MouseEvent('click',{bubbles:true}));break;}}";

// 打开「接入调度主站」全页弹层（点按钮）
const OPEN_WIZARD = "var bs=document.querySelectorAll('button');"
  + "for(var i=0;i<bs.length;i++){var t=(bs[i].textContent||'').trim();"
  + "if(t==='接入调度主站'){bs[i].click();break;}}";
// 点步骤条上的第 6 格（「站内配置」）—— 步骤条可点，不必连点五次下一步
const GOTO_STEP6 = "var ps=document.querySelectorAll('.wz-pill');"
  + "for(var i=0;i<ps.length;i++){if((ps[i].textContent||'').trim()==='6'){ps[i].click();break;}}";

const ALL = [
  ['login',      '01_login',       false, false, null],
  ['overview',   '02_overview',    false, false, null],
  ['realtime',   '03_realtime',    false, false, null],
  ['curves',     '04_curves',      false, false, null],
  ['alarms',     '05_alarms',      false, false, null],
  ['strategies', '06_strategies',  true,  false, null],
  ['economics',  '07_economics',   false, false, null],
  ['report',     '08_report',      true,  false, null],
  ['structure',  '09_structure',   false, false, PICK_PART],
  ['wiring',     '10_wiring',      false, false, null],
  ['devices',    '11_devices',     false, false, PICK_DEVICE],
  ['devices',    '12_devices_sim', false, true,  PICK_DEVICE],
  ['users',      '13_users',       false, false, null],
  // 九步向导（v1.14 起）：入口弹层与第 6 步「站内配置」
  ['devices',    '14_wizard_entry', false, false, [PICK_DEVICE, OPEN_WIZARD]],
  ['devices',    '15_wizard_step6', false, false, [PICK_DEVICE, OPEN_WIZARD, GOTO_STEP6]],
];
const ONLY = process.argv[3] || '';
const PAGES = ONLY ? ALL.filter(p => p[0] === ONLY || p[1] === ONLY) : ALL;

// 个别页面一屏放不下（第 6 步有 4 张卡），单独把视口拉高，避免表格被切在页脚上
const TALL = { '15_wizard_step6': 1900, '14_wizard_entry': 950 };

const prof = 'C:\\Users\\31857\\AppData\\Local\\Temp\\cdpShot_' + Date.now();
const chrome = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-sandbox',
  '--hide-scrollbars', '--force-device-scale-factor=2', '--font-render-hinting=none',
  `--remote-debugging-port=${PORT}`, `--user-data-dir=${prof}`, 'about:blank'],
  { stdio: ['ignore', 'ignore', 'ignore'] });

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function waitCdp() {
  for (let i = 0; i < 40; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/version`);
      return await r.json();
    } catch { await sleep(250); }
  }
  throw new Error('CDP 未就绪');
}

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

const ver = await waitCdp();
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
await S('Runtime.enable');
await S('Emulation.setDeviceMetricsOverride', {
  width: 1560, height: 1300, deviceScaleFactor: 2, mobile: false,
});

if (TOKEN) {
  await S('Page.addScriptToEvaluateOnNewDocument', {
    source: `try{localStorage.setItem('ems_platform_token',${JSON.stringify(TOKEN)});
             localStorage.setItem('ems_platform_user',${JSON.stringify(USER)});}catch(e){}`,
  });
} else {
  await S('Page.addScriptToEvaluateOnNewDocument', {
    source: `try{localStorage.removeItem('ems_platform_token');localStorage.removeItem('ems_platform_user');}catch(e){}`,
  });
}

// 探测页面上的「异常/降级」提示：红橙徽章、空态、错误提示条
const PROBE = `JSON.stringify((function(){
  var out = {bad:[], warn:[], empty:[], err:[]};
  function grab(sel, key, limit){
    document.querySelectorAll(sel).forEach(function(n){
      var t = (n.textContent||'').trim().replace(/\\s+/g,' ');
      if (t && out[key].length < limit) out[key].push(t.slice(0,60));
    });
  }
  grab('.b-bad', 'bad', 8);
  grab('.b-warn', 'warn', 8);
  grab('.empty', 'empty', 6);
  grab('.err, .error, .hint-err, .banner.bad, .banner.err', 'err', 6);
  out.badN = document.querySelectorAll('.b-bad').length;
  out.warnN = document.querySelectorAll('.b-warn').length;
  out.emptyN = document.querySelectorAll('.empty').length;
  out.errN = document.querySelectorAll('.err,.error,.hint-err').length;
  // 向导是否真的打开了、停在第几步（判据：DOM 里有没有节点，不是"我以为点到了"）
  var pills = document.querySelectorAll('.wz-pill');
  out.wzPillN = pills.length;
  var cur = document.querySelector('.wz-pill.cur');
  out.wzCur = cur ? (cur.getAttribute('title') || '') : '';
  var md = document.querySelector('.mdl');
  out.wzOpen = md ? (md.offsetParent !== null || getComputedStyle(md).display !== 'none') : false;
  out.wzCfg = document.body.innerHTML.indexOf('IOA 转发表') >= 0;
  return out;
})())`;

const report = [];
const probeOut = {};
for (const [hash, name, full, scrollEnd, prep] of PAGES) {
  const url = hash === 'login' ? BASE + '/' : `${BASE}/#/${hash}`;
  try {
    if (TALL[name]) {
      await S('Emulation.setDeviceMetricsOverride', {
        width: 1560, height: TALL[name], deviceScaleFactor: 2, mobile: false,
      });
    }
    await S('Page.navigate', { url });
    await sleep(3200);
    const prepSteps = Array.isArray(prep) ? prep : [prep];
    for (const js of prepSteps) {
      if (!js) continue;
      await S('Runtime.evaluate', { expression: js });
      await sleep( js === OPEN_WIZARD ? 3200 : 2600 );
    }
    if (scrollEnd) {
      await S('Runtime.evaluate', { expression:
        "var c=document.getElementById('content'); if(c){c.scrollTop=c.scrollHeight;}" });
      await sleep(1500);
    }
    const pb = await S('Runtime.evaluate', { expression: PROBE, returnByValue: true });
    probeOut[name] = JSON.parse(pb.result.value);
    const shot = await S('Page.captureScreenshot', {
      format: 'png', captureBeyondViewport: false, fromSurface: true,
    });
    const file = path.join(OUTDIR, name + '.png');
    fs.writeFileSync(file, Buffer.from(shot.data, 'base64'));
    const h = full ? await S('Runtime.evaluate', { expression: 'document.body.scrollHeight', returnByValue: true }) : null;
    const sz = fs.statSync(file).size;
    report.push(`${name}: ${Math.round(sz / 1024)} KB${full ? ' pageH=' + h.result.value : ''}`);
  } catch (e) {
    report.push(`${name}: FAIL ${e.message}`);
  }
}

console.log(report.join('\n'));
const PF = path.join(OUTDIR, '_probe.json');
fs.writeFileSync(PF, JSON.stringify(probeOut, null, 1));
for (const k of Object.keys(probeOut)) {
  const p = probeOut[k];
  const flag = (p.errN > 0 || p.emptyN > 0) ? ' <== 有异常提示' : '';
  console.log(`\n[${k}] bad=${p.badN} warn=${p.warnN} empty=${p.emptyN} err=${p.errN}${flag}`
    + (p.wzPillN !== undefined ? ` | 向导 pill=${p.wzPillN} open=${p.wzOpen} cur=「${p.wzCur}」 cfg=${p.wzCfg}` : ''));
  for (const t of p.err) console.log('    ERR  ' + t);
  for (const t of p.empty) console.log('    EMPTY ' + t);
  for (const t of p.bad.slice(0, 4)) console.log('    bad  ' + t);
  for (const t of p.warn.slice(0, 4)) console.log('    warn ' + t);
}
try { await send(ws, 'Browser.close'); } catch {}
ws.close();
chrome.kill();
