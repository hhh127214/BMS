/* =====================================================================
   15/ — 平台界面外壳
   路由（hash）/ 登录 / 数据源 / 菜单与权限

   ★ 数据源只有一个：平台库的"当前数据集"，由后端 `/api/engine` 的
     `scenario` 字段给出（后端按采集状态决定：实时在跑就是实时那套，
     否则是最近一次入库的数据）。界面**不提供**切换器 ——
     "选哪份演示场景 / 切运行还是仿真"是研发调试用的概念，
     一旦摆到产品界面上，用户就分不清"这是现场的数"还是"这是演示的数"。

   与它正交的是"运行模式"（业务维度：峰谷套利 / 削峰填谷 / 辅助服务 /
   保电备电）—— 那个是真正的产品功能，顶栏右侧保留。
   ===================================================================== */

const ICONS = {
  overview:   '<path d="M3 12h4l3-7 4 14 3-7h4" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/>',
  realtime:   '<circle cx="12" cy="12" r="8" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M12 12l4-2" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>',
  curves:     '<path d="M3 17l5-6 4 3 5-8 4 5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/>',
  alarms:     '<path d="M6 16V10a6 6 0 1112 0v6l2 2H4l2-2z" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M10 20h4" stroke="currentColor" stroke-width="1.8"/>',
  strategies: '<rect x="4" y="5" width="16" height="4" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.7"/><rect x="4" y="14" width="16" height="4" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.7"/>',
  economics:  '<circle cx="12" cy="12" r="8" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M9 10h6M9 14h6M13 8l-4 8" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  report:     '<rect x="5" y="3" width="14" height="18" rx="2" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M8 8h8M8 12h8M8 16h5" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  /* 装配视图（爆炸图）：三个沿等轴测斜向错开的层板 —— 与图上"层层展开"的画法同构 */
  structure:  '<rect x="3" y="14" width="12" height="6" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.6"/><rect x="7" y="8" width="12" height="6" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.6"/><rect x="11" y="2" width="12" height="6" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.6"/>',
  /* 一次接线图：一条主链上挂三个节点方块 —— 节点与连线一眼可辨 */
  wiring:     '<path d="M2 12h4M18 12h4" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/><rect x="6" y="8" width="5" height="8" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.6"/><rect x="13" y="8" width="5" height="8" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M11 12h2" stroke="currentColor" stroke-width="1.7"/>',
  devices:    '<rect x="4" y="4" width="16" height="12" rx="2" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M8 20h8M12 16v4" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
  users:      '<circle cx="9" cy="9" r="3" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M4 19c0-2.8 2.2-5 5-5s5 2.2 5 5" fill="none" stroke="currentColor" stroke-width="1.8"/><path d="M16 8h4M18 6v4" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
};

const MENU = [
  { id: 'overview',   name: '总览',        render: Pages.overview,   sub: '全站实时态势' },
  { id: 'realtime',   name: '实时监控',    render: Pages.realtime,   sub: '逐拍快照' },
  { id: 'curves',     name: '历史曲线',    render: Pages.curves,     sub: '功率 / SOC' },
  { id: 'alarms',     name: '告警中心',    render: Pages.alarms,     sub: '事件与状态迁移' },
  { id: 'strategies', name: '策略配置',    render: Pages.strategies, sub: '九个策略 / 仲裁优先级' },
  { id: 'economics',  name: '收益分析',    render: Pages.economics,  sub: '经济性核算' },
  { id: 'report',     name: '报表系统',    render: Pages.report,     sub: '日 / 周 / 月' },
  { id: 'structure',  name: '装配视图',    render: Pages.structure,  sub: '柜 → 舱 → 部件（爆炸图）' },
  { id: 'wiring',     name: '一次接线图',  render: Pages.wiring,     sub: '电气连接与当前功率' },
  { id: 'devices',    name: '设备管理',    render: Pages.devices,    sub: '台账与点表' },
  { id: 'users',      name: '用户与权限',  render: Pages.users,      sub: '账号 / 审计', minRole: 'admin' },
];

const ROLE_LEVEL = { viewer: 1, operator: 2, admin: 3 };

/* 会自动重刷的页面。
   ★ 刻意不含 策略配置 / 设备管理 / 用户与权限 —— 那三页有输入框与开关，
     定时重渲染会把正在敲的内容冲掉。 */
/* ★ 「装配视图」与「一次接线图」也在内：两页都是纯展示，没有输入框，
     自动重渲染不会冲掉用户正在敲的东西 —— 判据是"有没有输入控件"，不是"重不重要"。 */
const LIVE_PAGES = ['overview', 'realtime', 'curves', 'alarms', 'economics', 'report',
                    'structure', 'wiring'];
/* 重刷周期。数据粒度是 10 s（引擎 log_every=10），5 s 足够"看得见在长"，
   又不至于把接口按秒打满。 */
