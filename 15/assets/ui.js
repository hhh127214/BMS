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
    /* ★ 与后端 14/src/api.py::_hhmm 同一套口径，改一处两边都要改：
       `t_s = 86400`（24 h 仿真的最后一拍）必须写 "24:00" —— 以前取 %24
       会折回 "00:00"，时间轴最右端和第一拍长得一样，看着就是"永远 0:00"。
       跨天数据（周 / 月）加 `Dn ` 前缀标明第几天。 */
    if (_bad(t_s)) return '—';
    const sec = Math.floor(Number(t_s));
    const day = Math.floor(sec / 86400), rem = sec % 86400;
    const mm = Math.floor(rem / 60);
    const hm = `${String(Math.floor(mm / 60)).padStart(2, '0')}:${String(mm % 60).padStart(2, '0')}`;
    if (day === 0) return hm;
    if (rem === 0) return day === 1 ? '24:00' : `D${day} 24:00`;
    return `D${day + 1} ${hm}`;
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

/* ---------------------------- 全页弹层 ----------------------------
   逐台编辑里有点表 32 行 × 9 列，塞进一个居中小对话框等于让人在邮票上
   看表格。所以这层**跟页面一样大**：铺满视口、内部自己滚，关掉回到原处。
   ★ 不用 <dialog>：原生 dialog 自带 ::backdrop 与顶层渲染，与本项目
     .content 的滚动容器叠在一起时焦点行为不一致（ESC 有时吞、有时不吞）。
   ★ 按 key 复用：页面来回切时不会每切一次就往 body 上多挂一层。 */
