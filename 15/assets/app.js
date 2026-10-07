/* =====================================================================
   15/ — 平台界面外壳
   路由（hash）/ 登录 / 引擎模式 / 数据集切换 / 菜单与权限

   两种正交的"模式"，别混（这是本文件最容易看错的地方）：
     engine    数据维度：run 运行模式（引擎按墙钟实时跑）/ sim 仿真模式（离线批量）
     run_mode  业务维度：峰谷套利 / 削峰填谷 / 辅助服务 / 保电备电
   前者决定"这些数字从哪来"，后者决定"电池在干哪种生意"。
   ===================================================================== */

const ICONS = {
  overview:   '<path d="M3 12h4l3-7 4 14 3-7h4" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/>',
  realtime:   '<circle cx="12" cy="12" r="8" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M12 12l4-2" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>',
  curves:     '<path d="M3 17l5-6 4 3 5-8 4 5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/>',
  alarms:     '<path d="M6 16V10a6 6 0 1112 0v6l2 2H4l2-2z" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M10 20h4" stroke="currentColor" stroke-width="1.8"/>',
  strategies: '<rect x="4" y="5" width="16" height="4" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.7"/><rect x="4" y="14" width="16" height="4" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.7"/>',
  economics:  '<circle cx="12" cy="12" r="8" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M9 10h6M9 14h6M13 8l-4 8" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  report:     '<rect x="5" y="3" width="14" height="18" rx="2" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M8 8h8M8 12h8M8 16h5" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  sim:        '<path d="M5 8h9l5 4-5 4H5z" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round"/><path d="M3 4v16" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
  devices:    '<rect x="4" y="4" width="16" height="12" rx="2" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M8 20h8M12 16v4" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
  users:      '<circle cx="9" cy="9" r="3" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M4 19c0-2.8 2.2-5 5-5s5 2.2 5 5" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M16 8h4M18 6v4" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
};

const MENU = [
  { id: 'overview',   name: '总览',        render: Pages.overview,   sub: '全站实时态势' },
  { id: 'realtime',   name: '实时监控',    render: Pages.realtime,   sub: '按仿真时标回放' },
  { id: 'curves',     name: '历史曲线',    render: Pages.curves,     sub: '功率 / SOC' },
  { id: 'alarms',     name: '告警中心',    render: Pages.alarms,     sub: '事件与状态迁移' },
  { id: 'strategies', name: '策略配置',    render: Pages.strategies, sub: '九个策略 / 仲裁优先级' },
  { id: 'economics',  name: '收益分析',    render: Pages.economics,  sub: '经济性核算' },
  { id: 'report',     name: '报表系统',    render: Pages.report,     sub: '日 / 周 / 月' },
  { id: 'sim',        name: '仿真',        render: Pages.sim,        sub: '日 / 周 / 月批量仿真' },
  { id: 'devices',    name: '设备管理',    render: Pages.devices,    sub: '台账与点表' },
  { id: 'users',      name: '用户与权限',  render: Pages.users,      sub: '账号 / 审计', minRole: 'admin' },
];

const ROLE_LEVEL = { viewer: 1, operator: 2, admin: 3 };

/* 实时引擎状态 → 徽章文案（与 14/src/engine.py 的 live_state 一一对应） */
const LIVE_STATE = {
  starting: { t: '启动中', k: 'b-info' },
  running:  { t: '运行中', k: 'b-ok' },
  stopped:  { t: '未启动', k: 'b-mute' },
  stale:    { t: '已停写', k: 'b-warn' },
  failed:   { t: '异常',   k: 'b-bad' },
};

/* 运行模式下会自动重刷的页面。
   ★ 刻意不含 策略配置 / 设备管理 / 用户与权限 —— 那三页有输入框与开关，
     定时重渲染会把正在敲的内容冲掉。 */
const LIVE_PAGES = ['overview', 'realtime', 'curves', 'alarms', 'economics', 'report'];
/* 重刷周期。数据粒度是 10 s（引擎 log_every=10），5 s 足够"看得见在长"，
   又不至于把接口按秒打满。 */
