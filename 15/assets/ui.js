/* =====================================================================
   15/ — 通用渲染工具
   DOM 辅助 / 数字格式化 / 内联 SVG 图表（照 10/src/sim_report.h 的路数：
   手写 SVG，不引 JS 图表库，单文件离线可看）
   ===================================================================== */

/* ------------------------------- DOM ------------------------------- */
function h(tag, attrs, children) {
  const el = document.createElement(tag);
  if (attrs) {
    for (const [k, v] of Object.entries(attrs)) {
      if (v === null || v === undefined || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k === 'html') el.innerHTML = v;
      else if (k === 'text') el.textContent = v;
      else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
      else if (k === 'dataset') Object.assign(el.dataset, v);
      else el.setAttribute(k, v);
    }
  }
  if (children) {
    (Array.isArray(children) ? children : [children]).forEach(c => {
      if (c === null || c === undefined || c === false) return;
      el.appendChild(typeof c === 'string' || typeof c === 'number'
        ? document.createTextNode(String(c)) : c);
    });
  }
  return el;
}
function $(sel, root) { return (root || document).querySelector(sel); }
function $$(sel, root) { return Array.from((root || document).querySelectorAll(sel)); }
function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); return el; }

/* SVG 字符串 → 可插入节点（图表函数返回字符串，页面侧要换节点时用它）。
   ★ 为什么不走 'image/svg+xml' 的 XML 解析：XML 解析不会给无前缀的 <svg> 补命名空间
   —— MIME 类型不代表命名空间 —— import 进来会被当作未知元素，宽高塌成一行纯文本。
   这里改走 HTML 解析：它对 foreign content 会自动补上 SVG 命名空间，并按 SVG 规则
   修正 viewBox / preserveAspectRatio 等属性大小写，与页面里直接塞字符串那条路径同源。 */
function svgNode(s) {
  const doc = new DOMParser().parseFromString(s, 'text/html');
  return document.importNode(doc.body.firstElementChild, true);
}

/* ----------------------------- 格式化 ----------------------------- */
// ★ 统一判定"这个值能不能显示"：null / undefined / NaN / ±Infinity 都不可显示。
//   NaN 之外还要挡 Infinity —— 本项目的字序错曾产出天文数字 / 极小非规格化数，
//   后端若漏一个 Infinity，直接拼成 "Infinity kW" 会糊在脸上。
function _bad(v) { return v === null || v === undefined || (typeof v === 'number' && !Number.isFinite(v)); }