function modal(opts) {
  const o = opts || {};
  if (o.key) {
    const old = document.getElementById('mdl-' + o.key);
    if (old && old._modal) { clear(old._modal.body); return old._modal; }
  }
  const tt = h('h2');
  const sub = h('p', { class: 'mdl-sub' });
  const body = h('div', { class: 'mdl-body' });
  const closeBtn = h('button', { class: 'btn-sm', text: '关闭', title: '关闭（Esc）' });
  const root = h('div', { class: 'mdl' }, [
    h('div', { class: 'mdl-head' }, [
      h('div', { class: 'mdl-tt' }, [tt, sub]),
      h('div', { class: 'pm-grow' }),
      closeBtn,
    ]),
    body,
  ]);
  tt.textContent = o.title || '';
  sub.textContent = o.sub || '';

  let on = false;
  const api = {
    node: root, body,
    isOpen: () => on,
    setTitle(t) { tt.textContent = t; },
    setSub(s) { sub.textContent = s; },
    open() {
      if (on) return;
      on = true;
      root.classList.add('on');
      document.body.classList.add('mdl-open');
      body.scrollTop = 0;   // 每次打开都从头看，别停在上一次滚到的位置
    },
    close() {
      if (!on) return;
      on = false;
      root.classList.remove('on');
      document.body.classList.remove('mdl-open');
    },
  };
  closeBtn.addEventListener('click', () => api.close());
  document.addEventListener('keydown', (e) => {
    if (on && e.key === 'Escape') api.close();
  });
  if (o.key) { root.id = 'mdl-' + o.key; root._modal = api; }
  document.body.appendChild(root);
  return api;
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
  // ★ 刻度数按「最长标签的估算宽度」自适应，不再固定 ~8 个：
  //   sim 场景标签是 "HH:MM"（5 字符），8~9 个刚好；run 模式（wall 口径）
  //   是 "2026-10-11 22:03:12"（19 字符），同样 8~9 个必然两两重叠
  //   （2026-10-12 用户截图实测：54 行实录 · 10 个刻度 · 相邻中心 ~92
  //   单位 < 标签宽 ~118 单位，功率与 SOC 两张图同时中招）。
  //   水平方向 preserveAspectRatio="none" 等比拉伸，viewBox 内不重叠
  //   ⇒ 显示出来也不重叠，所以判据可以在 viewBox 坐标里做。
  // 另：wall 标签通常共享同一个日期前缀，逐个重复既占地又挡视线 ——
  //   全体一致时只保留时间部分，日期在右上角画一次；跨午夜（日期不一致）
  //   时不动标签，靠降低刻度密度保不重叠。
  const labels = cfg.xLabels || [];
  if (labels.length) {
    const ticksAll = labels.map(String);
    const mDate = ticksAll[0].match(/^\d{4}-\d{2}-\d{2}(?= )/);
    const sameDay = mDate && ticksAll.every(s => s.startsWith(mDate[0] + ' '));
    const ticks = sameDay ? ticksAll.map(s => s.slice(mDate[0].length + 1)) : ticksAll;
    const maxLen = ticks.reduce((m, s) => Math.max(m, s.length), 0);
    // 6.2 ≈ 11px 字号下数字/冒号的平均字宽；+14 是刻度间最小呼吸空隙。
    // 上限 9：短标签场景维持原有的 ~8-9 个刻度观感，不因装得下就铺满。
    const maxFit = Math.min(9, Math.max(2, Math.floor(iw / (maxLen * 6.2 + 14))));
    const step = Math.max(1, Math.ceil(ticks.length / maxFit));
    for (let i = 0; i < ticks.length; i += step) {
      out.push(`<text x="${X(i).toFixed(1)}" y="${H - 8}" text-anchor="middle"
        font-size="11" fill="var(--text-3)">${ticks[i]}</text>`);
    }
    if (sameDay) {
      out.push(`<text x="${W - padR}" y="${padT - 1}" text-anchor="end"
        font-size="10" fill="var(--text-3)">${mDate[0]}</text>`);
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

/* =====================================================================
   装配视图（爆炸图）：柜 → 舱 → 部件，点部件看它的测点
   ---------------------------------------------------------------------
   ★ 为什么画成"等轴测托盘 + 立方体"而不是平面框图：
     `energyFlow` 已经是这个界面里唯一的等轴测图形（3D 母线 + 四个场景），
     装配视图沿用同一套"顶面 + 正面 + 右侧面"的画法，两张图放在一起才像同一个产品；
     若这里换成平面方框，视觉上会像两个项目拼起来的。

   ★ 为什么四个舱是 2×2 而不是沿一条斜线排开：
     斜线排开（真正的爆炸图）在 900px 宽度里只能画 2 个舱，
     剩下两个要滚屏才能看到 —— 而这张图的用途是"一眼看清柜里有几个舱、
     哪个部件在报警"。可读性优先于形式感。舱内部件仍按等轴测斜向错开，
     保留了"层层展开"的意思。

   ★ 部件的三种颜色是有语义的，不是配色：
     live   有实时数据源（能显示当前值）
     cfg    测点已定义，但当前数据源给不出实时值 —— ★ 必须显式画成灰色，
            涂成"正常"等于谎报现场这个部件在传数据（这是本项目最忌讳的失效）
     alarm  该部件当前有告警
   ===================================================================== */
const ASM_KX = 0.55;                       // 等轴测斜切系数（与 energyFlow 的投影同一手感）
const ASM_W  = 900, ASM_H = 560;
const ASM_TRAY = { w: 300, d: 160, th: 16 };
const ASM_CUBE = { w: 62, d: 62, h: 40 };

/* 托盘顶面内一点的屏幕坐标。ta/tb ∈ [0,1]，分别沿"宽"与"深"两个方向。 */
function asmTopPoint(ox, oy, ta, tb) {
  return [ox + ta * ASM_TRAY.w + tb * ASM_TRAY.d * ASM_KX,
          oy + tb * ASM_TRAY.d * 0.55];
}

function assemblyView(d) {
  const areas = (d && d.areas) || [];
  const active = d && d.active;
  const s = [];
  s.push(`<svg viewBox="0 0 ${ASM_W} ${ASM_H}" class="chart asm" role="img">`);
  s.push(`<title>柜体装配视图</title><desc>柜 → 舱 → 部件三层装配关系，点部件看其测点</desc>`);

  areas.forEach((a, i) => {
    const col = i % 2, row = Math.floor(i / 2);
    const ox = 26 + col * 452;
    const oy = 74 + row * 250;
    const [ax, ay] = asmTopPoint(ox, oy, 0, 0);
    const [bx, by] = asmTopPoint(ox, oy, 1, 0);
    const [cx2, cy2] = asmTopPoint(ox, oy, 1, 1);
    const [dx, dy] = asmTopPoint(ox, oy, 0, 1);
    const th = ASM_TRAY.th;

    // 托盘：顶面 + 正面 + 右侧面（等轴测三面）
    s.push(`<polygon points="${dx},${dy} ${cx2},${cy2} ${cx2},${cy2 + th} ${dx},${dy + th}" fill="var(--line)" opacity="0.35"/>`);
    s.push(`<polygon points="${bx},${by} ${cx2},${cy2} ${cx2},${cy2 + th} ${bx},${by + th}" fill="var(--line)" opacity="0.22"/>`);
    s.push(`<polygon points="${ax},${ay} ${bx},${by} ${cx2},${cy2} ${dx},${dy}" fill="var(--card)" stroke="var(--line)" stroke-width="1.2"/>`);

    // 舱名挂在托盘左上方（与 energyFlow 的"名称挂图案外侧"同一条规矩）
    s.push(`<text x="${ax + 4}" y="${ay - 12}" fill="var(--text)" font-size="12.5" font-weight="600">${a.name}</text>`);
    s.push(`<text x="${ax + 4}" y="${ay - 0}" fill="var(--text-3)" font-size="10.5">${a.parts.length} 部件 · ${a.parts.reduce((n, p) => n + p.n, 0)} 点</text>`);

    // 舱内部件：网格排布，最多 3 列
    a.parts.forEach((p, j) => {
      const cCol = j % 3, cRow = Math.floor(j / 3);
      const ta = 0.06 + cCol * 0.30;
      const tb = 0.10 + cRow * 0.52;
      const [px, py] = asmTopPoint(ox, oy, ta, tb);
      const w = ASM_CUBE.w, dd = ASM_CUBE.d, hh = ASM_CUBE.h;
      const p0 = [px, py];
      const p1 = [px + w, py];
      const p2 = [px + w + dd * ASM_KX, py + dd * 0.55];
      const p3 = [px + dd * ASM_KX, py + dd * 0.55];
      const pts = (arr) => arr.map(q => `${q[0].toFixed(1)},${q[1].toFixed(1)}`).join(' ');

      const kind = p.state === 'alarm' ? 'alarm' : (p.hasLive ? 'live' : 'cfg');
      const topC = kind === 'alarm' ? 'var(--bad)'
                 : kind === 'live'  ? 'var(--c-bat)'
                                    : 'var(--text-3)';
      const op   = kind === 'cfg' ? 0.30 : 0.85;
      const sw   = p.id === active ? 2.4 : 1.1;

      // 部件立方体：正面 + 右侧面 + 顶面
      s.push(`<polygon points="${pts([p3, p2, [p2[0], p2[1] + hh], [p3[0], p3[1] + hh]])}" fill="${topC}" opacity="${op * 0.7}"/>`);
      s.push(`<polygon points="${pts([p1, p2, [p2[0], p2[1] + hh], [p1[0], p1[1] + hh]])}" fill="${topC}" opacity="${op * 0.5}"/>`);
      s.push(`<g class="asm-part" data-pid="${p.id}" style="cursor:pointer">`);
      s.push(`<polygon points="${pts([p0, p1, p2, p3])}" fill="${topC}" opacity="${op}" stroke="var(--text)" stroke-width="${sw}"/>`);

      // 部件名 + 当前值：名在顶面上，值在顶面下方（与 energyFlow 的"数值在图案下方"一致）
      const lx = px + w * 0.5 + dd * ASM_KX * 0.5, ly = py + dd * 0.55 * 0.5;
      s.push(`<text x="${lx.toFixed(1)}" y="${ly.toFixed(1)}" text-anchor="middle" fill="${kind === 'cfg' ? 'var(--text-3)' : '#fff'}" font-size="10.5" font-weight="600" pointer-events="none">${p.name}</text>`);
      const vt = p.hasLive && p.value != null
        ? `${fmt.num(p.value, 1)}${p.unit ? ' ' + p.unit : ''}`
        : (kind === 'alarm' ? '告警' : '无实时源');
      s.push(`<text x="${lx.toFixed(1)}" y="${(ly + 13).toFixed(1)}" text-anchor="middle" fill="${kind === 'cfg' ? 'var(--text-3)' : 'var(--text)'}" font-size="10" pointer-events="none">${vt}</text>`);
      s.push(`</g>`);
    });
  });

  s.push(`</svg>`);
  return s.join('');
}

/* =====================================================================
   一次接线图：柜/舱/部件之间的电气连接 + 当前功率方向
   ---------------------------------------------------------------------
   ★ 与 `energyFlow` 的分工（不要混）：
     energyFlow 是**能流示意**——四节点定死（光伏/储能/母线/负荷/电网），
     回答"电往哪走、走多少"。
     一次接线图是**拓扑事实**——节点来自 `docs/规划/柜体装配视图.md` §2 的 9 条连接，
     回答"柜里有什么、怎么接的"。节点数不是四个，加一台设备就多一个节点。

   ★ 为什么辅助舱三个部件挂在主链**外面**：
     消防 / 空调 / 门禁不走主电流。把它们画进主链等于说"电流流过烟感"——
     现场会当场指出画错了。所以挂在柜体轮廓上，用虚线表示"辅助用电 / 保护"。

   ★ PCS 的 AC/DC 是**同一个部件内部**的变换，不画成两个节点。

   d.nodes: [{ id, name, x, y, w, h, live, value, unit, branch }]
   d.edges: [{ from, to, label, active, color, dashed }]
   ===================================================================== */
function wiringDiagram(d) {
  const W = 900, H = 330;
  const nodes = (d && d.nodes) || [];
  const edges = (d && d.edges) || [];
  const idx = {};
  nodes.forEach(n => { idx[n.id] = n; });

  const s = [];
  s.push(`<svg viewBox="0 0 ${W} ${H}" class="chart wiring" role="img">`);
  s.push(`<title>一次接线图</title><desc>柜 → 舱 → 部件的电气连接与当前功率方向</desc>`);

  /* ---- 三个几何工具：让线不进框、数不压字 ---- */

  // 竖直段若落在某个节点框内（且 y 区间有交叠），线就会从框里穿出来
  const hitsNode = (x, yA, yB) => nodes.some(n =>
    x > n.x - 3 && x < n.x + n.w + 3 &&
    !(Math.max(yA, yB) < n.y - 3 || Math.min(yA, yB) > n.y + n.h + 3));

  // 折线的竖直段落点：从两节点中点起，向两侧找第一个不碰框的位置
  function safeX(x1, x2, y1, y2) {
    const mid = (x1 + x2) / 2;
    if (!hitsNode(mid, y1, y2)) return mid;
    const lo = Math.min(x1, x2), hi = Math.max(x1, x2);
    for (let d = 6; d <= Math.abs(hi - lo); d += 6) {
      if (mid + d <= hi && !hitsNode(mid + d, y1, y2)) return mid + d;
      if (mid - d >= lo && !hitsNode(mid - d, y1, y2)) return mid - d;
    }
    return mid;
  }

  /* 功率数字：抬到节点上方（原来放在「中心 −8」，正落在节点名称那一行上）；
     再加一圈卡片底色的描边，万一仍与线相交也读得清。 */
  const label = (x, y, anchor, txt) => {
    s.push(`<text x="${x}" y="${y}" text-anchor="${anchor}" fill="var(--text)"`
         + ` font-size="10.5" font-weight="600" paint-order="stroke"`
         + ` stroke="var(--card)" stroke-width="3.5" stroke-linejoin="round">${txt}</text>`);
  };

  /* ---- 辅助舱：共用一条「辅助母线」，各部件竖直挂上去 ----
     原来每个部件一根折线拉到母线，竖直段必然落在主链某个节点框内
     （消防那根就压在「交流断路器」框上），横向段还会穿过主链文字。
     改成一条水平母线架在辅助行与主链之间，部件各自向上/下接。 */
  const AUX_Y = 138;
  const auxEdges = edges.filter(e => e.aux && idx[e.from] && idx[e.to]);
  if (auxEdges.length) {
    const bus = idx[auxEdges[0].from];
    const bx = bus.x + bus.w / 2;
    const tail = idx[auxEdges[auxEdges.length - 1].to];
    const tx = tail.x + tail.w / 2;
    s.push(`<path d="M${bx},${bus.y} L${bx},${AUX_Y} L${tx},${AUX_Y}" fill="none"`
         + ` stroke="var(--text-3)" stroke-width="1.6" stroke-dasharray="6 4" opacity="0.5"/>`);
    auxEdges.forEach(e => {
      const n = idx[e.to];
      const cx = n.x + n.w / 2;
      s.push(`<path d="M${cx},${n.y + n.h} L${cx},${AUX_Y}" fill="none"`
           + ` stroke="var(--text-3)" stroke-width="1.6" stroke-dasharray="6 4" opacity="0.5"/>`);
    });
  }

  /* ---- 主链与分支：连线先画（压在节点下面，接头才干净） ---- */
  edges.filter(e => !e.aux).forEach(e => {
    const a = idx[e.from], b = idx[e.to];
    if (!a || !b) return;
    const stroke = e.color || 'var(--line)';
    const wdt = e.active ? 3.2 : 1.6;
    const dash = e.dashed ? ' stroke-dasharray="6 4"' : '';
    const opac = e.active ? ' opacity="0.95"' : ' opacity="0.4"';

    /* 出线侧自适应：
       ★ 固定「a 右缘 → b 左缘」在 b 位于 a 左侧（或同列）时会绕回去、穿过 b 的框
         —— 母线 → 厂区负荷就是这种情况，线从负荷框里横穿过去。 */
    if (Math.abs(a.x - b.x) < 1) {              // 同一列 → 竖连（底 → 顶）
      const down = b.y > a.y;
      const x = a.x + a.w / 2;
      const y1 = down ? a.y + a.h : a.y;
      const y2 = down ? b.y : b.y + b.h;
      s.push(`<path d="M${x},${y1} L${x},${y2}" fill="none" stroke="${stroke}"`
           + ` stroke-width="${wdt}"${dash}${opac}/>`);
      if (e.active) {
        const sg = down ? -1 : 1;               // 箭头朝行进方向
        s.push(`<polygon points="${x},${y2} ${x - 6.5},${y2 + 11 * sg} ${x + 6.5},${y2 + 11 * sg}"`
             + ` fill="${stroke}"/>`);
      }
      if (e.label) label(x + 10, (y1 + y2) / 2 + 4, 'start', e.label);
      return;
    }

    const right = b.x > a.x;                    // 从靠近的那一侧出线
    const x1 = right ? a.x + a.w : a.x;
    const x2 = right ? b.x : b.x + b.w;
    const y1 = a.y + a.h / 2, y2 = b.y + b.h / 2;
    const mx = safeX(x1, x2, y1, y2);
    const path = Math.abs(y1 - y2) < 1
      ? `M${x1},${y1} L${x2},${y2}`
      : `M${x1},${y1} L${mx},${y1} L${mx},${y2} L${x2},${y2}`;
    s.push(`<path d="${path}" fill="none" stroke="${stroke}" stroke-width="${wdt}"`
         + `${dash}${opac}/>`);
    if (e.active) {
      // 箭头：与 energyFlow 同规矩——实心多边形，不用 <marker>
      const sg = right ? -1 : 1;
      s.push(`<polygon points="${x2},${y2} ${x2 + 11 * sg},${y2 - 6.5} ${x2 + 11 * sg},${y2 + 6.5}"`
           + ` fill="${stroke}"/>`);
    }
    // 节点高 50：中心到顶 25，再留 8 ⇒ 抬 33 才落在框外
    if (e.label) label(mx, Math.min(y1, y2) - 33, 'middle', e.label);
  });

  /* ---- 节点 ---- */
  nodes.forEach(n => {
    const cls = n.branch ? 'branch' : '';
    const fill = n.live ? 'var(--card)' : 'var(--card-2)';
    const stroke = n.state === 'alarm' ? 'var(--bad)'
                 : n.live ? 'var(--line)' : 'var(--text-3)';
    const op = n.live ? 1 : 0.55;
    s.push(`<g opacity="${op}">`);
    if (n.branch) {
      s.push(`<rect x="${n.x}" y="${n.y}" width="${n.w}" height="${n.h}" rx="8"`
           + ` fill="${fill}" stroke="${stroke}" stroke-width="1.4" stroke-dasharray="5 3"/>`);
    } else {
      s.push(`<rect x="${n.x}" y="${n.y}" width="${n.w}" height="${n.h}" rx="6"`
           + ` fill="${fill}" stroke="${stroke}" stroke-width="1.6"/>`);
    }
    // 名称在框内上方，数值在框内下方（与 energyFlow「数值在图案下方」同一条规矩）
    s.push(`<text x="${n.x + n.w / 2}" y="${n.y + 20}" text-anchor="middle"`
         + ` fill="var(--text)" font-size="11.5" font-weight="600">${n.name}</text>`);
    const vt = n.live && n.value != null ? `${fmt.num(n.value, 1)}${n.unit ? ' ' + n.unit : ''}`
             : (n.live ? '—' : '无实时源');
    s.push(`<text x="${n.x + n.w / 2}" y="${n.y + n.h - 10}" text-anchor="middle"`
         + ` fill="${n.live ? 'var(--text)' : 'var(--text-3)'}" font-size="11" class="mono">${vt}</text>`);
    s.push(`</g>`);
  });

  s.push(`</svg>`);
  return s.join('');
}