const LIVE_REFRESH_MS = 5000;

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

    // 窗口尺寸变化时指示条重新对位（按钮宽度随字体度量变化）
    window.addEventListener('resize', () => {
      moveSegInd($('#mode-seg'), false);
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
    // ★ 2026-10-16：登录框此刻**已经隐藏**了。这一段只要有一处抛错，
    //   异常就会冒泡回登录处理器的 catch —— 而错误文案是写进 #login-err 的，
    //   那时它在一个 hidden 的容器里，用户看到的是「进了个空白页、什么都没说」。
    //   所以：初始化失败**不弹回登录框**（口令是对的，问题不在登录），
    //   留在界面里画出「出了什么事」，并 toast 一声。
    try {
      try { state.health = await API.health(); } catch (e) { state.health = null; }
      await loadDataSource();      // 当前数据集 + 采集状态（决定页面读哪份数据）
      await loadModes();
      drawFooter();

      // 校验 hash 里的页面是否有权限
      const want = location.hash.replace('#/', '');
      const visible = visibleMenu();
      state.page = visible.some(m => m.id === want) ? want : 'overview';
      if (!location.hash) location.hash = '#/' + state.page;

      await renderPage();
    } catch (e) {
      try {
        const content = $('#content');
        clear(content);
        content.appendChild(h('div', { class: 'empty', text: `界面初始化失败：${e.message}` }));
      } catch (e2) { /* 连兜底都画不出来时，至少还有 toast */ }
      toast(`界面初始化遇到问题：${e.message}`, 'bad');
    }
  }

  /* ------------- 「这一刻没有数据」≠「加载失败」 -------------
     采集侧还没攒到数据时，后端会以 **404 + 一句人话原因**回绝
     overview / realtime / series 等接口。这不是功能坏了，是**这条路此刻没数据**。

     ★ 2026-10-16 实跑（真后端 + jsdom 驱动真前端）确认：
       登录**进得去**、菜单都在，坏的是**呈现** —— 6 个页面显示红字
       「加载失败：…」，用户读到的是"功能坏了"，而不是"去哪拿数据"。
       排障方向被这句措辞带偏，比报错本身更贵。

     分流判据用 **HTTP 状态**（api.js 已把 res.status 挂到错误上），不猜文案：
       404 → 空状态（+ 出路按钮）；其余（5xx / 网络）→ 才是「加载失败」。 */
  function isNotReady(e) {
    return !!e && e.status === 404;
  }

  function renderNotReady(e, ctx) {
    const canAct = canWrite();
    const acts = [];
    // ★ 其他页面是**正式运行页面**：这里不放"启动采集"。
    //   采集的启停属于接入，统一在设备管理页 —— 一页一个开关，
    //   而不是每个页面都能把采集拉起来（那样"谁把它停的"就查不清了）。
    if (canAct) {
      acts.push(h('button', {
        class: 'btn-sm primary', text: '去设备管理接入数据源',
        onclick: () => { location.hash = '#/devices'; },
      }));
    }
    acts.push(h('button', {
      class: 'btn-sm', text: '重新加载',
      onclick: () => { renderPage(); },
    }));
    return h('div', { class: 'empty' }, [
      h('div', { text: '这段时间还没有数据' }),
      h('div', { class: 'muted', style: 'margin-top:8px', text: e.message || '' }),
      acts.length
        ? h('div', { style: 'margin-top:16px;display:flex;gap:10px;justify-content:center' }, acts)
        : null,
      h('div', {
        class: 'muted', style: 'margin-top:14px',
        text: '接口是通的 —— 这不是加载失败，是采集侧还没攒到这一刻的数据。',
      }),
    ]);
  }

  function visibleMenu() {
    const lv = ROLE_LEVEL[state.user && state.user.role] || 0;
    return MENU.filter(m => !m.minRole || lv >= (ROLE_LEVEL[m.minRole] || 99));
  }

  /* ===================== 数据源（唯一解析点） =====================
     ★ 全站只有这一个地方决定"看哪份数据"。页面一律用 ctx.scenario，
       自己不再拼、不再猜 —— 两处各解析一次必然漂移，
       表现就是"顶栏写 A、页面看的是 B"，而且不报错。 */
  async function loadDataSource() {
    let d = null;
    try { d = await API.engine(); } catch (e) { d = null; }
    state.engineInfo = d;
    if (d) {
      state.engine = d.engine || 'run';
      // 后端按采集状态给出"当前数据集"：实时在跑就是实时那套，否则最近一次入库的。
      // 前端只接受这个答案，**没有第二个来源**。
      const sc = d.scenario || '';
      if (sc && sc !== state.scenario) {
        state.scenario = sc;
        state.t = null;   // 换了数据集，原来记住的时刻不再属于这份数据
      }
    }
  }

  /* ===================== 自动刷新 ===================== */
  function syncLiveTimer() {
    // ★ 判据只有"登录着 + 这一页是纯展示页" —— 不再看引擎模式。
    //   产品界面就是定时刷新；把刷新条件绑在"引擎在跑"上，
    //   会让同一页在不同数据源下行为不一样，用户看不出为什么。
    const want = !!state.user && LIVE_PAGES.includes(state.page);
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
      // 走同一个解析入口：采集侧一启动，后端认定的"当前数据集"就可能从历史库
      // 翻到实时那套。各页自己重取一份 JSON 是不会知道这件事的。
      await loadDataSource();
      lastRefreshAt = new Date().toLocaleTimeString('zh-CN', { hour12: false });
      drawFooter();
      await renderPage({ keepScroll: true, quiet: true });
    } catch (e) { /* 单次刷新失败不弹窗，避免每 5 秒刷一块红条 */ }
    finally { refreshing = false; }
  }

  function updateLiveBadge() {
    const b = $('#live-badge');
    if (!b) return;
    if (!state.user) { b.classList.add('hidden'); return; }
    const lv = (state.engineInfo && state.engineInfo.live) || {};
    const run = state.engine === 'run';
    const unhealthy = lv.state === 'stale' || lv.state === 'failed';
    /* ★ 采集没在跑、但 live 里已经有数据 ⇒ 页面上这批数是**最后一拍**，
       它不会自己再长。这件事必须写在徽章上：
       原来这里永远是「每 5 s 自动刷新 · 更新于 <现在>」，而那个"更新于"
       是**前端刷新时间**、不是**数据时间** —— 关掉采集之后它照样每次刷新都在变，
       于是"数据已经停了两小时"看起来和"正在实时采"一模一样。
       （2026-10-19 实测：关掉模拟源后 live 冻结在 01:37:39，概览仍 200
        有数，页面上没有任何一处说它是旧的。）
       ★ 判据**不看是哪一页**：设备管理页上同样挂着这个徽章，而它恰恰是
         "刚把模拟源关掉"的那一页 —— 在这里漏掉，人就会在关完之后
         看到"每 5 s 自动刷新"而以为还在采。 */
    const frozen = run && !unhealthy
      && lv.state !== 'running' && lv.state !== 'starting' && !!lv.rows;
    const at = String(lv.imported_at || '').slice(-8) || '—';
    b.classList.remove('hidden');
    b.className = `live-badge ${
      !run ? 'lv-off'
      : (unhealthy || frozen) ? 'lv-warn'
      : (lv.state === 'running' ? 'lv-on' : 'lv-off')}`;
    b.textContent = !run
      ? (LIVE_PAGES.includes(state.page)
          ? `每 ${LIVE_REFRESH_MS / 1000} s 自动刷新`
          : '本页不自动刷新')
        + (lastRefreshAt ? ` · 更新于 ${lastRefreshAt}` : '')
      : unhealthy
        ? `采集${lv.state === 'failed' ? '失败' : '中断'} · 数据停在 ${at}`
          + (lv.rows ? ` · 实时库 ${lv.rows} 行` : '')
        : frozen
          ? `采集已停止 · 数据停在 ${at} · 实时库 ${lv.rows} 行`
          : `每 ${LIVE_REFRESH_MS / 1000} s 自动刷新`
            + (lastRefreshAt ? ` · 更新于 ${lastRefreshAt}` : '')
            + (lv.rows ? ` · 实时库 ${lv.rows} 行` : '');
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

    // ★ 静默刷新（自动重刷）绝不在取数前清屏：旧屏保持挂载、可交互，
    //   新树在幕后建好后再一次性换上。原实现先 clear + 插"加载中…"再 await 取数
    //   —— 两个任务之间浏览器把空屏画了出来，这就是运行模式每 5 s 闪一次的根源。
    //   非 quiet（点菜单 / 切模式 / 切数据集）仍是用户主动换页，保留"加载中…"。
    //   d0 = 入界时 disposers 的长度：quiet 时数组前段 [0,d0) 是旧屏的定时器，
    //   本次构建新注册的在 [d0, end) —— 换屏 / 跳过 / 失败三条路各自只退该退的那段。
    const d0 = o.quiet ? disposers.length : 0;
    if (!o.quiet) {
      // 先清理上一轮页面留下的定时器 / 监听器
      disposers.splice(0).forEach(fn => { try { fn(); } catch (e) { /* 忽略 */ } });

      clear(content);
      content.appendChild(h('div', { class: 'empty', text: '加载中…' }));
    }

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
      reloadEngine: async () => { await loadDataSource(); },
    };

    try {
      const node = await meta.render(ctx);
      // 数据一字节没变就不换屏：保住悬停态 / 焦点 / 文本选区，肉眼完全无感
      //（outerHTML 对比：树是我们自己 createElement 建的，属性顺序确定，可比）。
      const old = content.firstElementChild;
      const unchanged = o.quiet && old && node.outerHTML === old.outerHTML;
      if (unchanged) {
        // 旧屏原样保留（它的定时器 = 前段 [0,d0)，继续活着）；
        // 这次幕后构建注册的（[d0,end)）随被丢弃的树一起退掉。
        disposers.splice(d0).forEach(fn => { try { fn(); } catch (e) { /* 忽略 */ } });
      } else {
        // 只退旧屏的（quiet 时是 [0,d0)；非 quiet 已在前面清空 = 0 条）。
        // 新树注册的定时器留着 —— 它们属于即将上屏的这一页，下一轮再退。
        disposers.splice(0, d0).forEach(fn => { try { fn(); } catch (e) { /* 忽略 */ } });
        clear(content);
        content.appendChild(node);
        // ★ 页面里自带的 .seg（实时监控的倍速 / 报表系统的日报周报月报）在这里才
        //   第一次有布局，指示条必须等挂载完再对位 —— 页面函数自己调的话读到的是 0。
        placeSegs(content);
        if (o.keepScroll) content.scrollTop = keepTop;
      }
    } catch (e) {
      // ★ 2026-10-17 合并时定的分流规则（两边的策略在这里交叉，必须显式定）：
      //   · 静默刷新 + 已有旧屏   → 保留旧屏（一次抖动不该把数据页打空，下一拍自动重试）
      //   · 静默刷新 + 还没有旧屏 → 画「未就绪」（否则首屏会永远停在"加载中…"）
      //   · 用户主动换页          → 404 画「未就绪 + 出路」，其余才是「加载失败」
      // 注意：只要**换掉了屏**，旧屏的定时器就必须一起退（[0,d0) 那段），否则泄漏。
      const hasOld = !!content.firstElementChild;
      const notReady = isNotReady(e);
      if (o.quiet && hasOld) {
        // 只退本次幕后构建注册的 [d0,end)，旧屏原样保留
        disposers.splice(d0).forEach(fn => { try { fn(); } catch (e2) { /* 忽略 */ } });
        console.warn(notReady ? '自动刷新：数据未就绪（保留上一屏）:' : '自动刷新失败（保留上一屏）:', e);
      } else if (notReady) {
        disposers.splice(0).forEach(fn => { try { fn(); } catch (e2) { /* 忽略 */ } });
        clear(content);
        content.appendChild(renderNotReady(e, ctx));
      } else {
        disposers.splice(0).forEach(fn => { try { fn(); } catch (e2) { /* 忽略 */ } });
        clear(content);
        content.appendChild(h('div', { class: 'empty', text: `加载失败：${e.message}` }));
      }
    }
    syncLiveTimer();
  }

  function drawFooter() {
    const hh = state.health;
    const info = state.engineInfo;
    if (hh) {
      // ★ 左右分工：左边回答"这些数从哪来、是不是真机的"，右边回答
      //   "平台这一侧是什么版本、什么时候灌进来的"。
      //   ★ "模型源 / 现场设备源"必须**常驻**在这儿，不能只挂在异常状态条里 ——
      //     否则数据看着正常的时候，反而没人知道它是不是真机来的。
      const lv = (info && info.live) || {};
      // ★ 报的是"数据源"这个名字，不是"仿真/正式"这种模式名：
      //   现场最需要看清的是"这些数是谁给的"。模拟数据源一定带括号说明
      //   它不是现场装置，因为那才是最容易被误读的一层。
      const kind = state.engine !== 'run' ? '历史库'
        : (lv.data_source_name || '数据源')
          + (lv.source_is_model ? '（模型 · 非现场装置）' : '（现场设备）');
      const sch = ((hh.scenarios || []).find(x => x.scenario_id === state.scenario) || {});
      $('#foot-left').textContent =
        `数据源 ${kind} · 数据集 ${state.scenario || '—'}`
        + (sch.name ? `（${String(sch.name).split(' · ')[0]}）` : '')
        + ` · 采集 ${hh.import_source || '—'}`;
      $('#foot-right').textContent =
        `schema v${hh.schema_version} · 平台层 14/`
        + (hh.imported_at ? ` · 入库于 ${hh.imported_at}` : '')
        + (state.engine === 'run' && lv.rows ? ` · 实时库 ${lv.rows} 行` : '');
    } else {
      $('#foot-left').textContent = '接口未连通';
      $('#foot-right').textContent = '';
    }
  }

  return { boot, state, refreshEngine: loadDataSource, refreshFooter: drawFooter };
})();

document.addEventListener('DOMContentLoaded', App.boot);