const fmt = {
  num(v, nd = 1) {
    if (_bad(v)) return '—';
    return Number(v).toFixed(nd);
  },
  signed(v, nd = 1) {
    if (_bad(v)) return '—';
    const n = Number(v);
    return (n > 0 ? '+' : '') + n.toFixed(nd);
  },
  kw(v, nd = 1) { return _bad(v) ? '—' : `${this.num(v, nd)} kW`; },
  kwh(v, nd = 1) { return _bad(v) ? '—' : `${this.num(v, nd)} kWh`; },
  pct(v, nd = 1) { return _bad(v) ? '—' : `${this.num(v, nd)} %`; },
  soc(v) { return _bad(v) ? '—' : `${this.num(Number(v) * 100, 1)} %`; },
  money(v, nd = 2) {
    if (_bad(v)) return '—';
    return Number(v).toLocaleString('zh-CN', { minimumFractionDigits: nd, maximumFractionDigits: nd });
  },
  hhmm(t_s) {
    if (_bad(t_s)) return '—';
    const m = Math.floor(Number(t_s) / 60);
    return `${String(Math.floor(m / 60) % 24).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
  },
  int(v) { return _bad(v) ? '—' : String(Math.round(Number(v))); },
};

/* ------------------------------ 徽章 ------------------------------ */
const STATE_KIND = {
  NORMAL: 'b-ok', READY: 'b-info', INIT: 'b-mute', SELF_CHECK: 'b-info',
  WARNING: 'b-warn', DERATED: 'b-warn', FAULT: 'b-bad', EMERGENCY: 'b-bad',
  HOLD_LAST: 'b-mute', OFFLINE: 'b-mute',
};
const LEVEL_KIND = {
  INFO: 'b-info', WARNING: 'b-warn', DERATED: 'b-warn',
  FAULT: 'b-bad', EMERGENCY: 'b-bad',
};
const RESULT_KIND = {
  ok: 'b-ok', clamped: 'b-warn', safety_clip: 'b-warn',
  gated: 'b-info', hold_last: 'b-mute',
};
const RESULT_TEXT = {
  ok: '正常', clamped: '区间夹紧', safety_clip: '安全削顶',
  gated: '门控归零', hold_last: '保持上一拍',
};

function badge(text, kind) {
  return h('span', { class: `badge ${kind || 'b-mute'}`, text: text || '—' });
}
function stateBadge(state) { return badge(state, STATE_KIND[state] || 'b-mute'); }
function levelBadge(level) { return badge(level, LEVEL_KIND[level] || 'b-mute'); }
function resultBadge(r) { return badge(RESULT_TEXT[r] || r, RESULT_KIND[r] || 'b-mute'); }

/* ---------------------------- 通知条 ---------------------------- */
let _toastTimer = null;
function toast(msg, kind) {
  const el = $('#toast');
  if (!el) return;
  el.textContent = msg;
  el.className = `toast ${kind || ''}`;
  if (_toastTimer) clearTimeout(_toastTimer);
  _toastTimer = setTimeout(() => el.classList.add('hidden'), 3600);
}

/* ------------------- 分段控件滑动指示条（.seg 通用） -------------------
   选中底色不画在按钮上，而是 .seg 里一块绝对定位的 pill（.seg-ind）：
   用 transform/width 对齐 active 按钮 —— 切换时底色平滑滑过去。
   ★ 为什么必须共用这一份：.seg button.active 只把文字刷白（color:#fff），
     **底色全靠这块 pill**。少了 pill 的地方就是"白字落在浅灰底上"，
     实时监控的倍速、报表系统的日报/周报/月报都栽过这个坑。
   ★ 必须在 .seg 挂进文档之后再调：要读 offsetWidth / offsetLeft，
     没挂载时两者都是 0，pill 会缩成看不见的一条。
   ★ 首次定位走 animate=false：先关过渡摆到位再恢复，避免从 (0,0) 飞入。 */
function ensureSegInd(seg) {
  let ind = seg.querySelector('.seg-ind');
  if (!ind) {
    ind = h('span', { class: 'seg-ind', 'aria-hidden': 'true' });
    seg.appendChild(ind);
  }
  return ind;
}
function moveSegInd(seg, animate) {
  if (!seg) return;
  const btn = seg.querySelector('button.active');
  const ind = ensureSegInd(seg);
  if (!btn) { ind.style.width = '0px'; return; }
  const place = () => {
    ind.style.width = btn.offsetWidth + 'px';
    ind.style.transform = `translateX(${btn.offsetLeft}px)`;
  };
  if (animate === false) {
    ind.style.transition = 'none';
    place();
    void ind.offsetWidth;        // 强制回流，让无过渡位置先生效
    ind.style.transition = '';
  } else {
    place();
  }
}
/* 把 root（默认整个文档）里所有 .seg 对一次位 —— 页面挂载后调一次即可 */
function placeSegs(root) {
  $$('.seg', root || document).forEach(s => moveSegInd(s, false));
}

/* ---------------------------- 卡片 ---------------------------- */
function card(title, sub, body, extraClass) {
  return h('div', { class: `card ${extraClass || ''}` }, [
    title ? h('div', { class: 'card-head' }, [
      h('h2', { text: title }),
      sub ? h('span', { class: 'sub', text: sub }) : null,
    ]) : null,
    body,
  ]);
}

function kpi(label, value, unit, note, kind, opts) {
  const o = opts || {};
  return h('div', { class: `kpi ${kind || ''}` }, [
    h('div', { class: 'kpi-top' }, [
      h('div', { class: 'label', text: label }),
      o.icon ? h('span', { class: 'kpi-ico', html: o.icon }) : null,
    ]),
    h('div', { class: 'value' }, [
      String(value),
      unit ? h('span', { class: 'unit', text: unit }) : null,
      o.pill ? h('span', { class: `pill ${o.pillKind || 'p-ok'}`, text: o.pill }) : null,
    ]),
    note ? h('div', { class: 'note', text: note }) : null,
  ]);
}

/* KPI 卡右上角小图标（stroke 用 currentColor，随 .kpi-ico 配色） */
const KPI_ICON = {
  power:  '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M13 2L4.5 13H11l-1 9 8.5-11H12l1-9z"/></svg>',
  battery:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="2" y="8" width="17" height="9" rx="2"/><path d="M22 11v3"/><path d="M6 11v3M10 11v3"/></svg>',
  coin:   '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="6" width="20" height="12" rx="2"/><circle cx="12" cy="12" r="2.5"/></svg>',
  shield: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3l7 3v5c0 4.5-3 8.5-7 10-4-1.5-7-5.5-7-10V6l7-3z"/><path d="M9.5 12l2 2 3.5-4"/></svg>',
};

/* ---------------------------- 表格 ---------------------------- */
function table(cols, rows, opts) {
  opts = opts || {};
  const thead = h('thead', null, h('tr', null, cols.map(c =>
    h('th', { class: c.num ? 'num' : '', text: c.title }))));
  const tbody = h('tbody', null, rows.length ? rows.map(r =>
    h('tr', null, cols.map(c => {
      const v = c.render ? c.render(r) : r[c.key];
      const td = h('td', { class: [c.num ? 'num' : '', c.mono ? 'mono' : ''].join(' ') });
      if (v instanceof Node) td.appendChild(v);
      else td.textContent = (v === null || v === undefined || v === '') ? '—' : String(v);
      return td;
    }))
  ) : h('tr', null, h('td', { colspan: cols.length }, h('div', { class: 'empty', text: opts.empty || '暂无数据' }))));
  const t = h('table', { class: 'tbl' }, [thead, tbody]);
  return opts.wrap === false ? t : h('div', { class: 'tbl-wrap' }, t);
}

/* =====================================================================
   内联 SVG 折线图
   series: [{ name, color, values: [] }]
   ===================================================================== */
function lineChart(cfg) {
  const W = 900, H = cfg.height || 250;
  const padL = 54, padR = 14, padT = 12, padB = 26;
  const iw = W - padL - padR, ih = H - padT - padB;
  const series = (cfg.series || []).filter(s => s && s.values && s.values.length);
  if (!series.length) return `<svg viewBox="0 0 ${W} ${H}" class="chart"></svg>`;

  const n = Math.max(...series.map(s => s.values.length));
  const all = series.flatMap(s => s.values).filter(v => v !== null && v !== undefined && !Number.isNaN(v));
  let yMin = Math.min(...all), yMax = Math.max(...all);
  if (cfg.yMin !== undefined) yMin = Math.min(yMin, cfg.yMin);
  if (cfg.yMax !== undefined) yMax = Math.max(yMax, cfg.yMax);
  if (yMin > 0) yMin = 0;
  if (yMax < 0) yMax = 0;
  if (yMax === yMin) { yMax += 1; yMin -= 1; }
  const pad = (yMax - yMin) * 0.1;
  yMin -= pad; yMax += pad;

  const X = i => padL + (n === 1 ? iw / 2 : iw * i / (n - 1));
  const Y = v => padT + ih * (1 - (v - yMin) / (yMax - yMin));

  const out = [];
  out.push(`<svg viewBox="0 0 ${W} ${H}" class="chart" preserveAspectRatio="none" role="img">`);

  // 网格 + y 轴刻度
  const TICKS = 5;
  for (let i = 0; i <= TICKS; i++) {
    const v = yMin + (yMax - yMin) * i / TICKS;
    const y = Y(v);
    out.push(`<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}"
      stroke="var(--line)" stroke-width="1"/>`);
    out.push(`<text x="${padL - 8}" y="${(y + 4).toFixed(1)}" text-anchor="end"
      font-size="11" fill="var(--text-3)">${v.toFixed(Math.abs(yMax - yMin) > 50 ? 0 : 2)}</text>`);
  }
  // 零线
  if (yMin < 0 && yMax > 0) {
    out.push(`<line x1="${padL}" y1="${Y(0).toFixed(1)}" x2="${W - padR}" y2="${Y(0).toFixed(1)}"
      stroke="var(--accent)" stroke-width="1.2" stroke-dasharray="4 3"/>`);
  }
  // x 轴标签
  const labels = cfg.xLabels || [];
  if (labels.length) {
    const step = Math.max(1, Math.floor(labels.length / 8));
    for (let i = 0; i < labels.length; i += step) {
      out.push(`<text x="${X(i).toFixed(1)}" y="${H - 8}" text-anchor="middle"
        font-size="11" fill="var(--text-3)">${labels[i]}</text>`);
    }
  }
  // 曲线
  series.forEach(s => {
    const pts = s.values.map((v, i) =>
      (v === null || v === undefined) ? null : `${X(i).toFixed(1)},${Y(v).toFixed(1)}`)
      .filter(Boolean).join(' ');
    if (!pts) return;
    if (s.area) {
      out.push(`<polygon points="${X(0).toFixed(1)},${Y(0).toFixed(1)} ${pts} ${X(s.values.length - 1).toFixed(1)},${Y(0).toFixed(1)}"
        fill="${s.color}" opacity="0.10"/>`);
    }
    out.push(`<polyline points="${pts}" fill="none" stroke="${s.color}"
      stroke-width="${s.width || 1.7}" stroke-linejoin="round" stroke-linecap="round"
      ${s.dash ? `stroke-dasharray="${s.dash}"` : ''}/>`);
  });

  // 游标（可选）：竖线 + tooltip，默认隐藏；由页面侧绑定 mousemove 驱动。
  // 只放结构，不塞任何接口数据字符串 —— 数值由页面用 textContent 回填。
  // 每系列一个 dot，其 data-points 存该系列所有点的 "x,y x,y …" 坐标，
  // 供页面侧 mousemove 反查精确 y（避免页面重复算 yMin/yMax）。
  if (cfg.cursor) {
    out.push(`<line class="chart-cursor-line" x1="${padL}" y1="${padT}" x2="${padL}" y2="${H - padB}"
      stroke="var(--text-3)" stroke-width="1" stroke-dasharray="3 3" style="display:none"/>`);
    series.forEach((s, si) => {
      const coords = s.values.map((v, i) =>
        (v === null || v === undefined) ? '' : `${X(i).toFixed(1)},${Y(v).toFixed(1)}`).join(' ');
      out.push(`<circle class="chart-cursor-dot" data-si="${si}" data-points="${coords}"
        cx="${padL}" cy="${padT}" r="3.5"
        fill="${s.color}" stroke="var(--card)" stroke-width="1.5" style="display:none"/>`);
    });
    out.push(`<g class="chart-cursor-tip" style="display:none">
      <rect x="0" y="4" width="10" height="16" rx="4" fill="var(--text)" opacity="0.9"/>
      <text class="chart-cursor-tip-text" x="5" y="15" text-anchor="middle"
        font-size="11" fill="var(--card)"></text>
    </g>`);
  }

  out.push('</svg>');
  return out.join('');
}

function chartLegend(series) {
  return h('div', { class: 'legend' }, series.map(s =>
    h('span', null, [h('i', { style: `background:${s.color}` }), h('span', { text: s.label || s.name })])));
}

/* =====================================================================
   迷你走势条（窄栏 / 小卡专用）
   为什么不复用 lineChart：它按 900 单位宽出图，塞进 300~400 px 的侧栏会被
   整体缩到 ~0.4 倍，11 px 刻度字变成 4 px 的糊点。这里 viewBox 就按实际
   像素量级取（360×H），字号/线宽基本 1:1，不缩水。
   cfg = { values, labels, color, height, marker: { i, label } }
   ===================================================================== */
function sparkChart(cfg) {
  const W = 360, H = cfg.height || 96;
  const padL = 32, padR = 10, padT = 18, padB = 16;
  const iw = W - padL - padR, ih = H - padT - padB;
  const n = (cfg.values || []).length;
  const nums = (cfg.values || []).filter(v => v !== null && v !== undefined);
  if (n < 2 || nums.length < 2) return `<svg viewBox="0 0 ${W} ${H}" class="chart"></svg>`;

  let yMin = Math.min(...nums), yMax = Math.max(...nums);
  const pad = ((yMax - yMin) * 0.18) || 1;
  yMin -= pad; yMax += pad;
  // 量本身有天然上下界时（如 SOC 0~100%）由调用方钉住，免得刻度出现负百分比
  if (cfg.yMin !== undefined) yMin = cfg.yMin;
  if (cfg.yMax !== undefined) yMax = cfg.yMax;
  const X = i => padL + iw * i / (n - 1);
  const Y = v => padT + ih * (1 - (v - yMin) / (yMax - yMin));

  const out = [`<svg viewBox="0 0 ${W} ${H}" class="chart" role="img">`];
  for (let i = 0; i <= 2; i++) {
    const v = yMin + (yMax - yMin) * i / 2, y = Y(v);
    out.push(`<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}"
      stroke="var(--line)" stroke-width="1"/>`);
    out.push(`<text x="${padL - 6}" y="${(y + 3.5).toFixed(1)}" text-anchor="end"
      font-size="10" fill="var(--text-3)">${v.toFixed(0)}</text>`);
  }
  const pts = (cfg.values || []).map((v, i) =>
    (v === null || v === undefined) ? null : `${X(i).toFixed(1)},${Y(v).toFixed(1)}`)
    .filter(Boolean).join(' ');
  if (pts) {
    out.push(`<polygon points="${X(0).toFixed(1)},${Y(yMin).toFixed(1)} ${pts} ${X(n - 1).toFixed(1)},${Y(yMin).toFixed(1)}"
      fill="${cfg.color}" opacity="0.10"/>`);
    out.push(`<polyline points="${pts}" fill="none" stroke="${cfg.color}" stroke-width="1.7"
      stroke-linejoin="round" stroke-linecap="round"/>`);
  }
  // x 轴只放首/中/尾三个时刻 —— 窄栏塞不下更多，密了就成麻
  const lb = cfg.labels || [];
  if (lb.length) {
    [[0, 'start'], [Math.floor((n - 1) / 2), 'middle'], [n - 1, 'end']].forEach(([i, anc]) => {
      out.push(`<text x="${X(i).toFixed(1)}" y="${H - 4}" text-anchor="${anc}"
        font-size="10" fill="var(--text-3)">${lb[i] || ''}</text>`);
    });
  }
  // 当前拍标记：竖虚线 + 曲线上的点 + 顶部时刻小签
  if (cfg.marker && cfg.marker.i != null) {
    const mi = Math.max(0, Math.min(n - 1, Math.round(cfg.marker.i)));
    const mx = X(mi), mv = cfg.values[mi];
    out.push(`<line x1="${mx.toFixed(1)}" y1="${(padT - 4).toFixed(1)}" x2="${mx.toFixed(1)}"
      y2="${(H - padB).toFixed(1)}" stroke="var(--accent)" stroke-width="1.2"
      stroke-dasharray="3 3" opacity="0.65"/>`);
    if (mv !== null && mv !== undefined) {
      out.push(`<circle cx="${mx.toFixed(1)}" cy="${Y(mv).toFixed(1)}" r="3.4"
        fill="${cfg.color}" stroke="var(--card)" stroke-width="1.6"/>`);
    }
    if (cfg.marker.label) {
      const lw = 42, lx = Math.min(Math.max(mx - lw / 2, 2), W - lw - 2);
      out.push(`<g><rect x="${lx.toFixed(1)}" y="1" width="${lw}" height="15" rx="7" fill="var(--accent)"/>
        <text x="${(lx + lw / 2).toFixed(1)}" y="12" text-anchor="middle"
          font-size="10.5" fill="#FFFFFF">${cfg.marker.label}</text></g>`);
    }
  }
  out.push('</svg>');
  return out.join('');
}

/* =====================================================================
   能量流图：几何常量 + "这一拍该画什么"
   为什么要抽出来：实时监控页边播放边重渲染（每 120ms 一拍）。能量流卡若跟着
   整张重建，箭头里的流光动画就是**新元素、从 0 相位起步** —— 看到的是胶囊被
   反复拽回起点，也就是"一播放就卡顿"。所以拆成两条路径、共用同一份计算：
     energyFlow(d)         —— 画整张 SVG（首帧 / 结构变了才重来）
     energyFlowPatch(el,d) —— 只改数字（每拍），元素不动、动画不重启
   两条路径都读 flowValues(d)，不会出现"重建的图和补丁改的数字不一致"。
   ===================================================================== */
const FLOW_W = 640, FLOW_H = 344;
const FLOW_SOC = { w: 260, h: 10, y: FLOW_H - 44 };   // 底部 SOC 条
/* 光带一个循环走完一个 dash 周期（= 30 + 70 = 100 px）；
   ★ 这个毫秒数必须与 app.css 里 .flow-sweep 的动画周期一致。 */
const FLOW_SWEEP_MS = 1500;

function flowValues(d) {
  const pv = d.pv_kw || 0;
  const bat = d.battery_kw || 0;   // >0 放电，<0 充电
  const load = d.load_kw || 0;
  const grid = d.grid_kw || 0;     // >0 从电网取电
  // 能量守恒小结：进母线 = 出母线
  const supply = (pv > 0 ? pv : 0) + (bat > 0 ? bat : 0) + (grid > 0 ? grid : 0);
  const demand = load + (bat < 0 ? -bat : 0) + (grid < 0 ? -grid : 0);
  const socPct = Math.max(0, Math.min(1, d.soc || 0));
  return {
    pv, bat, load, grid, socPct,
    socColor: socPct > 0.4 ? 'var(--ok)' : socPct > 0.2 ? 'var(--warn)' : 'var(--bad)',
    sumText: `进 ${fmt.num(supply, 1)} kW · 出 ${fmt.num(demand, 1)} kW · 差 ${fmt.num(supply - demand, 1)} kW`,
  };
}

/* 只改数字（不动元素）：数字文本 + SOC 条宽度/颜色 + 守恒小结。
   结构（哪几路有功率、朝哪边）变了要整张重建 —— 见 pages.js 的 flowSig。 */
function energyFlowPatch(svg, d) {
  if (!svg) return false;
  const v = flowValues(d);
  const q = (k) => svg.querySelector(`[data-k="${k}"]`);
  const setText = (k, t) => { const el = q(k); if (el) el.textContent = t; };
  setText('pv', fmt.num(v.pv, 1));
  setText('bat', fmt.num(Math.abs(v.bat), 1));
  setText('load', fmt.num(v.load, 1));
  setText('grid', fmt.num(Math.abs(v.grid), 1));
  setText('sum', v.sumText);
  const bar = q('soc-bar');
  if (bar) {
    bar.setAttribute('width', (FLOW_SOC.w * v.socPct).toFixed(1));
    bar.setAttribute('fill', v.socColor);
  }
  const st = q('soc-txt');
  if (st) { st.textContent = fmt.soc(d.soc); st.setAttribute('fill', v.socColor); }
  return true;
}

/* =====================================================================
   能量流图
   布局：左（光伏 / 储能）—— 中间母线 —— 右（负荷 / 电网）
   约定：储能放电为正（>0 向母线送电）；电网取电为正（>0 从电网受电）
   ===================================================================== */
function energyFlow(d) {
  const W = FLOW_W, H = FLOW_H;
  const cx = W / 2;
  /* 母线（等轴测 3D 汇流柱）的屏幕落点：底台中心在这条基线上。
     柱身屏幕跨度约 64..259，与原竖直细条（58..250）同一段。 */
  const busBaseY = 246, busH = 168, busHalf = 16;
  /* 箭头两端各留出的空隙 —— 不留的话箭头端面就贴死在插画/柱身上，看着是"叠上去"：
       节点侧 gap   = 58（地块顶点在 52，塔身导线伸到约 54）；
       母线侧 busGap = 10（柱身轮廓在 ±16，顶盖 ±21，底台 ±30）。
     ★ 母线侧退到 ±26 之后，箭头端面既离开了柱身边线，也整个落在顶盖的
       水平范围（±21）之外 —— 顶盖与底台都比柱身宽，箭头贴到柱边时会被这两片
       读成"插进母线里"，退开 10 才干净。 */
  const gap = 58, busGap = 10;

  const v = flowValues(d);
  const { pv, bat, load, grid } = v;

  // ★ 色值统一走 CSS 变量，与 --c-load/--c-pv/--c-grid/--c-bat 同源。
  const C_LOAD = 'var(--c-load)', C_PV = 'var(--c-pv)',
        C_GRID = 'var(--c-grid)', C_BAT = 'var(--c-bat)', C_BUS = 'var(--accent)';

  /* —— 粗箭头（潮流主干）——
     原来是「细虚线 + 小三角 marker」，细到只能靠虚线密度表达方向；
     现在是**实心粗箭头**（杆 + 箭头合成一个多边形），方向由形状本身说清。
     ★ 体量口径：杆厚 16、箭头半高 12 —— 够粗到能读，又不至于盖住设备插画
     （早先 21/15 的版本端面会压到储能柜与铁塔导线）。
     动态感不再靠"虚线在跑"，改用杆内一条**流动光带**（.flow-sweep，CSS 驱动）——
     半透明白色圆头短划沿流向滑动，看到的是「能量在流」，不是「这是一条虚线」。
     方向字（停机/用电/充电/购电…）就落在杆里：字在杆内，视线一步就能对上。
     调用方保证 x1→x2 就是潮流方向；四路箭头都是水平的（y1 == y2）。 */
  const AT = 16, AH = 14, AHH = 12;     // 杆厚 / 箭头长 / 箭头半高
  const arrow = (x1, x2, y, color, active, label, key) => {
    const right = x2 >= x1;
    const base = x2 - (right ? AH : -AH);          // 箭头底 = 杆的收尾
    const t = AT / 2;
    const poly = [[x1, y - t], [base, y - t], [base, y - AHH], [x2, y],
                  [base, y + AHH], [base, y + t], [x1, y + t]]
      .map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ');
    /* 光带：粗描边 + 圆头短划，dashoffset 动画沿流向滑。
       ★ 线的画法：**一律从箭尾 x1 画到 base**（base 落在箭尾与箭头之间），
         所以线自身的正方向就已经等于潮流方向 —— 动画只要固定往负方向走，
         短划就是朝箭头去的，箭头朝左朝右都对（详见 app.css 的方向口径说明）。
       ★ 负延迟 = 相位跟"页面时刻"走，不跟元素寿命走：整张图重建时新元素直接
         落在正确相位，不会从 0 重来（否则播放时看着就是被反复拽回起点）。 */
    const sweep = active
      ? `<line x1="${x1}" y1="${y}" x2="${base}" y2="${y}"
           class="flow-sweep"
           style="animation-delay:-${(performance.now() % FLOW_SWEEP_MS).toFixed(0)}ms"
           stroke="var(--card)" stroke-width="${AT - 7}" stroke-linecap="round"
           stroke-dasharray="30 70" opacity="0.5"/>`
      : '';
    // 有功率：白字 + 深色细描边（压在任何一路量测色上都读得清）；
    // 闲置：字用该路量测色，压在淡描边空杆上。
    const txt = active
      ? `fill="var(--card)" stroke="rgba(15,23,42,.42)" stroke-width="2.2" paint-order="stroke"`
      : `fill="${color}"`;
    const mx = ((x1 + base) / 2).toFixed(1);
    return `
    <g class="flow-arrow" data-ch="${key || ''}">
      <polygon points="${poly}" fill="${color}" opacity="${active ? 0.9 : 0.15}"
        stroke="${color}" stroke-width="1.2" stroke-opacity="${active ? 0 : 0.5}"
        stroke-linejoin="round"/>
      ${sweep}
      <text x="${mx}" y="${(y + 3.6).toFixed(1)}" text-anchor="middle" font-size="11"
        font-weight="700" letter-spacing="1" ${txt}>${label}</text>
    </g>`;
  };

  /* —— 等轴测（2:1）3D 场景 ——
     坐标系：u 向右后、v 向左后、z 向上；屏幕 x = u - v，y = (u + v)/2 - z。
     面的明暗不另建色号：同一节点色叠三档透明度（顶亮 / 左中 / 右深），
     压在白卡上就是三档明度 —— 换主题色不用改一行画法。
     每个场景自带立体地块，物件按"远处先画"排（u+v 小的先画）。 */
  const ip = (u, v, z) => [u - v, (u + v) * 0.5 - z];
  const P = (arr) => arr.map(([u, v, z]) => {
    const p = ip(u, v, z);
    return `${p[0].toFixed(1)},${p[1].toFixed(1)}`;
  }).join(' ');
  const L = (x1, y1, x2, y2, stroke, w, op) =>
    `<line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}"
      stroke="${stroke}" stroke-width="${w}" stroke-linecap="round" opacity="${op}"/>`;
  const lineC = (p1, p2, stroke, w, op) => L(p1[0], p1[1], p2[0], p2[1], stroke, w, op);

  // 立体地块：顶面菱形 + 近端两条厚度侧面（只画一个菱形像贴纸，加了厚度才像"一块地"）
  const plate = (c, h, th) => `
    <polygon points="${P([[-h, h, 0], [h, h, 0], [h, h, -th], [-h, h, -th]])}" fill="${c}" opacity="0.40"/>
    <polygon points="${P([[h, -h, 0], [h, h, 0], [h, h, -th], [h, -h, -th]])}" fill="${c}" opacity="0.26"/>
    <polygon points="${P([[-h, -h, 0], [h, -h, 0], [h, h, 0], [-h, h, 0]])}" fill="${c}" opacity="0.13"/>`;

  // 长方体：底面中心 (u0,v0)，底边 du×dv，自 z0 起高 h，五面取三（可见面）
  const box = (c, u0, v0, du, dv, h, z0, o) => {
    o = o || {};
    const a = u0 - du / 2, b = u0 + du / 2, p = v0 - dv / 2, q = v0 + dv / 2, z = z0 || 0;
    return `
    <polygon points="${P([[a, q, z + h], [b, q, z + h], [b, q, z], [a, q, z]])}" fill="${c}" opacity="${o.l || 0.60}"/>
    <polygon points="${P([[b, p, z + h], [b, q, z + h], [b, q, z], [b, p, z]])}" fill="${c}" opacity="${o.r || 0.92}"/>
    <polygon points="${P([[a, p, z + h], [b, p, z + h], [b, q, z + h], [a, q, z + h]])}" fill="${c}" opacity="${o.t || 0.30}"/>`;
  };

  // 落在平面上的软阴影（椭圆按 2:1 压扁，与地面同角度）
  const shadow = (u, v, r) => {
    const p = ip(u, v, 0);
    return `<ellipse cx="${p[0].toFixed(1)}" cy="${p[1].toFixed(1)}" rx="${r.toFixed(1)}"
      ry="${(r * 0.5).toFixed(1)}" fill="var(--text)" opacity="0.085"/>`;
  };

  // 贴面的白色闪电标（屏幕坐标平移缩放；不做面投影也读得出是储能）
  const bolt = (x, y, s, op) => `<polygon transform="translate(${x.toFixed(1)},${y.toFixed(1)}) scale(${s})"
    points="1.8,-8.4 -5.3,1.4 -1.1,1.4 -3.2,8.4 5.3,-0.7 0.4,-0.7" fill="var(--card)" opacity="${op || 0.92}"/>`;

  /* 尺寸口径：地块 h=26 → 屏幕 ±52 宽、±26 高（2:1 压扁），厚度 6。
     物件一律落在 u,v ∈ [-26,26] 内才不越出地块；按 u+v 由小到大画 = 远到近。 */

  // 光伏：2×2 倾斜板（后缘抬高 = 朝阳，白格线 = 电池片）
  const scenePv = (c) => {
    const panel = (u0, v0, du, dv, tilt) => {
      const a = u0 - du / 2, b = u0 + du / 2, p = v0 - dv / 2, q = v0 + dv / 2;
      const post = (u) => lineC(ip(u, p, 0), ip(u, p, tilt), c, 1.4, 0.5);
      const cells = [1 / 3, 2 / 3].map(k =>
        lineC(ip(a + du * k, q, 0), ip(a + du * k, p, tilt), 'var(--card)', 0.85, 0.85)).join('');
      return `${post(a + 2.5)}${post(b - 2.5)}
        <polygon points="${P([[a, q, 0], [b, q, 0], [b, p, tilt], [a, p, tilt]])}" fill="${c}" opacity="0.5"/>
        ${cells}`;
    };
    return `
      ${plate(c, 26, 6)}
      ${shadow(-11, -13, 14)}${shadow(10, 13, 14)}
      ${panel(-12, -14, 22, 14, 9)}
      ${panel(10, -14, 22, 14, 9)}
      ${panel(-12, 12, 22, 14, 9)}
      ${panel(10, 12, 22, 14, 9)}`;
  };

  // 储能：电池集装箱（白舱线分舱 + 侧面闪电标）+ PCS 变流柜
  const sceneBat = (c) => {
    const q = 5, fc = ip(-13, q, 8);
    const bays = [0.2, 0.4, 0.6, 0.8].map(k =>
      lineC(ip(-23 + 40 * k, q, 0), ip(-23 + 40 * k, q, 13), 'var(--card)', 1.1, 0.85)).join('');
    return `
      ${plate(c, 26, 6)}
      ${shadow(-3, -4, 19)}${shadow(15, 14, 8)}
      ${box(c, -3, -4, 40, 18, 13)}
      ${bays}
      ${bolt(fc[0], fc[1], 0.95)}
      ${box(c, 15, 14, 16, 11, 10, 0, { r: 0.72, t: 0.32 })}`;
  };

  // 负荷：主厂房（近端面开亮窗）+ 屋顶二层 + 烟囱冒烟 + 侧附楼
  const sceneLoad = (c) => {
    const winL = (v, u1, u2, z1, z2) =>
      `<polygon points="${P([[u1, v, z2], [u2, v, z2], [u2, v, z1], [u1, v, z1]])}" fill="var(--card)" opacity="0.9"/>`;
    const winR = (u, v1, v2, z1, z2) =>
      `<polygon points="${P([[u, v1, z2], [u, v2, z2], [u, v2, z1], [u, v1, z1]])}" fill="var(--card)" opacity="0.9"/>`;
    const puff = (u, v, z, r, op) => {
      const p = ip(u, v, z);
      return `<circle cx="${p[0].toFixed(1)}" cy="${p[1].toFixed(1)}" r="${r}" fill="${c}" opacity="${op}"/>`;
    };
    return `
      ${plate(c, 26, 6)}
      ${shadow(-2, -2, 18)}${shadow(16, 13, 7)}
      ${box(c, -2, -2, 34, 20, 12)}
      ${box(c, 11, -8, 9, 9, 22, 12, { r: 0.86, t: 0.24 })}
      ${puff(11, -8, 36, 2.6, 0.22)}${puff(10, -9, 41, 3.4, 0.14)}
      ${box(c, -4, -3, 22, 12, 8, 12, { r: 0.82, t: 0.24 })}
      ${winL(8, -15, -9, 3, 8)}${winL(8, -5, 1, 3, 8)}${winL(8, 5, 11, 3, 8)}
      ${winR(15, -8, -2, 3, 8)}${winR(15, 2, 7, 3, 8)}
      ${box(c, 16, 13, 14, 10, 8, 0, { r: 0.70, t: 0.30 })}
      ${winR(23, 11, 16, 2.5, 6)}`;
  };

  // 电网：格构铁塔（四腿 + 层间 X 斜撑，不画实心锥面）+ 双层横担 + 绝缘子 + 两侧弧垂导线
  const sceneGrid = (c) => {
    const HB = 13, HT = 4, HZ = 24, LV = [0, 6, 12, 18, 24];
    const w = (z) => HB - (HB - HT) * z / HZ;       // 该高度的半宽（塔身收分）
    const leg = (su, sv, op) => lineC(ip(su * HB, sv * HB, 0), ip(su * HT, sv * HT, HZ), c, 1.5, op);
    const rung = (z) => {
      const k = w(z);
      return lineC(ip(-k, k, z), ip(k, k, z), c, 1, 0.8) + lineC(ip(k, -k, z), ip(k, k, z), c, 1, 0.8);
    };
    const brace = (z1, z2) => {
      const k1 = w(z1), k2 = w(z2);
      return lineC(ip(-k1, k1, z1), ip(k2, k2, z2), c, 0.9, 0.7)
           + lineC(ip(k1, k1, z1), ip(-k2, k2, z2), c, 0.9, 0.7)
           + lineC(ip(k1, -k1, z1), ip(k2, k2, z2), c, 0.9, 0.7);
    };
    const wire = (u, v, z, x2, y2) => {
      const p = ip(u, v, z);
      const mx = (p[0] + x2) / 2, my = Math.max(p[1], y2) + 13;
      return `<path d="M ${p[0].toFixed(1)} ${p[1].toFixed(1)} Q ${mx.toFixed(1)} ${my.toFixed(1)} ${x2} ${y2}"
        fill="none" stroke="${c}" stroke-width="1.1" opacity="0.45"/>`;
    };
    const ins = (u, z) => box(c, u, 0, 4, 4, 4.5, z - 4.5, { r: 0.8, t: 0.4 });
    return `
      ${plate(c, 26, 6)}
      ${shadow(0, 0, 13)}
      ${wire(-14, 0, 24, -54, 9)}${wire(14, 0, 24, 54, 9)}
      ${wire(-17, 0, 18, -47, 16)}${wire(17, 0, 18, 47, 16)}
      ${leg(-1, -1, 0.45)}${leg(1, -1, 0.45)}
      ${LV.slice(0, -1).map((z, i) => rung(z) + brace(z, LV[i + 1])).join('')}
      ${leg(-1, 1, 0.95)}${leg(1, 1, 0.95)}
      <polygon points="${P([[-HT, -HT, HZ], [HT, -HT, HZ], [HT, HT, HZ], [-HT, HT, HZ]])}" fill="${c}" opacity="0.45"/>
      ${box(c, 0, 0, 34, 5, 2.2, 16, { r: 0.88, t: 0.36 })}
      ${box(c, 0, 0, 28, 5, 2.2, 21.8, { r: 0.88, t: 0.36 })}
      ${ins(-15, 16)}${ins(15, 16)}${ins(-12, 21.8)}${ins(12, 21.8)}`;
  };

  /* 交流母线：等轴测 3D 汇流柱（与四个场景同一套投影 / 明暗规则）。
     为什么能做：本投影里屏幕 x = u - v **与高度 z 无关**，所以柱身的左右轮廓
     在屏幕上恒为竖直线 —— 四路水平箭头照旧能精确接在柱边上。
     底台 + 柱身分节（每节一道 V 形束带）+ 顶盖，读作"一条立起来的汇流排"。 */
  const sceneBus = (c) => {
    const ribs = [0.25, 0.5, 0.75].map(k => {
      const z = busH * k;
      return lineC(ip(-8, 8, z), ip(8, 8, z), 'var(--card)', 1.2, 0.5)
           + lineC(ip(8, 8, z), ip(8, -8, z), 'var(--card)', 1.2, 0.5);
    }).join('');
    return `
      ${plate(c, 15, 5)}
      ${shadow(0, 0, 15)}
      ${box(c, 0, 0, 16, 16, busH, 0)}
      ${ribs}
      ${box(c, 0, 0, 21, 21, 7, busH - 1, { r: 0.8, t: 0.42 })}`;
  };

  const SCENES = { pv: scenePv, bat: sceneBat, load: sceneLoad, grid: sceneGrid };

  // 节点 = 场景插画 + 外侧名称 + 下方数值；闲置时场景整组淡显。
  // side='L' 名称在图案左侧（右对齐朝图案收拢），'R' 在右侧。
  // ★ 方向字（发电/停机/充电/用电…）**不在这里** —— 它挪进了箭头杆里，
  //   见 arrow()；节点侧边只留名称，读数与设备名仍贴着图案。
  // ★ key = data-k，供 energyFlowPatch() 原地改数字（元素不动 = 动画不重启）。
  const node = (x, ry, kind, label, value, color, active, side, key) => {
    const tx = side === 'L' ? x - 58 : x + 58;
    const anchor = side === 'L' ? 'end' : 'start';
    return `
    <g class="flow-node">
      <g transform="translate(${x}, ${ry})" opacity="${active ? 1 : 0.35}">${SCENES[kind](color)}</g>
      <text x="${tx}" y="${ry + 5}" text-anchor="${anchor}" font-size="12.5"
        font-weight="600" fill="var(--text-2)" letter-spacing="1">${label}</text>
      <text${key ? ` data-k="${key}"` : ''} x="${x}" y="${ry + 46}" text-anchor="middle"
        font-size="19" font-weight="600"
        fill="${color}" font-family="ui-monospace,Consolas,monospace">${value}</text>
    </g>`;
  };

  const s = [];
  s.push(`<svg viewBox="0 0 ${W} ${H}" class="chart flow" role="img">`);
  s.push(`<title>能量流</title><desc>母线两侧的能量流向</desc>`);

  // 交流母线：等轴测 3D 汇流柱（与四节点同一套投影规则）；
  // 标签竖排"每行一字"（字正、不旋转），居中压在柱身上，白晕保证不糊。
  s.push(`<g transform="translate(${cx}, ${busBaseY})" class="flow-bus">${sceneBus(C_BUS)}</g>`);
  const busLabel = ['交', '流', '母', '线'];
  const busMid = busBaseY - busH / 2;
  s.push(busLabel.map((ch, i) => `<text x="${cx}" y="${(busMid + (i - 1.5) * 16 + 4).toFixed(1)}"
    text-anchor="middle" font-size="12.5" font-weight="700" fill="var(--card)"
    stroke="${C_BUS}" stroke-width="3.4" paint-order="stroke" stroke-linejoin="round">${ch}</text>`).join(''));

  // 连线（先画箭头，场景画在箭头端面之上）
  // 节点横位 102：给外侧名称腾出画布边距；
  // 箭头尾端收到 Lx±gap（不是地块顶点 Lx±52）—— 再留 6px，粗箭头的端面才不压
  // 储能柜的斜面与铁塔垂下来的导线；母线端收到 cx±(busHalf+busGap)，
  // 同样退开一个空隙，端面不贴死在柱身轮廓线上。
  const pvY = 92, batY = 218, loadY = 92, gridY = 218;
  const Lx = 102, Rx = W - 102;
  const nearL = Lx + gap, nearR = Rx - gap;
  const busL = cx - busHalf - busGap, busR = cx + busHalf + busGap;

  s.push(arrow(nearL, busL, pvY, C_PV, pv > 0.5, pv > 0.5 ? '发电' : '停机', 'pv'));
  // 储能：放电(>0) 储能→母线(向右)；充电(<0) 母线→储能(向左)
  s.push(bat < -0.5
    ? arrow(busL, nearL, batY, C_BAT, true, '充电', 'bat')
    : arrow(nearL, busL, batY, C_BAT, bat > 0.5, bat > 0.5 ? '放电' : '静置', 'bat'));
  s.push(arrow(busR, nearR, loadY, C_LOAD, load > 0.5, load > 0.5 ? '用电' : '无载', 'load'));
  // 电网：取电(>0) 电网→母线（向左，买电）；送电(<0) 母线→电网（向右，卖电）
  // ★ 符号口径（14/ 实测）：`P_grid_kW > 0` = 关口从电网受电（pv+bat+grid ≈ load 成立），
  //   所以箭头端点与另外三路同一条规矩 —— 源指向母线、母线指向汇。
  s.push(grid < -0.5
    ? arrow(busR, nearR, gridY, C_GRID, true, '售电', 'grid')
    : arrow(nearR, busR, gridY, C_GRID, Math.abs(grid) > 0.5,
            grid > 0.5 ? '购电' : '断开', 'grid'));

  // 节点（等轴测场景版；左列名称挂图案左侧，右列挂右侧，一律朝画布外缘）
  const batOn = Math.abs(bat) > 0.5, gridOn = Math.abs(grid) > 0.5;
  s.push(node(Lx, pvY, 'pv', '光伏', fmt.num(pv, 1), C_PV, pv > 0.5, 'L', 'pv'));
  s.push(node(Lx, batY, 'bat', '储能', fmt.num(Math.abs(bat), 1), C_BAT, batOn, 'L', 'bat'));
  s.push(node(Rx, loadY, 'load', '负荷', fmt.num(load, 1), C_LOAD, load > 0.5, 'R', 'load'));
  s.push(node(Rx, gridY, 'grid', '电网', fmt.num(Math.abs(grid), 1), C_GRID, gridOn, 'R', 'grid'));

  // 底部：SOC 进度 + 能量守恒小结（几何口径与 energyFlowPatch 共用 FLOW_SOC）
  const barW = FLOW_SOC.w, barX = cx - barW / 2, barY = FLOW_SOC.y;
  const socPct = v.socPct, socColor = v.socColor;
  s.push(`<rect x="${barX}" y="${barY}" width="${barW}" height="${FLOW_SOC.h}" rx="5" fill="var(--line-soft)"/>`);
  s.push(`<rect data-k="soc-bar" x="${barX}" y="${barY}" width="${(barW * socPct).toFixed(1)}"
    height="${FLOW_SOC.h}" rx="5" fill="${socColor}"/>`);
  s.push(`<text x="${barX - 10}" y="${barY + 8}" text-anchor="end" font-size="12" fill="var(--text-2)">SOC</text>`);
  s.push(`<text data-k="soc-txt" x="${barX + barW + 10}" y="${barY + 8}" font-size="12.5" font-weight="600"
    fill="${socColor}" font-family="ui-monospace,Consolas,monospace">${fmt.soc(d.soc)}</text>`);

  // 能量守恒小结：进母线 = 出母线
  s.push(`<text data-k="sum" x="${cx}" y="${H - 12}" text-anchor="middle" font-size="11"
    fill="var(--text-3)">${v.sumText}</text>`);

  s.push('</svg>');
  return s.join('');
}

/* =====================================================================
   柱状图（单序列，用于告警时间线 / 收益构成）
   ===================================================================== */
function barChart(cfg) {
  const W = 900, H = cfg.height || 190;
  const padL = 46, padR = 12, padT = 12, padB = 30;
  const iw = W - padL - padR, ih = H - padT - padB;
  const labels = cfg.labels || [];
  const values = cfg.values || [];
  if (!values.length) return `<svg viewBox="0 0 ${W} ${H}" class="chart"></svg>`;
  const max = Math.max(...values, 1);
  const n = values.length;
  const bw = Math.max(2, (iw / n) * 0.62);
  const out = [`<svg viewBox="0 0 ${W} ${H}" class="chart" preserveAspectRatio="none">`];
  for (let i = 0; i <= 3; i++) {
    const v = max * i / 3, y = padT + ih * (1 - i / 3);
    out.push(`<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}"
      stroke="var(--line)" stroke-width="1"/>`);
    out.push(`<text x="${padL - 8}" y="${(y + 4).toFixed(1)}" text-anchor="end"
      font-size="11" fill="var(--text-3)">${v.toFixed(0)}</text>`);
  }
  values.forEach((v, i) => {
    const x = padL + iw * (i + 0.5) / n - bw / 2;
    const hgt = Math.max(1, ih * v / max);
    out.push(`<rect x="${x.toFixed(1)}" y="${(padT + ih - hgt).toFixed(1)}"
      width="${bw.toFixed(1)}" height="${hgt.toFixed(1)}" rx="2"
      fill="${cfg.color || 'var(--accent)'}" opacity="0.85"/>`);
  });
  const step = Math.max(1, Math.floor(n / 12));
  for (let i = 0; i < n; i += step) {
    const x = padL + iw * (i + 0.5) / n;
    out.push(`<text x="${x.toFixed(1)}" y="${H - 10}" text-anchor="middle"
      font-size="11" fill="var(--text-3)">${labels[i] ?? i}</text>`);
  }
  out.push('</svg>');
  return out.join('');
}

/* 横向比例条（收益构成等） */
function ratioRow(label, value, max, color, unit) {
  const pct = max > 0 ? Math.max(0, Math.min(1, value / max)) : 0;
  return h('div', { style: 'margin-bottom:11px' }, [
    h('div', { style: 'display:flex;justify-content:space-between;font-size:12.5px;margin-bottom:5px' }, [
      h('span', { text: label }),
      h('span', { class: 'mono', text: `${fmt.num(value, 2)}${unit || ''}` }),
    ]),
    h('div', { class: 'bar' }, h('i', { style: `width:${(pct * 100).toFixed(1)}%;background:${color}` })),
  ]);
}

/* =====================================================================
   图表导出 PNG（纯前端，零依赖）
   把内联 SVG 序列化成字符串 → 画到 canvas → toDataURL 下载。
   不拼接任何 HTML 字符串，也不引外部库，保持离线可用。
   ===================================================================== */
function exportSvgPng(svgEl, filename) {
  const clone = svgEl.cloneNode(true);
  // 内联 SVG 依赖 CSS 变量（--c-load 等），序列化到 <img> 时这些变量取不到，
  // 会变成黑块。这里把变量解析成当前计算值，内联进 SVG，保证导出的颜色正确。
  const styles = getComputedStyle(document.documentElement);
  const varNames = ['--c-load', '--c-pv', '--c-grid', '--c-bat',
                    '--accent', '--ok', '--warn', '--bad', '--info',
                    '--card', '--line', '--line-soft', '--text', '--text-3'];
  const resolve = (v) => {
    if (typeof v !== 'string' || !v.startsWith('var(')) return v;
    const m = v.match(/var\((--[a-z0-9-]+)\)/i);
    return m ? (styles.getPropertyValue(m[1]).trim() || v) : v;
  };
  clone.querySelectorAll('*').forEach(el => {
    ['fill', 'stroke'].forEach(attr => {
      const val = el.getAttribute(attr);
      if (val) el.setAttribute(attr, resolve(val));
    });
    const st = el.getAttribute('style');
    if (st) {
      el.setAttribute('style', st.replace(/var\((--[a-z0-9-]+)\)/gi,
        (_, name) => styles.getPropertyValue(name).trim() || name));
    }
  });

  const svgStr = new XMLSerializer().serializeToString(clone);
  const img = new Image();
  const svgBlob = new Blob([svgStr], { type: 'image/svg+xml;charset=utf-8' });
  const url = URL.createObjectURL(svgBlob);
  img.onload = () => {
    const scale = 2;   // 2× 导出更清晰
    const w = (svgEl.viewBox && svgEl.viewBox.baseVal && svgEl.viewBox.baseVal.width)
              || svgEl.clientWidth || 900;
    const h = (svgEl.viewBox && svgEl.viewBox.baseVal && svgEl.viewBox.baseVal.height)
              || svgEl.clientHeight || 300;
    const canvas = document.createElement('canvas');
    canvas.width = w * scale; canvas.height = h * scale;
    const cx = canvas.getContext('2d');
    cx.fillStyle = '#FFFFFF';   // 透明底变白底，否则 PNG 是黑底
    cx.fillRect(0, 0, canvas.width, canvas.height);
    cx.drawImage(img, 0, 0, canvas.width, canvas.height);
    URL.revokeObjectURL(url);
    canvas.toBlob(blob => {
      if (!blob) return;
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = filename || 'chart.png';
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    }, 'image/png');
  };
  img.onerror = () => URL.revokeObjectURL(url);
  img.src = url;
}