const LIVE_REFRESH_MS = 5000;
/* 顶栏数据集分段最多放几个按钮，超出的（老仿真）只出现在仿真页历史台账里 */
const SEG_SCENARIO_MAX = 4;

const App = (() => {
  let state = { scenario: '', page: 'overview', user: null, health: null,
                mode: null, modes: [], t: null,
                engine: 'run', engineInfo: null };
  // 当前页面注册的清理函数（仿真页的轮询定时器等）。换页 / 重渲染前统一清掉，
  // 否则每次自动重刷都会多留一个定时器，几个周期后接口被重复打 N 遍。
  let disposers = [];
  let liveTimer = null;
  let refreshing = false;
  let lastRefreshAt = '';

  /* ------------------- 分段控件滑动指示条（.seg 通用） -------------------
     ensureSegInd / moveSegInd / placeSegs 都在 ui.js 里（全局）：
     实时监控的倍速、报表系统的日报/周报/月报也要用同一套，放在这里它们够不着。 */

  /* ------------------------------ 登录 ------------------------------ */
  async function boot() {
    API.setUnauthorizedHandler((msg) => {
      showLogin(msg || '会话已过期，请重新登录');
    });

    const form = $('#login-form');
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      const btn = $('#login-btn');
      const err = $('#login-err');
      err.textContent = '';
      btn.disabled = true;
      btn.textContent = '登录中…';
      try {
        const r = await API.login($('#login-user').value.trim(), $('#login-pass').value);
        API.setSession(r.token, r.user);
        await enterApp(r.user);
      } catch (e) {
        err.textContent = e.message || '登录失败';
      } finally {
        btn.disabled = false;
        btn.textContent = '登 录';
      }
    });

    $('#logout-btn').addEventListener('click', async () => {
      // ★ 记住点击瞬间的 token：logout() 是异步的，等它回来时可能已经有新的
      //   登录发生了（「点退出 → 立刻登录」）。此时若无条件 setSession('', null)，
      //   就会把**刚签发的新 token 抹掉** —— 表现是界面进去了、但每个接口都 401
      //   「未登录或会话已过期」。所以只在没人重新登录过时才真正清会话。
      const mine = API.token;
      try { await API.logout(); } catch (e) { /* 忽略 */ }
      if (API.token !== mine) return;
      stopLiveTimer();
      API.setSession('', null);
      showLogin('已退出登录');
    });

    // 侧边栏收起 / 展开（记住偏好，刷新后保持；存的是布局偏好，不是任何口令）
    const appView = $('#app-view');
    if (localStorage.getItem('ems_sidebar_collapsed') === '1') {
      appView.classList.add('side-collapsed');
    }
    $('#nav-toggle').addEventListener('click', () => {
      const collapsed = appView.classList.toggle('side-collapsed');
      localStorage.setItem('ems_sidebar_collapsed', collapsed ? '1' : '0');
    });

    // 数据集切换（按钮是动态生成的，用事件委托，别在 build 时逐个绑）
    $('#scenario-seg').addEventListener('click', (ev) => {
      const b = ev.target.closest('button[data-scenario]');
      if (!b || b.disabled) return;
      if (b.dataset.scenario === state.scenario) return;
      $$('#scenario-seg button').forEach(x => x.classList.remove('active'));
      b.classList.add('active');
      moveSegInd($('#scenario-seg'));
      state.scenario = b.dataset.scenario;
      // 换数据集要丢掉"当前时刻"：live 是墙钟秒、sim 是一天内秒，混着用会取到空拍
      state.t = null;
      renderPage();
    });

    // 引擎模式切换（数据维度；与业务维度 mode-seg 相互独立）
    $('#engine-seg').addEventListener('click', async (ev) => {
      const b = ev.target.closest('button[data-engine]');
      if (!b) return;
      const eng = b.dataset.engine;
      if (eng === state.engine) return;
      if (!canWrite()) { toast('当前角色为只读，无法切换引擎模式（需 operator 及以上）', 'bad'); return; }
      try {
        await API.setEngine(eng);
        state.engine = eng;
        // 切模式必须重新解析默认数据集：运行模式看 live、仿真模式看最近一次成功仿真
        state.scenario = '';
        state.t = null;
        await loadEngine();
        toast(eng === 'run'
          ? '已切到运行模式：数据由实时引擎产生（需在状态条里点「启动引擎」）'
          : '已切到仿真模式：数据来自离线批量仿真结果', 'ok');
        renderPage();
      } catch (e) { toast(e.message, 'bad'); }
    });

    // 窗口尺寸变化时指示条重新对位（按钮宽度随字体度量变化）
    window.addEventListener('resize', () => {
      moveSegInd($('#scenario-seg'), false);
      moveSegInd($('#mode-seg'), false);
      moveSegInd($('#engine-seg'), false);
    });

    // 运行模式切换（独立顶层业务概念，切换即联动启停后端策略）
    const modeSeg = $('#mode-seg');
    if (modeSeg) {
      modeSeg.addEventListener('click', async (ev) => {
        const btn = ev.target.closest('button[data-mode]');
        if (!btn) return;
        const mid = btn.dataset.mode;
        if (mid === state.mode) return;
        if (!canWrite()) { toast('当前角色为只读，无法切换运行模式（需 operator 及以上）', 'bad'); return; }
        try {
          await API.activateMode(mid);
          state.mode = mid;
          syncModeSeg();
          toast(`已切换到「${btn.dataset.name}」模式，相关策略已联动启停`, 'ok');
          renderPage();
        } catch (e) { toast(e.message, 'bad'); }
      });
    }

    // 顶栏搜索：回车按菜单名/ID 匹配跳转
    const searchInput = $('#nav-search');
    if (searchInput) searchInput.addEventListener('keydown', (ev) => {
      if (ev.key !== 'Enter') return;
      const q = searchInput.value.trim();
      if (!q) return;
      const hit = visibleMenu().find(m =>
        (m.name + ' ' + m.id).toLowerCase().includes(q.toLowerCase()));
      if (hit) {
        location.hash = '#/' + hit.id;
        searchInput.value = '';
        searchInput.blur();
      } else {
        toast(`没有匹配「${q}」的页面`, 'bad');
      }
    });

    window.addEventListener('hashchange', () => {
      const id = location.hash.replace('#/', '');
      const target = MENU.find(m => m.id === id);
      if (!target) return;
      // 与 enterApp() 同一道校验：无权页面不能靠手改 hash 进去。
      // ★ 这**不是安全边界** —— 真正的门在 14/src/api.py 的 ROUTES（越权 403）。
      //   这里拦的是体验：不拦的话，viewer 手改到 #/users 会看到一行
      //   「加载失败：权限不足」，而正确的话术是"你没权限"。
      if (!visibleMenu().some(m => m.id === id)) {
        location.hash = '#/' + state.page;   // 落回当前可见页（不会死循环）
        toast(`当前角色（${state.user ? state.user.role : '—'}）无权访问「${target.name}」`, 'bad');
        return;
      }
      state.page = id;
      renderPage();
    });

    // 有 token 就直接进入
    if (API.token) {
      try {
        const me = await API.me();
        await enterApp(me.user);
        return;
      } catch (e) { /* 落到登录页 */ }
    }
    showLogin('');
  }

  function showLogin(msg) {
    stopLiveTimer();
    $('#app-view').classList.add('hidden');
    $('#login-view').classList.remove('hidden');
    const err = $('#login-err');
    if (err) err.textContent = msg || '';
    state.user = null;
  }

  function canWrite() {
    return ['operator', 'admin'].includes(state.user && state.user.role);
  }

  async function enterApp(user) {
    state.user = user;
    $('#login-view').classList.add('hidden');
    $('#app-view').classList.remove('hidden');

    $('#user-name').textContent = user.display_name || user.username;
    $('#user-role').textContent = user.role;
    $('#user-avatar').textContent = (user.display_name || user.username || 'U').slice(0, 1).toUpperCase();

    buildNav();
    try { state.health = await API.health(); } catch (e) { state.health = null; }
    await loadEngine();          // 引擎模式 + 数据集清单（决定顶栏两个分段器）
    await loadModes();
    drawFooter();

    // 校验 hash 里的页面是否有权限
    const want = location.hash.replace('#/', '');
    const visible = visibleMenu();
    state.page = visible.some(m => m.id === want) ? want : 'overview';
    if (!location.hash) location.hash = '#/' + state.page;

    await renderPage();
  }

  function visibleMenu() {
    const lv = ROLE_LEVEL[state.user && state.user.role] || 0;
    return MENU.filter(m => !m.minRole || lv >= (ROLE_LEVEL[m.minRole] || 99));
  }

  /* ===================== 引擎模式 / 数据集清单 ===================== */
  async function loadEngine() {
    let d = null;
    try { d = await API.engine(); } catch (e) { d = null; }
    state.engineInfo = d;
    if (d) state.engine = d.engine || 'run';
    buildEngineSeg();
    buildScenarioSeg(d);
    renderEngineBanner();
  }

  function buildEngineSeg() {
    const seg = $('#engine-seg');
    if (!seg) return;
    const info = state.engineInfo;
    const list = (info && info.engines) || [
      { id: 'run', name: '运行模式', desc: '实时引擎按墙钟跑' },
      { id: 'sim', name: '仿真模式', desc: '离线批量仿真' },
    ];
    clear(seg);
    list.forEach(e => {
      const rows = e.id === 'run'
        ? (info && info.live ? info.live.rows : 0)
        : (e.runs || 0);
      const unit = e.id === 'run' ? '行实时数据' : '次仿真';
      const b = h('button', {
        dataset: { engine: e.id, name: e.name },
        title: `${e.name} — ${e.desc || ''}（现有 ${rows} ${unit}）`,
        text: e.name,
      });
      if (e.id === state.engine) b.classList.add('active');
      seg.appendChild(b);
    });
    moveSegInd(seg, false);
  }

  /* 数据集分段：按引擎模式列出"现在能看的那些数据"。
     运行模式 → 只有 live（实时入库的那一份）；
     仿真模式 → 最近几次仿真数据集（多的收进仿真页历史台账）+ 内置正常/故障日。 */
  function buildScenarioSeg(engineInfo) {
    const seg = $('#scenario-seg');
    if (!seg) return;
    const list = scenarioList(engineInfo);
    const cur = state.scenario;

    if (cur && !list.some(s => s.id === cur)) {
      // 正在看的这份不在"最近几次"里（典型情形：从仿真页历史台账点进一个老数据集）。
      // 补一个按钮，而不是把用户悄悄切走 —— 悄悄切走的表现就是
      // "点了「查看结果」，界面看起来没变"。
      const meta = ((state.health && state.health.scenarios) || [])
        .find(s => s.scenario_id === cur);
      if (meta || cur === 'live') list.unshift(scenarioEntry(meta, cur));
      else state.scenario = '';
    }
    if (!state.scenario && list.length) {
      state.scenario = (engineInfo && engineInfo.scenario) || list[0].id;
    }

    clear(seg);
    list.forEach(s => {
      const b = h('button', {
        dataset: { scenario: s.id },
        title: `${s.name}（${s.id}）· ${s.rows || 0} 行`,
        text: s.name,
      });
      if (s.id === state.scenario) b.classList.add('active');
      seg.appendChild(b);
    });
    moveSegInd(seg, false);
  }

  /* health.scenarios 里的一行 → 分段按钮用的短条目 */
  function scenarioEntry(scn, fallbackId) {
    const id = (scn && scn.scenario_id) || fallbackId;
    if (id === 'live') return { id, name: '实时数据', rows: 0 };
    if (id === 'normal') return { id, name: '内置·正常日', rows: (scn && scn.log_rows) || 0 };
    if (id === 'fault') return { id, name: '内置·故障日', rows: (scn && scn.log_rows) || 0 };
    // 入库时写的是「日仿真d001 · 典型日（正常）」这种长名，
    // 分段按钮只取前面的短名（' · ' 之前那段）。
    return {
      id,
      name: String((scn && scn.name) || id).split(' · ')[0],
      rows: (scn && scn.log_rows) || 0,
    };
  }

  function scenarioList(engineInfo) {
    const info = engineInfo || {};
    if ((info.engine || state.engine) === 'run') {
      const live = info.live || {};
      return [{ id: 'live', name: '实时数据', rows: live.rows || 0 }];
    }
    const all = (state.health && state.health.scenarios) || [];
    const sims = all.filter(s => String(s.scenario_id).startsWith('sim-'))
                    .sort((a, b) => String(b.imported_at || '')
                      .localeCompare(String(a.imported_at || '')))
                    .slice(0, SEG_SCENARIO_MAX)
                    .map(s => scenarioEntry(s, s.scenario_id));
    const builtin = all.filter(s => ['normal', 'fault'].includes(s.scenario_id))
                       .map(s => scenarioEntry(s, s.scenario_id));
    return sims.concat(builtin);
  }

  /* ===================== 运行模式的实时引擎状态条 ===================== */
  function renderEngineBanner() {
    const el = $('#engine-banner');
    if (!el) return;
    const info = state.engineInfo;
    if (!info || info.engine !== 'run') { el.classList.add('hidden'); clear(el); return; }

    const lv = info.live || {};
    const st = LIVE_STATE[lv.state] || { t: lv.state || '—', k: 'b-mute' };
    const src = (info.sources || []).find(s => s.id === lv.source) || {};
    const csv = String(lv.csv || '').split(/[\\/]/).pop();

    el.classList.remove('hidden');
    clear(el);
    el.appendChild(h('div', { class: 'engine-banner-main' }, [
      h('span', { class: `eb-dot ${lv.running ? 'on' : ''}` }),
      h('span', { class: 'eb-title', text: '实时引擎' }),
      badge(st.t, st.k),
      h('span', { class: 'eb-meta', text:
        `数据源 ${src.name || '—'} · ${lv.dt_s || '—'} s 一拍 / 每 ${lv.log_every || '—'} 拍记录一行`
        + ` · 已入库 ${lv.rows || 0} 行`
        + (lv.started_at ? ` · 启动于 ${lv.started_at}` : '')
        + (csv ? ` · 实录 ${csv}` : '') }),
      // ★ 这句话必须由后端的 source_is_model 决定，不能写死：
      //   写死的话，将来真接上 07/ 的现场设备源，界面还会说"模型源"——
      //   那是这个项目最不能出现的一种谎。
      h('span', { class: 'eb-warn', text: lv.source_is_model
        ? '（模型源，不是真实电站）' : '（现场设备源 · 数据来自真实装置）' }),
    ]));

    if (lv.error) {
      el.appendChild(h('div', { class: 'eb-err', text: `引擎告警：${lv.error}` }));
    }

    const canW = canWrite();
    const busy = lv.state === 'starting';
    el.appendChild(h('div', { class: 'engine-banner-act' }, [
      lv.running
        ? h('button', { class: 'btn-sm', text: busy ? '启动中…' : '停止引擎',
                        disabled: !canW || busy, onclick: () => doLive('stop') })
        : h('button', { class: 'btn-sm primary', text: '启动引擎',
                        disabled: !canW || busy, onclick: () => doLive('start') }),
      h('button', { class: 'btn-sm', text: '清空实时数据',
                    disabled: !canW || lv.running,
                    title: '清掉已入库的实时数据（不动引擎模式与其它数据集）',
                    onclick: () => doLive('reset') }),
      canW ? null : h('span', { class: 'muted', style: 'font-size:12px', text: '只读角色' }),
    ]));
  }

  async function doLive(act) {
    try {
      if (act === 'reset' && !window.confirm('清空运行模式下已入库的全部实时数据？此操作不可撤销。')) return;
      if (act === 'start') await API.liveStart({});
      else if (act === 'stop') await API.liveStop();
      else await API.liveReset();
      toast(act === 'start' ? '实时引擎已启动，数据开始逐拍入库'
          : act === 'stop' ? '实时引擎已停止（剩余数据已收尾入库）'
          : '实时数据已清空', 'ok');
      await loadEngine();
      drawFooter();
      renderPage();
    } catch (e) { toast(e.message, 'bad'); }
  }

  /* ===================== 自动刷新（仅运行模式） ===================== */
  function syncLiveTimer() {
    const want = state.engine === 'run' && !!state.user
      && LIVE_PAGES.includes(state.page);
    if (want && !liveTimer) {
      liveTimer = setInterval(liveTick, LIVE_REFRESH_MS);
    } else if (!want) {
      stopLiveTimer();
    }
    updateLiveBadge();
  }

  function stopLiveTimer() {
    if (liveTimer) { clearInterval(liveTimer); liveTimer = null; }
    const b = $('#live-badge');
    if (b) b.classList.add('hidden');
  }

  async function liveTick() {
    if (document.hidden || refreshing) return;   // 后台标签页 / 上一次还没画完 → 跳过
    refreshing = true;
    try {
      state.engineInfo = await API.engine();
      renderEngineBanner();
      buildEngineSeg();
      lastRefreshAt = new Date().toLocaleTimeString('zh-CN', { hour12: false });
      await renderPage({ keepScroll: true, quiet: true });
    } catch (e) { /* 单次刷新失败不弹窗，避免每 5 秒刷一块红条 */ }
    finally { refreshing = false; }
  }

  function updateLiveBadge() {
    const b = $('#live-badge');
    if (!b) return;
    if (state.engine !== 'run') { b.classList.add('hidden'); return; }
    const lv = (state.engineInfo && state.engineInfo.live) || {};
    b.classList.remove('hidden');
    b.className = `live-badge ${
      lv.state === 'running' ? 'lv-on'
      : (lv.state === 'stale' || lv.state === 'failed') ? 'lv-warn' : 'lv-off'}`;
    b.textContent = `实时数据 · ${LIVE_STATE[lv.state] ? LIVE_STATE[lv.state].t : '—'}`
      + ` · 已入库 ${lv.rows || 0} 行`
      + (lastRefreshAt ? ` · 更新于 ${lastRefreshAt}` : '')
      + (LIVE_PAGES.includes(state.page) ? ` · 每 ${LIVE_REFRESH_MS / 1000} s 自动刷新` : '');
  }

  async function loadModes() {
    try {
      const d = await API.modes();
      state.modes = d.items || [];
      state.mode = d.active || (d.items.find(m => m.is_default) || {}).mode_id || null;
    } catch (e) {
      state.modes = [];
      state.mode = null;
    }
    buildModeSeg();
  }

  /* 模式分段：建按钮（loadModes 时整建，指示条无动画对位） */
  function buildModeSeg() {
    const seg = $('#mode-seg');
    if (!seg) return;
    const canSwitch = ['operator', 'admin'].includes(state.user && state.user.role);
    clear(seg);
    (state.modes || []).forEach(m => {
      const b = h('button', {
        dataset: { mode: m.mode_id, name: m.name },
        title: canSwitch ? `切换到「${m.name}」` : `${m.name}（只读，不可切换）`,
        text: m.name,
      });
      if (m.mode_id === state.mode) b.classList.add('active');
      seg.appendChild(b);
    });
    moveSegInd(seg, false);
  }

  /* 模式分段：只翻激活态（切换成功后调用），指示条平滑滑过去 */
  function syncModeSeg() {
    const seg = $('#mode-seg');
    if (!seg) return;
    $$('#mode-seg button').forEach(x =>
      x.classList.toggle('active', x.dataset.mode === state.mode));
    moveSegInd(seg);
  }

  function buildNav() {
    const nav = clear($('#nav'));
    visibleMenu().forEach(m => {
      // title：收起成图标栏后悬停仍能看到页面名
      const btn = h('button', { dataset: { id: m.id }, title: m.name }, [
        h('span', { html: `<svg viewBox="0 0 24 24" width="16" height="16">${ICONS[m.id] || ''}</svg>` }),
        h('span', { text: m.name }),
      ]);
      btn.addEventListener('click', () => { location.hash = '#/' + m.id; });
      nav.appendChild(btn);
    });
    highlightNav();
  }

  function highlightNav() {
    $$('#nav button').forEach(b => b.classList.toggle('active', b.dataset.id === state.page));
  }

  /* ------------------------------ 路由 ------------------------------ */
  async function renderPage(opts) {
    const o = opts || {};
    const meta = MENU.find(m => m.id === state.page) || MENU[0];
    highlightNav();
    $('#page-title').textContent = meta.name;
    $('#page-sub').textContent = meta.sub || '';

    const content = $('#content');
    // 自动重刷时保留滚动位置：否则运行模式每 5 s 把用户弹回页顶，
    // 一段长表格根本没法看。
    const keepTop = o.keepScroll ? content.scrollTop : 0;

    // 先清理上一轮页面留下的定时器 / 监听器
    disposers.splice(0).forEach(fn => { try { fn(); } catch (e) { /* 忽略 */ } });

    clear(content);
    content.appendChild(h('div', { class: 'empty', text: '加载中…' }));

    // 跨页面共享的「当前仿真时刻」：实时监控拖动/播放会更新它，
    // 总览据此取同一拍，消除「两页看的是不同时刻」的割裂。null = 未指定（默认最后一拍）。
    const ctx = {
      scenario: state.scenario,
      user: state.user,
      mode: state.mode,
      modes: state.modes,
      engine: state.engine,
      engineInfo: state.engineInfo,
      t: state.t,
      setT: (v) => { state.t = v; },
      setSub: (s) => { $('#page-sub').textContent = s; },
      // 页面挂载定时器 / 全局监听时必须走这个，否则换页后它还在跑
      onDispose: (fn) => { disposers.push(fn); },
      reloadEngine: async () => { await loadEngine(); },
      goPage: (id) => { location.hash = '#/' + id; },
      // 「打开某份数据集来看」：仿真页的「查看结果」用它 —— 仿真数据属于仿真模式，
      // 若当前在运行模式，必须先切引擎模式再选数据集（否则被 resolve_scenario
      // 按 live 覆盖掉，用户会看到"点了查看结果，界面没变"）。
      openDataset: async (scenarioId) => {
        if (state.engine !== 'sim') {
          await API.setEngine('sim');
          state.engine = 'sim';
        }
        state.scenario = scenarioId;
        state.t = null;
        await loadEngine();
        if (state.page !== 'overview') location.hash = '#/overview';
        else await renderPage();
      },
    };

    try {
      const node = await meta.render(ctx);
      clear(content);
      // 刷新失败时不要把上一屏数据清掉（本次已清，所以这里用"渲染中"占位更安全）
      content.appendChild(node);
      // ★ 页面里自带的 .seg（实时监控的倍速 / 报表系统的日报周报月报）在这里才
      //   第一次有布局，指示条必须等挂载完再对位 —— 页面函数自己调的话读到的是 0。
      placeSegs(content);
      if (o.keepScroll) content.scrollTop = keepTop;
    } catch (e) {
      clear(content);
      content.appendChild(h('div', { class: 'empty', text: `加载失败：${e.message}` }));
    }
    syncLiveTimer();
  }

  function drawFooter() {
    const hh = state.health;
    const info = state.engineInfo;
    const engName = state.engine === 'run' ? '运行模式（实时引擎）' : '仿真模式（离线批量）';
    if (hh) {
      $('#foot-left').textContent =
        `引擎：${engName} · 数据集 ${state.scenario || '—'} · 数据来源 ${hh.import_source || '—'}`;
      $('#foot-right').textContent =
        `schema v${hh.schema_version} · 平台层 14/`
        + (hh.imported_at ? ` · 导入于 ${hh.imported_at}` : '')
        + (info && info.live && state.engine === 'run'
           ? ` · 实时 ${info.live.rows || 0} 行` : '');
    } else {
      $('#foot-left').textContent = '接口未连通';
      $('#foot-right').textContent = '';
    }
  }

  return { boot, state, refreshEngine: loadEngine, refreshFooter: drawFooter };
})();

document.addEventListener('DOMContentLoaded', App.boot);
