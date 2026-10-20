/* =====================================================================
   15/ — 各页面渲染
   每个页面是一个 async 函数，接收
     ctx = { scenario, user, mode, modes, engine, engineInfo, t,
             setT, setSub, onDispose, reloadEngine }
   返回一个 DOM 节点。数据全部来自 14/ 的 REST 接口。
   ===================================================================== */

const Pages = (() => {

  /* 页面局部偏好的跨渲染记忆。
     ★ 运行模式下页面每 5 s 自动重渲染一次，如果每次都把用户选的曲线、
       报表视角重置回默认，界面会"自己乱跳"——用户刚点掉一条曲线，
       下一秒它又回来了。所以这类纯展示偏好要记在页面之外。 */
  const PREFS = {};

  /* 模拟源面板的跨渲染记忆 + 过渡态补刷（2026-10-19）：
     ① SIM_VIEW —— 页面每 5 s 自动重渲染会重建面板，缓存让重建时先画出上一次
        的状态；否则先清空再等 GET 回来，面板会空一下（看起来像"又坏了"）。
     ② SIM_RETRY —— 「换一个进程」返回的那一刻，采集器往往还在起，后端那一下
        报的是 stopped。面板会停在"采集器未运行"的黄色告警上，直到下次有人点。
        链路在跑而采集器没起是**过渡态**，要几秒后自己再看一眼；
        上限 4 次（10 s），免得真没起时无限轮询。 */
  let SIM_VIEW = null;
  let SIM_RETRY = null;
  let SIM_RETRY_N = 0;

  /* ======================= 1. 总览（首页） ======================= */
  async function overview(ctx) {
    // 跟随「最近一次在实时监控里看的那一拍」；没去过实时监控则取最后一拍（默认）。
    const d = await API.overview(ctx.scenario, ctx.t);
    const mode = (ctx.modes || []).find(m => m.mode_id === ctx.mode) || d.mode || {};
    // ★ 正式运行页面不写「运行模式 / 仿真模式」：那是数据**怎么产生**的，
    //   不是这一页的事。页面只报"哪份数据的哪一拍"，来源性质看页脚那行常驻说明。
    ctx.setSub(`运行模式「${mode.name || '—'}」 · 数据集「${d.scenario_name || d.scenario}」`
      + ` · ${d.time_base === 'wall' ? '实时时刻' : '历史时刻'} ${d.time}`
      + `（t=${fmt.num(d.t_s, 0)} ${d.time_base === 'wall' ? 'Unix s' : 's'}）`);

    const p = d.power, bat = d.battery, eco = d.economics, inv = d.invariants;

    // 关口功率对「并网变压器额定容量」判越限（grid_limit_kw），
    // 而不是对 PCS 允许区间（permission）——后者是储能变流器的动态限值，两者物理量不同。
    const gridLimit = p.grid_limit_kw != null ? p.grid_limit_kw : 500;
    const inRange = Math.abs(p.grid_kw) <= gridLimit + 0.001;
    const batState = p.battery_kw > 0.5 ? { t: '放电', k: 'p-ok' }
                   : p.battery_kw < -0.5 ? { t: '充电', k: 'p-info' }
                   : { t: '保持', k: 'p-mute' };
    const alarmPill = d.alarms.fault > 0 ? { t: `FAULT ${d.alarms.fault}`, k: 'p-bad' }
                    : d.alarms.total > 0 ? { t: `告警 ${d.alarms.total}`, k: 'p-warn' }
                    : { t: '无告警', k: 'p-ok' };

    // 第三张 KPI 卡按运行模式切换口径（独立顶层业务概念）
    const modeKpi = modeKpiOf(mode, eco, bat);
    const kpiRow = h('div', { class: 'grid g4' }, [
      card(null, null, kpi('关口功率', fmt.num(p.grid_kw, 1), 'kW',
        `变压器容量 ${fmt.num(gridLimit, 1)} kW`, 'accent',
        { icon: KPI_ICON.power, pill: inRange ? '未越限' : '越限', pillKind: inRange ? 'p-ok' : 'p-bad' })),
      card(null, null, kpi('储能 SOC', fmt.num(bat.soc * 100, 1), '%',
        `温度 ${fmt.num(bat.temperature_c, 1)} °C`, bat.soc > 0.2 ? 'ok' : 'warn',
        { icon: KPI_ICON.battery, pill: batState.t, pillKind: batState.k })),
      card(null, null, kpi(modeKpi.label, modeKpi.value, modeKpi.unit,
        modeKpi.note, modeKpi.kind,
        { icon: modeKpi.icon, pill: modeKpi.pill, pillKind: modeKpi.pillKind })),
      card(null, null, kpi('系统状态', d.state, '',
        `故障位 ${d.loop.fault_bits} · 告警 ${d.alarms.total} 条`,
        STATE_KIND[d.state] === 'b-bad' ? 'bad' : (STATE_KIND[d.state] === 'b-warn' ? 'warn' : 'ok'),
        { icon: KPI_ICON.shield, pill: alarmPill.t, pillKind: alarmPill.k })),
    ]);

    // 当前运行模式信息条（独立于数据场景）
    const modeCard = modeBar(mode, ctx);
    // ★ 方向不再由徽章承担 —— 它就在粗箭头的朝向与杆内文字里，
    //   所以副标题改口径（旧文案"方向见各侧徽章"已随徽章一起删掉）。
    const flow = card('能量流', '数值为功率大小 · 方向见箭头', h('div', {
      class: 'flow-wrap', html: energyFlow({
        pv_kw: p.pv_kw, load_kw: p.load_kw, grid_kw: p.grid_kw,
        battery_kw: p.battery_kw, soc: bat.soc,
      }),
    }));

    const loopRows = [
      ['本拍指令', fmt.kw(p.command_kw)],
      ['实际功率', fmt.kw(p.battery_kw)],
      ['权限区间', `${fmt.num(p.permission.lower_kw, 1)} ~ ${fmt.num(p.permission.upper_kw, 1)} kW`],
      ['归因', d.loop.reason || '—'],
      ['clamped', d.loop.clamped ? '是' : '否'],
      ['safety_clip', d.loop.safety_clip ? '是' : '否'],
      ['state_gated', d.loop.state_gated ? '是' : '否'],
      ['hold_last', d.loop.hold_last ? '是' : '否'],
    ];
    const loopCard = card('本拍闭环', '07/ StepRecord 的四个布尔',
      h('table', { class: 'tbl' }, h('tbody', null, loopRows.map(([k, v]) =>
        h('tr', null, [h('td', { class: 'muted', text: k }), h('td', { class: 'mono', text: v })])))));

    const invCard = card('安全红线', '全 0 才算过', h('div', { class: 'grid g2' }, [
      kpi('指令越区间', inv.out_of_interval, '拍', 'out_of_interval', inv.out_of_interval ? 'bad' : 'ok'),
      kpi('越设备限值', inv.over_limit, '拍', 'over_limit', inv.over_limit ? 'bad' : 'ok'),
      kpi('门控期非零', inv.gated_nonzero, '拍', 'gated_nonzero', inv.gated_nonzero ? 'bad' : 'ok'),
      kpi('关口越契约', inv.grid_breach, '拍', 'grid_breach', inv.grid_breach ? 'bad' : 'ok'),
      kpi('变压器越限', inv.tr_breach, '拍', 'tr_breach', inv.tr_breach ? 'bad' : 'ok'),
      kpi('SOC 越界', inv.soc_violation, '拍', 'soc_violation', inv.soc_violation ? 'bad' : 'ok'),
    ]));

    const alarmRows = Object.entries(d.alarms.by_level || {})
      .sort((a, b) => b[1] - a[1])
      .map(([lv, n]) => h('tr', null, [
        h('td', null, levelBadge(lv)),
        h('td', { class: 'num', text: String(n) }),
        h('td', { class: 'num muted', text: `${(n / (d.alarms.total || 1) * 100).toFixed(1)} %` }),
      ]));
    const alarmCard = card('告警分档', `共 ${d.alarms.total} 条 / FAULT ${d.alarms.fault} 条`,
      alarmRows.length
        ? h('table', { class: 'tbl' }, [h('thead', null, h('tr', null, [
          h('th', { text: '等级' }), h('th', { class: 'num', text: '条数' }), h('th', { class: 'num', text: '占比' })])),
          h('tbody', null, alarmRows)])
        : h('div', { class: 'empty', text: '本场景无告警' }));

    return h('div', { class: 'grid' }, [
      kpiRow,
      modeCard,
      h('div', { class: 'grid g-2-1' }, [flow, h('div', { class: 'grid' }, [loopCard, alarmCard])]),
      h('div', { class: 'grid g3' }, [invCard, econCard(eco, d), tipCard(d)]),
    ]);
  }

  // 按运行模式切换第三张 KPI 卡的口径（独立顶层业务概念）
  function modeKpiOf(mode, eco, bat) {
    const mid = mode.mode_id || 'arbitrage';
    switch (mid) {
      case 'peak_shaving':
        return {
          label: '当日节省', value: fmt.money(eco.saving_total_cny, 0), unit: '元',
          note: '相对无储能基线（自用型：少买高价电网电 + 需量下降）', kind: 'ok',
          icon: KPI_ICON.coin, pill: `节省率 ${fmt.pct(eco.saving_pct, 2)}`, pillKind: 'p-ok',
        };
      case 'ancillary':
        return {
          label: '服务净收益', value: fmt.money(eco.net_benefit_cny, 0), unit: '元',
          note: '调频/调峰/备用服务费（容量费 + 里程费口径）', kind: 'ok',
          icon: KPI_ICON.coin, pill: '响应调度', pillKind: 'p-info',
        };
      case 'backup':
        return {
          label: '保供 SOC', value: fmt.num(bat.soc * 100, 1), unit: '%',
          note: '满充待命 · 停电时顶关键负荷（保供优先，不计收益）', kind: 'accent',
          icon: KPI_ICON.shield, pill: '待命', pillKind: 'p-mute',
        };
      default: // arbitrage（峰谷套利，默认）
        return {
          label: '当日净收益', value: fmt.money(eco.net_benefit_cny, 0), unit: '元',
          note: '卖电收入 − 充电成本 − 电池衰减（卖电型：峰谷价差套利）', kind: 'ok',
          icon: KPI_ICON.coin, pill: `节省 ${fmt.pct(eco.saving_pct, 2)}`, pillKind: 'p-ok',
        };
    }
  }

  // 当前运行模式信息条：目标函数 + 结算口径 + 绑定策略
  function modeBar(mode, ctx) {
    if (!mode || !mode.mode_id) return h('div');
    const strategies = mode.bound_strategies || [];
    return card('运行模式', `${mode.customer_type || ''}`, h('div', null, [
      h('div', { class: 'mode-row' }, [
        h('span', { class: 'pill p-accent', text: mode.name }),
        h('span', { class: 'muted', text: mode.objective || '' }),
      ]),
      h('div', { class: 'mode-row', style: 'margin-top:6px' }, [
        h('span', { class: 'muted', text: '结算：' }),
        h('span', { text: mode.settlement || '' }),
      ]),
      (mode.constraints || []).length ? h('div', { class: 'mode-row', style: 'margin-top:6px' }, [
        h('span', { class: 'muted', text: '约束：' }),
        ...(mode.constraints || []).map(c => h('span', { class: 'pill p-mute', text: c })),
      ]) : null,
      h('div', { class: 'mode-row', style: 'margin-top:6px' }, [
        h('span', { class: 'muted', text: '绑定策略：' }),
        ...(strategies.length
          ? strategies.map(s => h('span', { class: 'mono pill p-info', text: s }))
          : [h('span', { class: 'muted', text: '（仅安全底座，无业务策略）' })]),
      ]),
    ]));
  }

  function econCard(eco, d) {
    return card('经济性', '按当前数据集核算', h('div', null, [
      ratioRow('净收益', eco.net_benefit_cny, Math.max(eco.saving_total_cny, 1), 'var(--ok)', ' 元'),
      ratioRow('节省电费', eco.saving_total_cny, Math.max(eco.cost_total_base_cny, 1), 'var(--accent)', ' 元'),
      ratioRow('实际电费', eco.cost_total_cny, Math.max(eco.cost_total_base_cny, 1), 'var(--c-grid)', ' 元'),
      ratioRow('无储能基线', eco.cost_total_base_cny, Math.max(eco.cost_total_base_cny, 1), 'var(--text-3)', ' 元'),
      h('p', { class: 'muted', style: 'font-size:11.5px;margin:10px 0 0' ,
        text: '口径：按分时电价与需量计费核算（含电池衰减成本）；不是电网结算账单。' }),
    ]));
  }

  function tipCard(d) {
    return card('数据来源', null, h('div', { style: 'font-size:12.5px;line-height:1.9' }, [
      h('p', { style: 'margin:0 0 8px', html:
        '页面上的量全部来自<strong>现场采集通道</strong>：设备侧进程逐拍写入平台库，界面按当前数据集渲染。' +
        '通道与点表在 <span class="mono">13/</span>（Modbus 主站）、<span class="mono">21/</span>（DL/T645 电能表）、' +
        '<span class="mono">07/</span>（实时库）里定义。' }),
      h('p', { class: 'muted', style: 'margin:0', html:
        '当前这份数据是<strong>模型源</strong>还是<strong>现场设备源</strong>，看页脚左侧那行 —— ' +
        '它由后端标记决定，不靠猜。换真机时<strong>表结构与接口都不用改</strong>。' }),
    ]));
  }

  /* ======================= 2. 实时监控 ======================= */
  // SOC 圆环（自绘 SVG；只插数值，不拼接口字符串）
  function socRing(o) {
    const pct = Math.max(0, Math.min(100, Math.round((o.soc || 0) * 100)));
    const R = 52, C = 2 * Math.PI * R;
    const off = (C * (1 - pct / 100)).toFixed(1);
    const color = pct >= 60 ? 'var(--ok)' : pct >= 20 ? 'var(--accent)' : 'var(--warn)';
    return `<svg viewBox="0 0 130 130" class="rt-ring" role="img">
      <circle cx="65" cy="65" r="${R}" fill="none" stroke="var(--line-soft)" stroke-width="11"/>
      <circle cx="65" cy="65" r="${R}" fill="none" stroke="${color}" stroke-width="11"
        stroke-linecap="round" stroke-dasharray="${C.toFixed(1)}" stroke-dashoffset="${off}"
        transform="rotate(-90 65 65)"/>
      <text x="65" y="61" text-anchor="middle" font-size="23" font-weight="700" fill="var(--text)">${pct}%</text>
      <text x="65" y="79" text-anchor="middle" font-size="11" fill="var(--text-3)">SOC</text>
    </svg>`;
  }

  // 四联功率卡：色点 + 名称 + 方向 pill + 大数字（取绝对值，方向交给 pill 表达）
  function pwCard(label, kw, color, dirText, dirKind) {
    const v = kw === null || kw === undefined ? 0 : kw;
    return h('div', { class: 'card rt-pw' }, [
      h('div', { class: 'rt-pw-top' }, [
        h('i', { style: `background:${color}` }),
        h('span', { class: 'rt-pw-label', text: label }),
        dirText ? h('span', { class: `pill ${dirKind}`, text: dirText }) : null,
      ]),
      h('div', { class: 'rt-pw-val' }, [
        fmt.num(Math.abs(v), 1),
        h('span', { class: 'unit', text: 'kW' }),
      ]),
    ]);
  }

  // 右侧闭环信息的一行「名-值」
  function infoRow(k, v) {
    return h('div', { class: 'rt-row' }, [
      h('span', { class: 'muted', text: k }),
      h('span', { class: 'mono', text: String(v) }),
    ]);
  }

  async function realtime(ctx) {
    ctx.setSub('拖动时间轴，查看任意时刻的全站快照');

    /* ---- 全天 SOC 走势：取一次，之后只移动游标 ----
       顺带把时间轴的上下界定下来。★ 范围必须**取自数据本身**，不能写死 0~86400：
       wall 口径（现场实录入库的那份）的 t_s 是 Unix 墙钟秒，写死 86400 会把整条轴
       压到一天之内 —— 滑块能拖、每一格取到的却是同一拍，
       "看着有交互、其实什么都没变"，比没有滑块更糟。 */
    let dayT = [], daySoc = [], dayLabels = [], dayTimeBase = 'sim';
    try {
      const dv = await API.series(ctx.scenario, ['soc'], 145);
      dayT = dv.t_s || [];
      daySoc = ((dv.series || {}).soc || []).map(v => (v === null || v === undefined) ? null : v * 100);
      dayLabels = dv.time || [];
      dayTimeBase = dv.time_base || 'sim';
    } catch (e) { dayT = []; }

    /* ★ 滑块的值域是**采样点下标**，不是秒。这一条改过，别再退回秒：
       早先按 [首拍, 末拍] 铺 864 格，再由后端取「≤ t 的最后一拍」。数据连续时
       两者等价（sim 是 8640 拍均匀的 24 h），但 live 数据集是**跨会话累积**的 ——
       库里现在就横着「9-30 的 12 h」+「今天 12 min」，中间空 18 天。轴按整段算
       时步长 ≈1812 s，97% 的位置落在空档里，后端一律回落到同一个旧拍：
       拖动有反应、画面纹丝不动，看着像卡死（而且比没有滑块更糟）。
       改成下标后每个位置都落在**数据里真实存在的一拍**上，断档只会表现为
       "某一格跳得远"，不会再有取不到数的位置。 */
    const N = dayT.length;
    const nearIdx = (v) => {
      let bi = 0, bd = Infinity;
      for (let i = 0; i < N; i++) {
        const d = Math.abs(Number(dayT[i]) - v);
        if (d < bd) { bd = d; bi = i; }
      }
      return bi;
    };
    // 回到本页时停在上次看的那一拍：app 层的 state.t 跨页共享，
    // 没看过（null）才给最后一拍。原实现每次进页都从末尾起，切走再回来就被拉回去。
    let tIdx = (ctx.t === null || ctx.t === undefined)
      ? Math.max(0, N - 1) : nearIdx(Number(ctx.t));
    /* 取不到采样点（series 挂了）就发 -1 = 「最新一拍」，让后端自己挑。
       不能拿 0 顶：wall 口径下 t_s=0 是 1970 年，后端必然找不到 ≤ 0 的行 → 404。 */
    let t = N ? Number(dayT[tIdx]) : -1;

    /* ---- 时刻查询条：当前时刻 + 时间轴 + 跳到最新 ----
       ★ 这里原先是"回放控制台"（播放键 / 倍速 / 24 h 定长轴）。产品里没有"播放"
         这个概念 —— 用户要的是"看看昨天 23:40 那一拍是什么样"，
         所以留时间轴 + 时刻读数，去掉播放与倍速。 */
    /* ---- 数据源说明条 ----
       ★ 这一页以前从头到尾不提"你在看哪份数据"。于是「仿真数据集的末拍
         t_s=86400 被折成 00:00」这件事就变成一个无解的问号：标题写着
         「实时监控」、读数永远是 0:00，也没任何地方说这一页其实在看离线仿真。
         两个模式各自的 dataset / 行数 / 采集状态都取自同一个 `/api/engine`
         （与后端 `default_scenario` 同源），不在这里另猜一份。 */
    const ei = ctx.engineInfo || {};
    const pickEng = (id) => (ei.engines || []).find(e => e.id === id) || {};
    const curEng = pickEng(ctx.engine);
    const othEng = pickEng(ctx.engine === 'run' ? 'sim' : 'run');
    const rx = (n) => Number(n || 0).toLocaleString('zh-CN');
    const srcName = h('span', { class: 'mono', text: curEng.dataset || ctx.scenario || '—' });
    const srcCard = h('div', { class: 'card' }, [
      h('div', { class: 'card-head' }, [
        h('h2', { text: '这一页在看哪份数据' }),
        /* 徽章只写「实时 / 离线」，不写「运行模式 / 仿真模式」—— 后者是研发语境
           （页脚与设备页统一用 data_source_name），而且 app 里「运行模式」这个词
           已经被业务侧（峰谷套利 / 削峰填谷）占用了，同屏出现两个意思必被误读。 */
        badge(dayTimeBase === 'wall' ? '实时数据' : '离线数据',
              dayTimeBase === 'wall' ? 'b-ok' : 'b-info'),
      ]),
      h('div', { class: 'rt-row' }, [
        h('span', { class: 'muted', text: '数据集' }), srcName,
      ]),
      h('div', { class: 'rt-row' }, [
        h('span', { class: 'muted', text: '样本量' }),
        h('span', { class: 'mono', text: `${rx(curEng.rows)} 行` }),
      ]),
      h('div', { class: 'rt-row' }, [
        h('span', { class: 'muted', text: '时刻口径' }),
        h('span', {
          text: dayTimeBase === 'wall' ? '墙钟秒（正在发生）' : '一天内秒（离线仿真）',
        }),
      ]),
      /* 看离线数据而采集却在跑 —— 这一行是必须的：不说就等于默认用户在看实时，
         正是"0:00 不动"被读成"系统卡了"的原因。
          ★ 只说清"另一边在写"，不写"点这里切换" —— 界面并没有那个入口，
            指到一条不存在的路上比不说更糟（这个缺口单独提给产品决策）。 */
      ctx.engine === 'run' ? null : h('div', { class: 'rt-sub' }, [
        h('span', {
          text: `实时采集正在写入「${othEng.dataset || 'live'}」`
            + `（${rx(othEng.rows)} 行${(ei.live || {}).running ? '，进行中' : '，当前未运行'}）`
            + ` —— 这一页看的不是它。`,
        }),
      ]),
    ]);

    const clock = h('span', { class: 'rt-clock', text: '—' });
    const stateHost = h('span');
    const slider = h('input', {
      type: 'range', min: '0', max: String(Math.max(0, N - 1)), step: '1',
      value: String(tIdx), disabled: N === 0,
    });
    /* 轴两端只写时刻：sim 口径后端给的本来就是 HH:MM；
       wall 口径是「2026-09-30 00:57:20」，照原样贴上去会把滑块挤到没地方。 */
    const edgeLabel = (i) => {
      const s2 = dayLabels[i] || '';
      const m = /^\d{4}-\d\d-\d\d[ T](\d\d:\d\d)/.exec(s2);
      return m ? m[1] : (s2 || (N ? fmt.hhmm(Number(dayT[i])) : '—'));
    };
    const consoleCard = h('div', { class: 'card rt-console' }, [
      h('div', { class: 'rt-clock-box' }, [clock, h('span', { class: 'rt-sub', text: '数据时刻' })]),
      stateHost,
      h('div', { class: 'rt-slider' }, [
        h('span', { class: 'rt-sub', text: edgeLabel(0) }),
        slider,
        h('span', { class: 'rt-sub', text: edgeLabel(N - 1) }),
      ]),
      h('button', {
        class: 'btn-sm', text: '跳到最新', disabled: N === 0,
        // ★ 必须现取末点的值，不能用闭包里那个 t：拖过轴之后 t 已经是旧值，
        //   点「跳到最新」会把人送回刚才那一拍。
        onclick: () => {
          slider.value = String(Math.max(0, N - 1));
          render(N ? Number(dayT[N - 1]) : -1);
        },
      }),
      h('span', { class: 'rt-sub',
        text: N ? `${N} 个采样点 · 逐点吸附` : '没有采样点' }),
    ]);

    const pwHost = h('div');
    /* ---- 能量流：整张卡只建一次，查询时只 patch 数字 ----
       ★ 为什么不能每次重建：粗箭头杆里的流动光带是 CSS 动画（stroke-dashoffset
         在跑），元素一旦被整段换掉，新元素就从相位 0 重新开始 ——
         每拖一格换一次，看着就是"胶囊被反复拽回起点"，也就是卡顿。
        ★ 所以：结构签名（哪几路有功率、朝哪边）不变时只改文本与 SOC 条宽度，
         元素原地保留，动画一直连着跑；签名变了才整张重建（那时"方向"本身
         变了，重画是对的）。
        ★ 签名口径必须与 ui.js 里 arrow() 的 active/方向判定逐条对上，
         否则会出现"读数变了但箭头没跟着变"。 */
    const flowWrap = h('div', { class: 'flow-wrap' });
    const flowSub = h('span', { class: 'sub' });
    const flowHost = h('div', null, h('div', { class: 'card' }, [
      h('div', { class: 'card-head' }, [h('h2', { text: '能量流' }), flowSub]),
      flowWrap,
    ]));
    let flowSig = null;
    const flowArgs = (p, soc) => ({
      pv_kw: p.pv_kw, load_kw: p.load_kw, grid_kw: p.grid_kw,
      battery_kw: p.battery_kw, soc,
    });
    const flowSignature = (p) => [
      p.pv_kw > 0.5,            // 光伏：发电 / 停机
      p.battery_kw < -0.5,      // 储能：充电（箭头反向：母线 → 储能）
      p.battery_kw > 0.5,       // 储能：放电（储能 → 母线）
      p.load_kw > 0.5,          // 负荷：用电 / 无载
      p.grid_kw < -0.5,         // 电网：售电（箭头反向：母线 → 电网）
      p.grid_kw > 0.5,          // 电网：购电（电网 → 母线）
    ].map(b => (b ? 1 : 0)).join('');

    // ★ 右列两张卡（储能与闭环 / 全天 SOC 走势）用 .rt-col —— 与全站 .grid 同一档
    //   14px 间距。原来是裸 div，两张卡贴着长在一起，比别的板块密。
    const battHost = h('div', { class: 'rt-col' });
    const devHost = h('div');
    const dayHost = h('div');

    // 设备类型 → 色点（与图表序列色一致，眼睛不用重新学一套颜色）
    const TYPE_COLOR = {
      PCS: 'var(--accent)', BMS: 'var(--c-bat)', 电表: 'var(--info)',
      光伏: 'var(--c-pv)', 变压器: 'var(--c-grid)', 负荷: 'var(--c-load)',
    };

    // ★ 请求序号防乱序：拖时间轴会并发多次 render，慢的旧响应若最后
    //   回来，会把更早的请求结果覆盖到"更新"的画面上。给每次 render 一个
    //   自增序号，await 回来后比对，非最新一次直接丢弃。
    let renderSeq = 0;
    async function render(tt) {
      const seq = ++renderSeq;
      const [rt, ov] = await Promise.all([
        API.realtime(ctx.scenario, tt),
        API.overview(ctx.scenario, tt),
      ]);
      if (seq !== renderSeq) return;   // 已有更新的请求，丢弃本次过期结果
      ctx.setT(tt);                    // 记录当前时刻，供总览页跟随
      clock.textContent = rt.time;
      /* ★ 数据集名写进说明条 / 副标题：这个时刻属于**哪一份数据**必须跟着读数走，
         否则同一个 "00:00" 到底是"今天的第 0 秒"还是"离线仿真最后一拍"，没人看得出来。 */
      const _dn = ov.scenario_name || curEng.dataset || ctx.scenario || '—';
      srcName.textContent = _dn === (curEng.dataset || '') ? _dn
        : `${_dn}（${curEng.dataset || ctx.scenario || '—'}）`;
      ctx.setSub(`数据集「${_dn}」`
        + ` · ${dayTimeBase === 'wall' ? '实时时刻' : '历史时刻'} ${rt.time}`
        + ` · 拖动时间轴看任意一拍`);
      clear(stateHost);
      stateHost.appendChild(stateBadge(ov.state));

      const p = ov.power;

      /* ---- 四联功率带 ---- */
      const pvDir = p.pv_kw > 0.5 ? ['发电中', 'p-ok'] : ['停机', 'p-mute'];
      const gridDir = p.grid_kw > 0.5 ? ['购电', 'p-warn'] : p.grid_kw < -0.5 ? ['售电', 'p-ok'] : ['无交换', 'p-mute'];
      const batDir = p.battery_kw > 0.5 ? ['放电', 'p-ok'] : p.battery_kw < -0.5 ? ['充电', 'p-info'] : ['静置', 'p-mute'];
      clear(pwHost);
      pwHost.appendChild(h('div', { class: 'grid g4' }, [
        pwCard('光伏', p.pv_kw, 'var(--c-pv)', pvDir[0], pvDir[1]),
        pwCard('负荷', p.load_kw, 'var(--c-load)', '用电中', 'p-info'),
        pwCard('电网', p.grid_kw, 'var(--c-grid)', gridDir[0], gridDir[1]),
        pwCard('储能', p.battery_kw, 'var(--c-bat)', batDir[0], batDir[1]),
      ]));

      // 能量流随时刻走：只看数字表不容易看出"谁在供谁"，
      // 一张图能立刻回答"这一拍是电网在供、还是电池在放"。
      // ★ 只 patch：结构没变就不换元素，杆内光带的动画不会被打断（见上面 flowSig 注释）。
      flowSub.textContent = rt.time;
      const flowSvg = flowWrap.querySelector('svg.flow');
      const sig = flowSignature(p);
      const fargs = flowArgs(p, ov.battery.soc);
      if (!flowSvg || sig !== flowSig) {
        flowSig = sig;
        clear(flowWrap);
        flowWrap.innerHTML = energyFlow(fargs);
      } else {
        energyFlowPatch(flowSvg, fargs);
      }

      /* ---- 储能与闭环（右列） ---- */
      const lp = ov.loop;
      clear(battHost);
      battHost.appendChild(card('储能与闭环', '本拍', h('div', null, [
        h('div', { class: 'rt-batt' }, [
          h('div', { html: socRing({ soc: ov.battery.soc }) }),
          h('div', { class: 'rt-batt-info' }, [
            infoRow('电池温度', fmt.num(ov.battery.temperature_c, 1) + ' °C'),
            infoRow('额定容量', fmt.num(ov.battery.capacity_kwh, 0) + ' kWh'),
            infoRow('指令 / 实际', `${fmt.num(p.command_kw, 1)} / ${fmt.num(p.battery_kw, 1)} kW`),
            infoRow('权限区间', `${fmt.num(p.permission.lower_kw, 0)} ~ ${fmt.num(p.permission.upper_kw, 0)} kW`),
          ]),
        ]),
        h('div', { style: 'display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin-top:11px' }, [
          lp.clamped ? badge('clamped', 'b-warn') : null,
          lp.safety_clip ? badge('safety_clip', 'b-warn') : null,
          lp.state_gated ? badge('state_gated', 'b-info') : null,
          lp.hold_last ? badge('hold_last', 'b-mute') : null,
          (!lp.clamped && !lp.safety_clip && !lp.state_gated && !lp.hold_last)
            ? badge('四布尔全否（正常）', 'b-ok') : null,
          lp.reason ? h('span', { class: 'muted', style: 'font-size:11.5px', text: lp.reason }) : null,
        ]),
      ])));

      // 全天 SOC 走势（游标跟随上一行那颗 SOC 圆环的当前拍）
      if (dayT.length) {
        clear(dayHost);
        dayHost.appendChild(svgNode(sparkChart({
          height: 96, yMin: 0, yMax: 100,
          color: 'var(--c-bat)',
          values: daySoc,
          labels: dayLabels,
          marker: { i: nearIdx(Number(tt)), label: rt.time },
        })));
        const lo = Math.min(...daySoc.filter(v => v !== null));
        const hi = Math.max(...daySoc.filter(v => v !== null));
        const now = daySoc[nearIdx(Number(tt))];
        battHost.appendChild(card('全天 SOC 走势',
          `当前 ${fmt.num(now, 1)} % · 全天 ${fmt.num(lo, 1)}~${fmt.num(hi, 1)} %`, dayHost));
      }

      /* ---- 设备实时量（全宽） ---- */
      clear(devHost);
      devHost.appendChild(card('设备实时量', `${rt.time}`,
        table([
          { title: '设备', render: r => {
              const tp = r.device_type || typeOf(r.device_id);
              return h('span', { class: 'rt-dev' }, [
                h('i', { style: `background:${TYPE_COLOR[tp] || 'var(--text-3)'}` }),
                h('span', { class: 'mono', text: r.device_id }),
              ]);
            } },
          { title: '类型', render: r => badge(r.device_type || typeOf(r.device_id), 'b-mute') },
          { title: '功率', num: true, render: r => {
              if (r.power_kw === null || r.power_kw === undefined) return '—';
              // 负值=充电/馈电，用主色蓝加粗提示方向
              const neg = r.power_kw < -0.005;
              return h('span', {
                style: 'white-space:nowrap;' + (neg ? 'color:var(--accent);font-weight:600' : ''),
                text: fmt.num(r.power_kw, 2) + ' kW',
              });
            } },
          { title: 'SOC', num: true, render: r => {
              if (r.soc === null || r.soc === undefined) return '—';
              const pct = r.soc * 100;
              return h('span', { class: 'rt-soc' }, [
                h('i', { style: `width:${Math.round(pct * 0.56)}px` }),
                h('em', { text: fmt.num(pct, 1) + ' %' }),
              ]);
            } },
          { title: '温度', num: true, render: r => {
              if (r.temperature_c === null || r.temperature_c === undefined) return '—';
              // 仅展示着色（不作告警判定）：≥32 °C 橙、≥40 °C 红
              const c = r.temperature_c >= 40 ? 'var(--bad)' : r.temperature_c >= 32 ? 'var(--warn)' : '';
              return h('span', {
                style: 'white-space:nowrap;' + (c ? `color:${c};font-weight:600` : ''),
                text: fmt.num(r.temperature_c, 1) + ' °C',
              });
            } },
          { title: '品质', render: r => badge(r.quality_ok ? 'GOOD' : 'UNCERTAIN', r.quality_ok ? 'b-ok' : 'b-warn') },
          { title: '状态', render: r => r.status ? stateBadge(r.status) : h('span', { class: 'muted', text: '—' }) },
        ], rt.items, { wrap: false })));
    }

    // ★ 拖动节流：input 事件每动一下都 render 会瞬间并发一堆请求，
    //   用 requestAnimationFrame 合并同一帧内的多次拖动，只发最后一次。
    let rafPending = false, lastV = t;
    slider.addEventListener('input', () => {
      lastV = N ? Number(dayT[Number(slider.value)]) : -1;
      if (rafPending) return;
      rafPending = true;
      requestAnimationFrame(() => {
        rafPending = false;
        render(lastV);
      });
    });

    await render(t);
    return h('div', { class: 'grid' }, [
      srcCard, consoleCard, pwHost,
      h('div', { class: 'grid g-2-1' }, [flowHost, battHost]),
      devHost,
    ]);
  }

  function typeOf(id) {
    if (id.includes('PCS')) return 'PCS';
    if (id.includes('BMS')) return 'BMS';
    if (id.includes('METER')) return '电表';
    if (id.includes('PV')) return '光伏';
    if (id.includes('TR')) return '变压器';
    if (id.includes('LOAD')) return '负荷';
    return '—';
  }

  /* ======================= 3. 历史曲线 ======================= */
  async function curves(ctx) {
    ctx.setSub('等间隔降采样；可多选曲线对照');
    const KEYS = [
      ['p_grid_kw', '关口功率', 'var(--c-grid)'],
      ['p_load_kw', '负荷', 'var(--c-load)'],
      ['p_pv_kw', '光伏', 'var(--c-pv)'],
      ['p_actual_kw', '储能实际', 'var(--c-bat)'],
      ['p_cmd_kw', '储能指令', 'var(--text-3)'],
      ['p_upper_kw', '权限上界', 'var(--warn)'],
      ['p_lower_kw', '权限下界', 'var(--warn)'],
    ];
    const chosen = new Set((PREFS.curves && PREFS.curves.keys)
      || ['p_grid_kw', 'p_load_kw', 'p_pv_kw', 'p_actual_kw']);
    const chartHost = h('div');
    const legendHost = h('div');
    const pvSlider = h('input', { type: 'range', min: '48', max: '960', step: '48',
      value: String((PREFS.curves && PREFS.curves.points) || 288) });
    const pvLabel = h('span', { class: 'muted', style: 'font-size:12px', text: `${pvSlider.value} 点` });
    const rememberCurves = () => {
      PREFS.curves = { keys: Array.from(chosen), points: Number(pvSlider.value) };
    };

    // ★ 请求序号防乱序：连续拖动采样滑块会并发多次 /api/series，
    //   慢的旧响应可能覆盖新响应，导致"滑到 960 点却显示 48 点的旧图"。
    let drawSeq = 0;
    let curSeries = [], curLabels = [];
    async function draw() {
      const seq = ++drawSeq;
      if (!chosen.size) { chartHost.innerHTML = '<div class="empty">请至少选择一条曲线</div>'; return; }
      const pts = Number(pvSlider.value);
      const d = await API.series(ctx.scenario, Array.from(chosen), pts);
      if (seq !== drawSeq) return;   // 已有更新的请求，丢弃过期结果
      const series = Array.from(chosen).map(k => {
        const meta = KEYS.find(x => x[0] === k);
        return { name: meta[1], label: meta[1], color: meta[2], values: d.series[k] };
      });
      curSeries = series;
      curLabels = d.time || [];
      chartHost.innerHTML = lineChart({ series, xLabels: d.time, height: 330, yMin: 0, cursor: true });
      bindCursor();
      clear(legendHost);
      legendHost.appendChild(chartLegend(series));
      pvLabel.textContent = `${d.points} 点`;
    }

    // 游标联动：mousemove 反算最近数据点，驱动竖线 / 圆点 / tooltip。
    // ★ 图表用了 preserveAspectRatio="none"，viewBox 会被拉伸，反算坐标必须
    //   按实际显示尺寸做比例换算，不能用 getScreenCTM。
    function bindCursor() {
      const svg = chartHost.querySelector('svg.chart');
      if (!svg) return;
      const line = svg.querySelector('.chart-cursor-line');
      const dots = svg.querySelectorAll('.chart-cursor-dot');
      const tip = svg.querySelector('.chart-cursor-tip');
      const tipText = svg.querySelector('.chart-cursor-tip-text');
      if (!line || !tip || !tipText) return;
      const W = 900, H = 330;
      const padL = 54, padR = 14, padT = 12, padB = 26;
      const iw = W - padL - padR, ih = H - padT - padB;
      const n = Math.max(1, ...curSeries.map(s => (s.values || []).length));

      function clearCursor() {
        line.style.display = 'none';
        dots.forEach(dt => dt.style.display = 'none');
        tip.style.display = 'none';
      }

      svg.addEventListener('mousemove', (ev) => {
        const rect = svg.getBoundingClientRect();
        const vx = (ev.clientX - rect.left) / rect.width * W;   // 比例换算到 viewBox 坐标
        if (vx < padL || vx > W - padR) { clearCursor(); return; }
        const idx = Math.round((vx - padL) / iw * (n - 1));
        const px = padL + (n === 1 ? iw / 2 : iw * idx / (n - 1));

        line.setAttribute('x1', px.toFixed(1));
        line.setAttribute('x2', px.toFixed(1));
        line.style.display = '';

        const tipLines = [];
        dots.forEach((dt, si) => {
          const s = curSeries[si];
          if (!s) { dt.style.display = 'none'; return; }
          const v = s.values[idx];
          if (v === null || v === undefined) { dt.style.display = 'none'; return; }
          // 从 data-points 反查该点的精确 y（与曲线同一 yMin/yMax/padding）
          const coords = (dt.getAttribute('data-points') || '').split(' ').filter(Boolean);
          const xy = coords[idx] ? coords[idx].split(',') : null;
          if (!xy) { dt.style.display = 'none'; return; }
          dt.style.display = '';
          dt.setAttribute('cx', xy[0]);
          dt.setAttribute('cy', xy[1]);
        });

        // tooltip：显示该时刻各系列的值
        const label = curLabels[idx] || fmt.hhmm(idx * 10);
        tipLines.push(label);
        curSeries.forEach(s => {
          const v = s.values[idx];
          if (v === null || v === undefined) return;
          tipLines.push(`${s.label} ${fmt.num(v, 1)}`);
        });
        tipText.textContent = tipLines.join(' · ');

        // 定位 tooltip（宽度动态，贴边翻转）
        const textW = tipText.getComputedTextLength();
        const tipW = textW + 20;
        let tx = px + 8;
        if (tx + tipW > W - 4) tx = px - 8 - tipW;
        tip.setAttribute('transform', `translate(${tx.toFixed(1)},0)`);
        const rectEl = tip.querySelector('rect');
        rectEl.setAttribute('x', '0');
        rectEl.setAttribute('width', tipW.toFixed(1));
        tipText.setAttribute('x', (tipW / 2).toFixed(1));
        tip.style.display = '';
      });

      svg.addEventListener('mouseleave', clearCursor);
    }

    const toggles = h('div', { style: 'display:flex;flex-wrap:wrap;gap:9px;margin-bottom:14px' },
      KEYS.map(([k, name, color]) => {
        const cb = h('input', { type: 'checkbox', checked: chosen.has(k) });
        cb.addEventListener('change', () => {
          if (cb.checked) chosen.add(k); else chosen.delete(k);
          rememberCurves();
          draw();
        });
        return h('label', { style: 'display:flex;align-items:center;gap:6px;font-size:12.5px;cursor:pointer' },
          [cb, h('i', { style: `width:9px;height:9px;border-radius:2px;background:${color};display:inline-block` }), name]);
      }));

    const socHost = h('div');

    async function drawSoc() {
      const d = await API.series(ctx.scenario, ['soc'], 288);
      socHost.innerHTML = lineChart({
        series: [{ name: 'SOC', label: 'SOC', color: 'var(--ok)', values: d.series.soc.map(v => v * 100) }],
        xLabels: d.time, height: 170, yMin: 0, yMax: 100,
      });
    }

    // ★ 拖动节流：合并同一帧内的多次 input，只发最后一次（防请求堆积）。
    let pvRaf = false, pvLast = Number(pvSlider.value);
    pvSlider.addEventListener('input', () => {
      pvLast = Number(pvSlider.value);
      pvLabel.textContent = `${pvLast} 点`;
      rememberCurves();
      if (pvRaf) return;
      pvRaf = true;
      requestAnimationFrame(() => { pvRaf = false; draw(); });
    });

    await draw();
    await drawSoc();

    const exportRow = h('div', { style: 'display:flex;gap:9px;margin-bottom:13px;flex-wrap:wrap' }, [
      h('button', { class: 'btn-sm', text: '导出时序 CSV', onclick: () => API.download('timeseries', ctx.scenario) }),
      h('button', { class: 'btn-sm', text: '导出指令 CSV', onclick: () => API.download('commands', ctx.scenario) }),
      h('button', { class: 'btn-sm', text: '导出功率图 PNG', onclick: () => {
        const svg = chartHost.querySelector('svg.chart');
        if (svg) exportSvgPng(svg, '功率曲线.png');
      } }),
      h('button', { class: 'btn-sm', text: '导出 SOC 图 PNG', onclick: () => {
        const svg = socHost.querySelector('svg.chart');
        if (svg) exportSvgPng(svg, 'SOC曲线.png');
      } }),
    ]);

    return h('div', { class: 'grid' }, [
      exportRow,
      card('曲线选择', '拖动下方点数滑块可改变采样密度', h('div', null, [toggles, h('div', { class: 'replay' }, [pvSlider, pvLabel])])),
      card('功率曲线', 'kW · 小时刻度', h('div', null, [chartHost, legendHost])),
      card('SOC 曲线', '% · 24 h', socHost),
    ]);
  }

  /* ======================= 4. 告警中心 =======================
     ★ 分类维度开哪些，是拿真实数据定的，不是拍脑袋
       （探针脚本 14/scripts/probe_alarm_dims.py，每次改完导入都要重跑）：

       —— 改前（只有 normal + fault 两个场景，290 行）——
         level      3 个取值（FAULT 272 / INFO 12 / DERATED 6）  → 可筛
         source     2 个取值（COMM 270 / FSM 20）               → 可筛
         status     1 个取值（active 290）                       → ★ 不开放
         device_id  1 个取值（DEV-BMS-01 290）                   → ★ 不开放

       —— 改后（加入 --stress 场景 + COMM 去抖 + 部位映射，39 行）——
         level      4 个取值（INFO 22 / DERATED 9 / FAULT 5 / WARNING 3）
         source     5 个取值（FSM 29 / COMM 6 / TEMP 2 / TRANSFORMER 1 / SOC 1）
         device_id  4 个取值（BMS 34 / PCS 2 / METER 2 / TR 1）
         status     1 个取值（active 39）                       → ★ 仍不开放

       ★ 行数从 290 掉到 39，是因为 COMM 从"逐拍刷屏 270 条"去抖成了 6 条
         （故障位不变也每 10 s 记一遍同一句话）。少的是噪音，不是信息。
       ★ status 至今仍只有 active：importer.py 把所有行写死成 active，
         SET/CLEAR 配对要等接 20/ 装配器才有 —— 在拿到真实配对之前，
         把它做成筛选项就是"恒 0 条"的假入口，比不给这个选项更糟。
     ============================================================ */
  async function alarms(ctx) {
    /* 等级全谱：与后端 h_alarm_summary 的 ALARM_LEVELS 同一份清单。
       后端已把未发生的等级补成 n=0，这里照同一份清单渲染 —— 界面上
       「这一档没告警」和「这一档没统计到」才分得开（两者长得一模一样）。 */
    const LEVELS = ['INFO', 'WARNING', 'DERATED', 'FAULT', 'EMERGENCY'];

    /* ★ 筛选条件必须跨渲染记住：本页在 LIVE_PAGES 里，运行模式每 5 s 重渲染一次。
       用闭包变量的话，用户刚点完"只看 FAULT"，下一秒就被冲回"全部" ——
       这正是 PREFS 存在的理由（见本文件顶部），原实现偏偏漏掉了这一页。 */
    let level  = PREFS.alarmLevel  || '';
    let source = PREFS.alarmSource || '';
    let offset = PREFS.alarmOffset || 0;
    const LIMIT = 50;

    const listHost = h('div');
    const sumHost  = h('div');
    const srcBar   = h('div', { style: 'display:flex;gap:8px;flex-wrap:wrap;align-items:center' });
    let srcSig = '';     // 来源按钮的指纹；集合与选中项都没变就不重建（否则每 5 s 闪一次）

    const lvlBtns = [];
    const lvlBar = h('div', { style: 'display:flex;gap:8px;flex-wrap:wrap;align-items:center' },
      [['', '全部']].concat(LEVELS.map(l => [l, l])).map(([v, name]) => {
        const b = h('button', { class: `btn-sm ${v === level ? 'primary' : ''}`, text: name,
                                onclick: () => setLevel(v) });
        lvlBtns.push([v, b]);
        return b;
      }));

    // 再点一次已选中的维度 = 取消该维度（回到"全部"），不必非得去点"全部"按钮
    function setLevel(v) {
      level = v; PREFS.alarmLevel = v; offset = 0; PREFS.alarmOffset = 0;
      lvlBtns.forEach(([vv, b]) => b.classList.toggle('primary', vv === v));
      draw();
    }
    function setSource(v) {
      source = v; PREFS.alarmSource = v; offset = 0; PREFS.alarmOffset = 0;
      draw();
    }
    function goPage(delta) {
      offset = Math.max(0, offset + delta);
      PREFS.alarmOffset = offset;
      draw();
    }

    async function draw() {
      const [sum, list] = await Promise.all([
        API.alarmSummary(ctx.scenario, { level, source }),
        API.alarms(ctx.scenario, { level, source, limit: LIMIT, offset }),
      ]);

      /* 来源按钮：计数直接写在按钮上，这些数来自交叉筛选后的 by_source ——
         点进去列表里的条数就是这个数，不会出现"按钮写 270、点进去 12 条"。 */
      const sig = (sum.by_source || []).map(r => `${r.source}:${r.n}`).join('|') + '#' + source;
      if (sig !== srcSig) {
        srcSig = sig;
        clear(srcBar);
        srcBar.appendChild(h('button', {
          class: `btn-sm ${source === '' ? 'primary' : ''}`, text: '全部',
          onclick: () => setSource(''),
        }));
        (sum.by_source || []).forEach(r => {
          const v = r.source || '';
          srcBar.appendChild(h('button', {
            class: `btn-sm ${source === v ? 'primary' : ''}`,
            text: `${v || '—'}（${r.n}）`,
            onclick: () => setSource(v),
          }));
        });
      }

      const sumOf = arr => (arr || []).reduce((a, b) => a + b.n, 0);

      // 按等级：逐行 + 合计行。★ 刻意拆成变量再拼 —— 原来把 map / concat / h('tbody')
      // 全压在一行里，括号层层嵌套，多一个少一个看不出来（2026-10-11 正是因此
      // 出过一个 SyntaxError：整个 pages.js 加载失败 ⇒ 登录按钮没有 JS 接管 ⇒
      // 表单原生提交 ⇒ 页面刷新，表现为「登录进不去」且**没有任何报错**）。
      const lvlRows = (sum.by_level || []).map(r => h('tr', {
        style: 'cursor:pointer',
        onclick: () => setLevel(r.level === level ? '' : r.level),
      }, [
        h('td', null, levelBadge(r.level)),
        h('td', { class: 'num', text: String(r.n) }),
      ]));
      lvlRows.push(h('tr', null, [
        h('td', { class: 'muted', text: '合计' }),
        h('td', { class: 'num', text: String(sumOf(sum.by_level)) }),
      ]));

      const srcRows = (sum.by_source || []).slice(0, 8).map(r => h('tr', {
        style: 'cursor:pointer',
        onclick: () => setSource((r.source || '') === source ? '' : (r.source || '')),
      }, [
        h('td', { text: r.source || '—' }),
        h('td', { class: 'num', text: String(r.n) }),
      ]));

      clear(sumHost);
      sumHost.appendChild(h('div', { class: 'grid g3' }, [
        card('按等级', '点一行即按该等级筛选',
          h('table', { class: 'tbl' }, h('tbody', null, lvlRows))),
        card('按来源', '点一行即按该来源筛选',
          h('table', { class: 'tbl' }, h('tbody', null, srcRows))),
        card('24 h 分布', '按小时 · 已应用当前筛选', h('div', {
          html: barChart({
            labels: (sum.timeline || []).map(r => `${String(r.hour).padStart(2, '0')}`),
            values: (sum.timeline || []).map(r => r.n), height: 170, color: 'var(--warn)',
          }),
        })),
      ]));

      clear(listHost);
      listHost.appendChild(h('div', { class: 'pager' }, [
        h('button', { class: 'btn-sm', text: '上一页', disabled: offset <= 0,
          onclick: () => goPage(-LIMIT) }),
        h('button', { class: 'btn-sm', text: '下一页', disabled: offset + LIMIT >= list.total,
          onclick: () => goPage(LIMIT) }),
        h('span', { class: 'muted', text: `第 ${Math.floor(offset / LIMIT) + 1} 页 / 共 ${list.total} 条` }),
      ]));
      listHost.appendChild(table([
        { title: '时刻', key: 'time_str', mono: true },
        { title: 't_s', render: r => fmt.num(r.ts, 1), num: true, mono: true },
        { title: '等级', render: r => levelBadge(r.level) },
        { title: '来源', key: 'source' },
        { title: '设备', key: 'device_id', mono: true },
        { title: '描述', key: 'description' },
      ], list.items));
    }

    await draw();
    const cur = [level && `等级=${level}`, source && `来源=${source}`].filter(Boolean).join(' · ');
    ctx.setSub(cur ? `已筛选：${cur}` : '等级 × 来源 双维筛选 + 24 h 分布');

    return h('div', { class: 'grid' }, [
      card('筛选', '两行可任意组合（AND）· 再点一次已选项即取消',
        h('div', { class: 'grid' }, [lvlBar, srcBar])),
      sumHost,
      card('告警明细', '与 10/ alarms.csv 一致', listHost),
    ]);
  }

  /* ======================= 5. 策略配置 ======================= */
  async function strategies(ctx) {
    ctx.setSub('04/ 的九个策略，按 L0–L3 收敛成唯一下发指令');
    const canWrite = ['operator', 'admin'].includes(ctx.user && ctx.user.role);
    const d = await API.strategies();
    const host = h('div');
    const PRIO_KIND = { L0: 'b-bad', L1: 'b-warn', L2: 'b-info', L3: 'b-ok' };

    async function refresh() {
      const data = await API.strategies();
      clear(host);
      host.appendChild(table([
        { title: '策略 ID', key: 'strategy_id', mono: true },
        { title: '类名', key: 'strategy_type', mono: true },
        { title: '优先级', render: r => badge(r.priority, PRIO_KIND[r.priority] || 'b-mute') },
        { title: '启用', render: r => badge(r.enabled ? '启用' : '停用', r.enabled ? 'b-ok' : 'b-mute') },
        { title: '说明', key: 'description' },
        { title: '参数', render: r => h('span', { class: 'mono muted', style: 'font-size:11.5px', text: JSON.stringify(r.parameters) }) },
        {
          title: '操作', render: r => {
            const b = h('button', { class: 'btn-sm', text: r.enabled ? '停用' : '启用', disabled: !canWrite });
            b.addEventListener('click', async () => {
              try {
                await API.updateStrategy(r.strategy_id, { enabled: !r.enabled });
                toast(`策略 ${r.strategy_id} 已${r.enabled ? '停用' : '启用'}`, 'ok');
                refresh();
              } catch (e) { toast(e.message, 'bad'); }
            });
            return b;
          },
        },
      ], data.items, { wrap: false }));
    }

    await refresh();

    const notice = canWrite ? null : h('p', { class: 'muted', style: 'font-size:12.5px' ,
      text: `当前角色 ${ctx.user.role} 为只读，无法修改策略（需要 operator 及以上）。` });

    const demo = card('下发指令（演示）', '现场无真实执行器时不会假装成功', h('div', null, [
      h('div', { style: 'display:flex;gap:9px;align-items:center;flex-wrap:wrap' }, [
        h('span', { class: 'muted', text: '目标功率 (kW)' }),
        (() => { const i = h('input', { type: 'number', value: '100', step: '10', style: 'width:110px' });
          i.id = 'cmd-target'; return i; })(),
        (() => {
          const b = h('button', { class: 'btn-sm primary', text: '下发', disabled: !canWrite });
          b.addEventListener('click', async () => {
            try {
              const r = await API.sendCommand({
                target_power_kw: Number($('#cmd-target').value),
                device_id: 'DEV-PCS-01', scenario: ctx.scenario,
              });
              toast(`accepted=${r.accepted}｜executor=${r.executor}｜判定=${r.verdict}`, 'bad');
            } catch (e) { toast(e.message, 'bad'); }
          });
          return b;
        })(),
      ]),
      h('p', { class: 'muted', style: 'font-size:11.5px;margin:11px 0 0' ,
        text: '接口会拿本次指令与本拍权限区间对照给出判定，并把意图写进审计。它不会返回「已下发」。' }),
    ]));

    return h('div', { class: 'grid' }, [
      notice,
      card('策略清单', `共 ${d.count} 条`, host),
      demo,
    ]);
  }

  /* ======================= 6. 收益分析 ======================= */
  async function economics(ctx) {
    ctx.setSub('仿真日经济性核算（含电池衰减成本）');
    const all = await API.economics();
    const norm = all.items.find(x => x.scenario_id === 'normal') || {};
    const fault = all.items.find(x => x.scenario_id === 'fault') || {};
    const energy = await API.energy();

    const cmpRows = [
      ['电量电费 实际', norm.cost_energy_cny, '元'],
      ['电量电费 无储能', norm.cost_total_base_cny, '元'],
      ['需量电费 实际', norm.cost_demand_cny, '元'],
      ['总电费 实际', norm.cost_total_cny, '元'],
      ['总电费 无储能', norm.cost_total_base_cny, '元'],
      ['节省（电量）', norm.saving_energy_cny, '元'],
      ['节省（需量）', norm.saving_demand_cny, '元'],
      ['节省（合计）', norm.saving_total_cny, '元'],
      ['电池衰减成本', norm.cost_degradation_cny, '元'],
      ['净收益', norm.net_benefit_cny, '元'],
      ['节省率', norm.saving_pct, '%'],
      ['关口峰值 实际', norm.peak_grid_kw, 'kW'],
      ['关口峰值 无储能', norm.peak_grid_base_kw, 'kW'],
      ['等效循环', norm.equiv_cycles, '次'],
      ['吞吐电量', norm.throughput_kwh, 'kWh'],
      ['光伏自用', norm.pv_self_use_kwh, 'kWh'],
    ];

    const maxAbs = Math.max(...cmpRows.map(r => Math.abs(r[1] || 0)), 1);

    // ★ 回收期：投资额可配置（原来是写死的 120 万）。输入框改值即时重算，
    //   不落盘（纯前端估算口径，正式值以后端配置为准）。
    let invest = 1200000;
    const paybackHost = h('span');
    const investInput = h('input', {
      type: 'number', value: '120', min: '1', step: '10', style: 'width:90px',
      title: '初始投资（万元）',
    });
    function drawPayback() {
      const wan = Number(investInput.value);
      invest = (Number.isFinite(wan) && wan > 0) ? wan * 10000 : 1200000;
      const years = norm.net_benefit_cny ? invest / (norm.net_benefit_cny * 365) : 0;
      clear(paybackHost);
      paybackHost.appendChild(h('span', { text: fmt.num(years, 2) }));
    }
    investInput.addEventListener('input', drawPayback);
    drawPayback();

    return h('div', { class: 'grid' }, [
      h('div', { class: 'grid g4' }, [
        card(null, null, kpi('净收益', fmt.money(norm.net_benefit_cny, 0), '元/日', '含衰减成本', 'ok')),
        card(null, null, kpi('节省率', fmt.num(norm.saving_pct, 2), '%', '相对无储能基线', 'accent')),
        card('回收期', null, h('div', null, [
          h('div', { class: 'kpi' }, [
            h('div', { class: 'value' }, [paybackHost, h('span', { class: 'unit', text: '年' })]),
            h('div', { class: 'note', text: '按 投资额 ÷ 日净收益 ÷ 365' }),
          ]),
          h('div', { style: 'display:flex;align-items:center;gap:6px;margin-top:8px' }, [
            h('span', { class: 'muted', style: 'font-size:11.5px', text: '投资' }),
            investInput,
            h('span', { class: 'muted', style: 'font-size:11.5px', text: '万元' }),
          ]),
        ])),
        card(null, null, kpi('等效循环', fmt.num(norm.equiv_cycles, 3), '次/日', '寿命成本已计入', null)),
      ]),
      h('div', { class: 'grid g-2-1' }, [
        card('收益构成（相对无储能基线）', null, h('div', null, [
          ratioRow('节省合计', norm.saving_total_cny, maxAbs, 'var(--ok)', ' 元'),
          ratioRow('　其中电量', norm.saving_energy_cny, maxAbs, 'var(--accent)', ' 元'),
          ratioRow('　其中需量', norm.saving_demand_cny, maxAbs, 'var(--info)', ' 元'),
          ratioRow('衰减成本（负向）', norm.cost_degradation_cny, maxAbs, 'var(--bad)', ' 元'),
          ratioRow('净收益', norm.net_benefit_cny, maxAbs, 'var(--ok)', ' 元'),
        ])),
        card('场景对照', '正常日 vs 故障日', table([
          { title: '指标', key: 'k' },
          { title: '正常日', num: true, render: r => fmt.num(r.a, 2) },
          { title: '故障日', num: true, render: r => fmt.num(r.b, 2) },
        ], [
          { k: '净收益 (元)', a: norm.net_benefit_cny, b: fault.net_benefit_cny },
          { k: '总电费 (元)', a: norm.cost_total_cny, b: fault.cost_total_cny },
          { k: '关口峰值 (kW)', a: norm.peak_grid_kw, b: fault.peak_grid_kw },
          { k: '节省率 (%)', a: norm.saving_pct, b: fault.saving_pct },
        ], { wrap: false })),
      ]),
      h('div', { class: 'grid g2' }, [
        card('经济性明细', '10/ summary.json', table([
          { title: '项目', key: '0' },
          { title: '数值', num: true, mono: true, render: r => fmt.num(r[1], 3) },
          { title: '单位', key: '2' },
        ], cmpRows, { wrap: false })),
        card('能量统计', '按日 × 场景', table([
          { title: '日期', key: 'date', mono: true },
          { title: '场景', key: 'scenario_id' },
          { title: '充电', num: true, render: r => fmt.num(r.charge_kwh, 1) },
          { title: '放电', num: true, render: r => fmt.num(r.discharge_kwh, 1) },
          { title: '光伏', num: true, render: r => fmt.num(r.pv_kwh, 0) },
          { title: '负荷', num: true, render: r => fmt.num(r.load_kwh, 0) },
          { title: '关口峰值', num: true, render: r => fmt.num(r.peak_grid_kw, 1) },
          { title: '净收益', num: true, render: r => fmt.num(r.net_benefit_cny, 2) },
        ], energy.items, { wrap: false })),
      ]),
    ]);
  }

  /* ======================= 7. 报表系统 ======================= */
  /* 日报 / 周报 / 月报：一处定义，分段按钮与打印稿抬头共用同一份名字 ——
     两处各写一遍的话，改个名字抬头就跟按钮对不上，纸面稿上就成了第二个真相。 */
  const PERIOD_NAME = { day: '日报', week: '周报', month: '月报' };

  async function report(ctx) {
    ctx.setSub('日 / 周 / 月聚合视角');
    let period = PREFS.report || 'day';
    const host = h('div');
    /* 打印稿抬头：屏幕上 display:none，只在 @media print 里显形（见 app.css）。
       ★ 没有它，纸上看不出这份报表是哪个数据集、什么时候出的 —— 拿出去当不了凭据。 */
    const printHead = h('div', { class: 'print-head' });

    function fillPrintHead(d) {
      clear(printHead);
      const n = new Date();
      const p2 = x => String(x).padStart(2, '0');
      const stamp = `${n.getFullYear()}-${p2(n.getMonth() + 1)}-${p2(n.getDate())} ${p2(n.getHours())}:${p2(n.getMinutes())}`;
      printHead.appendChild(h('h1', { text: `工商业储能 EMS · ${PERIOD_NAME[period]}` }));
      printHead.appendChild(h('div', { class: 'ph-meta' }, [
        h('span', { text: `数据集 ${d.scenario || ctx.scenario || '—'}` }),
        h('span', { text: `聚合视角 ${d.period} · 覆盖 ${d.days} 天` }),
        h('span', { text: ctx.engine === 'run' ? '数据来源 实时采集' : '数据来源 历史库' }),
        h('span', { text: `出具人 ${(ctx.user && ctx.user.username) || '—'}` }),
        h('span', { text: `出具时间 ${stamp}` }),
      ]));
      // data_note 是后端给的"这份数据能代表什么"的诚实边界，必须跟着走纸面，
      // 否则打印出来的报表看着像真实运行数据。
      if (d.data_note) printHead.appendChild(h('p', { class: 'ph-note', text: d.data_note }));
    }

    async function draw() {
      const d = await API.report(ctx.scenario, period);
      clear(host);
      fillPrintHead(d);
      host.appendChild(h('div', { class: 'grid g2' }, [
        card('聚合汇总', `视角 ${d.period} · 覆盖 ${d.days} 天`, table([
          { title: '项目', key: 'k' },
          { title: '数值', num: true, render: r => fmt.num(r.v, 3) },
          { title: '单位', key: 'u' },
        ], [
          { k: '充电量', v: d.aggregate.charge_kwh, u: 'kWh' },
          { k: '放电量', v: d.aggregate.discharge_kwh, u: 'kWh' },
          { k: '光伏发电', v: d.aggregate.pv_kwh, u: 'kWh' },
          { k: '负荷用电', v: d.aggregate.load_kwh, u: 'kWh' },
          { k: '倒送电量', v: d.aggregate.export_kwh, u: 'kWh' },
          { k: '关口峰值', v: d.aggregate.peak_grid_kw, u: 'kW' },
          { k: '净收益', v: d.aggregate.net_benefit_cny, u: '元' },
        ], { wrap: false })),
        card('安全与告警', '报表的"体检结论"', h('div', null, [
          h('div', { class: 'grid g2', style: 'margin-bottom:12px' }, [
            kpi('不变量', d.invariants.all_ok ? '全过' : '未过', '', 'hard / safety / all',
              d.invariants.all_ok ? 'ok' : 'bad'),
            kpi('告警总数', (d.alarms_by_level || []).reduce((a, b) => a + b.n, 0), '条', '', null),
          ]),
          h('div', { style: 'display:flex;gap:7px;flex-wrap:wrap' },
            (d.alarms_by_level || []).map(r => levelBadge(r.level)).concat(
              (d.alarms_by_level || []).map(r => badge(String(r.n), 'b-mute')))),
        ])),
      ]));

      host.appendChild(h('div', { class: 'grid g2', style: 'margin-top:14px' }, [
        card('日明细', null, table([
          { title: '日期', key: 'date', mono: true },
          { title: '场景', key: 'scenario_id' },
          { title: '充 / 放 (kWh)', render: r => `${fmt.num(r.charge_kwh, 1)} / ${fmt.num(r.discharge_kwh, 1)}` },
          { title: '净收益 (元)', num: true, render: r => fmt.num(r.net_benefit_cny, 2) },
        ], d.daily, { wrap: false })),
        card('指令归因分布', '本场景全部指令', table([
          { title: '归因', render: r => resultBadge(r.result) },
          { title: '条数', num: true, render: r => String(r.n) },
          { title: '占比', num: true, render: r => fmt.num(r.n / (d.commands_by_result.reduce((a, b) => a + b.n, 0) || 1) * 100, 2) + ' %' },
        ], d.commands_by_result, { wrap: false })),
      ]));

      host.appendChild(h('p', { class: 'muted', style: 'font-size:12px;margin-top:12px', text: d.data_note }));
    }

    /* 日报 / 周报 / 月报：同 .seg 一套（蓝色滑块 + 白字），
       对位由 app.js 挂载后的 placeSegs() 负责，这里只管点击时滑动。 */
    const seg = h('div', { class: 'seg' });
    Object.keys(PERIOD_NAME).forEach((k) => {
      const n = PERIOD_NAME[k];
      const b = h('button', { class: k === period ? 'active' : '', text: n });
      b.addEventListener('click', () => {
        period = k;
        PREFS.report = k;
        $$('button', seg).forEach(x => x.classList.toggle('active', x === b));
        moveSegInd(seg);
        draw();
      });
      seg.appendChild(b);
    });

    await draw();
    return h('div', { class: 'grid' }, [
      printHead,
      // 这一整行是操作条，纸面上不需要 —— 打印时靠 .no-print 整行隐掉
      h('div', { class: 'no-print', style: 'display:flex;justify-content:space-between;align-items:center' }, [
        seg,
        h('div', { style: 'display:flex;gap:9px' }, [
          h('button', { class: 'btn-sm', text: '导出时序', onclick: () => API.download('timeseries', ctx.scenario) }),
          h('button', { class: 'btn-sm', text: '导出告警', onclick: () => API.download('alarms', ctx.scenario) }),
          h('button', { class: 'btn-sm primary', text: '打印 / 存为 PDF', onclick: () => window.print() }),
        ]),
      ]),
      host,
    ]);
  }

  /* ==================== 装配视图（爆炸图） ====================
     ★ 数据来源，以及"为什么多数部件是灰的"（这不是没做完，是事实）：
       结构来自 `assets/assembly.js` ← `docs/规划/部件测点映射表.csv`
       （4 部位 / 14 部件 / 78 测点，逐行对齐，由 selftest J 段守住）。
       运行时值**只有 5 个部件拿得到** —— `/api/realtime` 仅返回
       DEV-METER-01 / DEV-PV-01 / DEV-LOAD-01 / DEV-PCS-01 / DEV-BMS-01 五台设备。
       其余 9 个部件的点要走 `13/` 的 Modbus 接入才有实时值，
       所以画成灰的并写「无实时源」。★ 把它们涂成"正常"等于谎报现场在传数据 ——
       这正是本项目最忌讳的失效方式（不报错、界面上什么都看不出来）。
     ============================================================ */

  /* 部件的运行时状态。
     cfg   设计上无实时源（dev=null）     → 灰 + 「无实时源」
     live  绑到设备且取到值               → 量测色 + 当前值
     alarm 绑了设备但取不到值（离线/坏值）→ 红 + 「告警」 */
  function partLive(p, devMap) {
    const it = p.dev ? devMap[p.dev] : null;
    if (!p.dev) return { kind: 'cfg', value: null };
    if (!it || !it.quality_ok) return { kind: 'alarm', value: null };
    const v = p.key ? it[p.key] : null;
    if (v == null) return { kind: 'alarm', value: null };
    return { kind: 'live', value: p.mul ? v * p.mul : v };
  }

  function findAsmPart(id) {
    for (const a of ASSEMBLY) {
      const p = a.parts.find(x => x.id === id);
      if (p) return { area: a, part: p };
    }
    return null;
  }

  async function structure(ctx) {
    const rt = await API.realtime(ctx.scenario, ctx.t);
    const devMap = {};
    (rt.items || []).forEach(it => { devMap[it.device_id] = it; });

    const nParts = ASSEMBLY.reduce((n, a) => n + a.parts.length, 0);
    const nPts = ASSEMBLY.reduce((n, a) => n + a.parts.reduce((m, p) => m + p.n, 0), 0);
    const nLive = ASSEMBLY.reduce((n, a) => n + a.parts.filter(p => p.dev).length, 0);
    ctx.setSub(`${nParts} 部件 · ${nPts} 测点 · 其中 ${nLive} 个部件有实时数据源`);

    // 选中的部件要跨重渲染记住：运行模式每 5 s 重渲一次，不记会自己跳回"未选中"
    let active = PREFS.asmPart || null;
    const svgHost = h('div');
    const detailHost = h('div');

    function drawSvg() {
      const areas = ASSEMBLY.map(a => ({
        id: a.id, name: a.name,
        parts: a.parts.map(p => {
          const st = partLive(p, devMap);
          return { id: p.id, name: p.name, n: p.n, state: st.kind,
                   hasLive: st.kind === 'live' && st.value != null,
                   value: st.value, unit: p.unit };
        }),
      }));
      clear(svgHost);
      svgHost.appendChild(h('div', { class: 'flow-wrap', html: assemblyView({ areas, active }) }));
      svgHost.querySelectorAll('.asm-part').forEach(g => {
        g.addEventListener('click', () => {
          active = g.getAttribute('data-pid');
          PREFS.asmPart = active;
          draw();
        });
      });
    }

    function drawDetail() {
      clear(detailHost);
      if (!active) {
        detailHost.appendChild(h('div', { class: 'empty', text: '点左侧任一部件，看它的全部测点' }));
        return;
      }
      const f = findAsmPart(active);
      if (!f) { active = null; PREFS.asmPart = null; drawDetail(); return; }
      const st = partLive(f.part, devMap);
      const stBadge = st.kind === 'live'
        ? badge(`${fmt.num(st.value, 1)} ${f.part.unit || ''}`.trim(), 'b-ok')
        : st.kind === 'alarm' ? badge('取不到值', 'b-bad')
        : badge('无实时源', 'b-mute');

      detailHost.appendChild(h('div', { class: 'grid g3' }, [
        card('当前值', null, h('div', { class: 'kpi-v' }, [stBadge])),
        card('测点数', null, h('div', { class: 'kpi-v', text: `${f.part.n} 个` })),
        card('实时源', null, h('div', { class: 'kpi-v muted mono', text: f.part.dev || '—' })),
      ]));
      detailHost.appendChild(card(
        `${f.area.name} · ${f.part.name}`,
        st.kind === 'cfg'
          ? '测点已在点表里定义，实时值需经 13/ Modbus 接入后才有'
          : `设备侧点名见下表（第 1 层，17/ 全点表）`,
        table([
          { title: '测点（现场口径）', key: 'n' },
          { title: '设备点名', key: 'd', mono: true },
          { title: '单位', key: 'u', mono: true },
        ], f.part.points)));
    }

    function draw() { drawSvg(); drawDetail(); }
    draw();

    return h('div', { class: 'grid' }, [
      card('柜体装配视图', '等轴测展开 · 点部件看测点', svgHost),
      h('div', { class: 'grid' }, [
        card('部件详情', '测点定义来自 部件测点映射表.csv', detailHost),
        card('图例', '三种颜色是有语义的，不是配色', h('div', { class: 'grid g3' }, [
          h('div', { class: 'lgd' }, [h('i', { class: 'sw sw-live' }), h('span', { text: '有实时数据源' })]),
          h('div', { class: 'lgd' }, [h('i', { class: 'sw sw-cfg' }), h('span', { text: '无实时源（需 13/ 接入）' })]),
          h('div', { class: 'lgd' }, [h('i', { class: 'sw sw-bad' }), h('span', { text: '取不到值 / 告警' })]),
        ])),
      ]),
    ]);
  }

  /* ==================== 一次接线图 ====================
     ★ 与「能量流」卡的区别，别做成同一个东西：
       能量流 = 能流示意（四节点定死，回答"电往哪走、走多少"）。
       接线图 = 拓扑事实（节点来自 柜体装配视图.md §2 的 9 条连接，
               回答"柜里有什么、怎么接的"）。节点不是四个，加设备就多一个。
     ★ 辅助舱三部件（消防/空调/门禁）挂在柜外、用虚线 —— 它们不走主电流。
       画进主链等于说"电流流过烟感"，现场会当场指出画错。
     ============================================================ */
  async function wiring(ctx) {
    const [ov, rt] = await Promise.all([
      API.overview(ctx.scenario, ctx.t),
      API.realtime(ctx.scenario, ctx.t),
    ]);
    const devMap = {};
    (rt.items || []).forEach(it => { devMap[it.device_id] = it; });
    const p = ov.power || {};
    const bat = ov.battery || {};

    ctx.setSub('电气连接来自 柜体装配视图.md §2 · 数值为当前功率');

    const NW = 82, NH = 50, ROW = 168;
    const colX = i => 12 + i * (NW + 24);
    const node = (id, name, i, y, opt) => Object.assign(
      { id, name, x: colX(i), y, w: NW, h: NH, live: false, value: null, unit: '' }, opt || {});

    const dOf = (id, key, unit, mul) => {
      const it = devMap[id];
      if (!it || it[key] == null) return { live: false, value: null, unit };
      return { live: true, value: mul ? it[key] * mul : it[key], unit };
    };

    const nodes = [
      node('grid', '电网', 0, ROW),
      Object.assign(node('meter', '关口电表', 1, ROW), dOf('DEV-METER-01', 'power_kw', 'kW')),
      node('tr', '变压器', 2, ROW),
      node('bus', '站内母线', 3, ROW),
      Object.assign(node('sw', '交流断路器', 4, ROW), { live: false }),
      Object.assign(node('conv', 'PCS 变流器', 5, ROW), dOf('DEV-PCS-01', 'power_kw', 'kW')),
      node('contactor', '直流接触器', 6, ROW),
      Object.assign(node('cluster', '电池簇', 7, ROW), dOf('DEV-BMS-01', 'soc', '%', 100)),
      // —— 分支：发电 / 用电 / 辅助（不走主链，虚线框）——
      Object.assign(node('pv', '光伏逆变器', 2, ROW + 92, { branch: true }),
        dOf('DEV-PV-01', 'power_kw', 'kW')),
      Object.assign(node('load', '厂区负荷', 3, ROW + 92, { branch: true }),
        dOf('DEV-LOAD-01', 'power_kw', 'kW')),
      node('fire', '消防主机', 5, ROW - 96, { branch: true }),
      node('hvac', '空调 / 液冷', 6, ROW - 96, { branch: true }),
      node('sec', '门禁 / 安防', 7, ROW - 96, { branch: true }),
    ];

    const kw = v => (v == null ? '' : `${fmt.num(v, 1)} kW`);
    const edges = [
      { from: 'grid', to: 'meter', label: kw(p.grid_kw), active: Math.abs(p.grid_kw || 0) > 0.5,
        color: 'var(--c-grid)' },
      { from: 'meter', to: 'tr',   color: 'var(--c-grid)' },
      { from: 'tr',    to: 'bus',  color: 'var(--c-grid)' },
      { from: 'bus',   to: 'sw',   color: 'var(--c-bat)' },
      { from: 'sw',    to: 'conv', color: 'var(--c-bat)' },
      { from: 'conv',  to: 'contactor', color: 'var(--c-bat)' },
      { from: 'contactor', to: 'cluster', label: kw(p.battery_kw),
        active: Math.abs(p.battery_kw || 0) > 0.5, color: 'var(--c-bat)' },
      { from: 'pv',   to: 'bus', label: kw(p.pv_kw), active: (p.pv_kw || 0) > 0.5,
        color: 'var(--c-pv)' },
      { from: 'bus',  to: 'load', label: kw(p.load_kw), active: (p.load_kw || 0) > 0.5,
        color: 'var(--c-load)' },
      // 辅助：只表示"挂在柜上"，不表示电流路径。
      // ★ aux: true ⇒ ui.js 把它们画成共用一条辅助母线、各自竖直挂上去，
      //   而不是三根折线横穿主链（那会让线从「交流断路器」框里穿出来）。
      { from: 'bus', to: 'fire', aux: true, dashed: true, color: 'var(--text-3)' },
      { from: 'bus', to: 'hvac', aux: true, dashed: true, color: 'var(--text-3)' },
      { from: 'bus', to: 'sec',  aux: true, dashed: true, color: 'var(--text-3)' },
    ];

    const linkRows = [
      ['1 电网 → 关口电表', 'AC', '关口表计量进线功率'],
      ['2 关口电表 → 变压器', 'AC', '站内一次侧'],
      ['3 变压器 → 站内母线', 'AC', '母线为汇流点'],
      ['4 光伏逆变器 → 母线', 'AC', '分支 · 发电'],
      ['5 母线 → 厂区负荷', 'AC', '分支 · 用电'],
      ['6 母线 → 交流断路器', 'AC', '辅助舱 · 配电/开关'],
      ['7 断路器 → PCS 交流侧', 'AC', 'PCS 舱'],
      ['8 PCS 直流侧 → 直流接触器', 'DC', '★ AC/DC 在 PCS 内部，不是一个独立设备'],
      ['9 接触器 → 电池簇', 'DC', '电池舱 · 主正/主负/预充'],
    ];

    return h('div', { class: 'grid' }, [
      /* ★ 图表调用要内联进 html:，不要先算进一个变量再传。
         selftest D 段用静态规则卡"html: 的实参必须是字面量或图表函数调用"，
         写成 `html: svg` 它看不出 svg 里装的是自绘 SVG 还是接口返回的字段 ——
         只有把调用摆在这里，静态检查才认得出来。 */
      card('一次接线图', '实线 = 主电流路径 · 虚线 = 辅助挂载（不导电）',
        h('div', { class: 'flow-wrap', html: wiringDiagram({ nodes, edges }) })),
      h('div', { class: 'grid g2' }, [
        card('连接清单', '9 条，依据 柜体装配视图.md §2',
          table([{ title: '连接', key: '0' }, { title: '性质', key: '1', mono: true },
                 { title: '说明', key: '2' }],
            linkRows.map(r => ({ 0: r[0], 1: r[1], 2: r[2] })))),
        card('当前断面', `SOC ${fmt.num((bat.soc || 0) * 100, 1)} % · 温度 ${fmt.num(bat.temperature_c, 1)} °C`,
          h('div', { class: 'grid g2' }, [
            kpi('关口', fmt.num(p.grid_kw, 1), 'kW', p.grid_kw > 0 ? '从电网取电' : '向电网送电',
              'accent'),
            kpi('光伏', fmt.num(p.pv_kw, 1), 'kW', '', 'ok'),
            kpi('负荷', fmt.num(p.load_kw, 1), 'kW', '', 'warn'),
            kpi('储能', fmt.num(p.battery_kw, 1), 'kW',
              p.battery_kw > 0.5 ? '放电' : p.battery_kw < -0.5 ? '充电' : '保持', 'info'),
          ])),
      ]),
    ]);
  }

  /* ======================= 9. 设备管理 ======================= */

  /* ---- 点表枚举（与 13/src/modbus_point_map.h、14/src/pointmap.py 同一套取值域） ---- */
  const PM_TABLES = ['IR', 'HR', 'DI', 'CO'];
  const PM_ENCODINGS = ['f32', 'u16', 'i16', 'bit'];
  const PM_WORD_ORDERS = ['high', 'low'];
  const PM_WRITABLE = ['ro', 'rw'];

  /* 轻量预检：只挑"一眼能看出错"的几条，给即时反馈。
     ★ 这不是安全边界、也不是最终判据 —— 前端可被绕过，真正的门在
       14/src/api.py（服务端校验）与 13/ 的 validate_map()。这里规则少写
       几条不丢安全性；但反过来，**绝不能**因为前端说"没问题"就跳过服务端。 */
  function pmRowCheck(r, i, names) {
    if (r.name !== names[i]) return `第 ${i + 1} 行点名应为 ${names[i]}`;
    if (!PM_TABLES.includes(r.table)) return '表类型应为 IR/HR/DI/CO';
    if (!PM_ENCODINGS.includes(r.encoding)) return '编码应为 f32/u16/i16/bit';
    if (!PM_WORD_ORDERS.includes(r.word_order)) return '字序应为 high/low';
    const sc = Number(r.scale);
    if (!Number.isFinite(sc)) return '缩放应为数值';
    if (r.encoding === 'bit') {
      if (r.table !== 'DI' && r.table !== 'CO') return '位类型只能落在 DI / CO';
      if (sc !== 0) return '位类型的缩放必须为 0';
    } else if (r.encoding === 'f32') {
      if (r.table !== 'IR' && r.table !== 'HR') return '浮点只能落在 IR / HR';
    } else {
      if (sc <= 0) return `${r.encoding} 的缩放必须大于 0（否则换算除零）`;
      if (r.table !== 'IR' && r.table !== 'HR') return '整数编码只能落在 IR / HR';
    }
    const addr = Number(r.address);
    if (!Number.isInteger(addr) || addr < 0 || addr > 65535) return '地址应为 0..65535 的整数';
    const isCmd = String(names[i] || '').startsWith('CMD.');
    if (isCmd && r.writable !== 'rw') return '指令点必须可写(rw)';
    if (isCmd && r.table !== 'HR') return '指令点必须在 HR 表';
    if (!isCmd && r.writable === 'rw') return '只读段不能标为可写(rw)';
    return '';
  }

  /* 点表 CSV 解析（与 14/src/pointmap.py 的 parse_text 同宽容度：
     剥 BOM / 跳 # 注释 / 跳表头 / 支持引号包字段） */
  function pmSplitCsv(line) {
    const out = []; let cur = ''; let q = false;
    for (let i = 0; i < line.length; i++) {
      const c = line[i];
      if (q) {
        if (c === '"') { if (line[i + 1] === '"') { cur += '"'; i++; } else q = false; }
        else cur += c;
      } else if (c === '"') q = true;
      else if (c === ',') { out.push(cur); cur = ''; }
      else cur += c;
    }
    out.push(cur);
    return out;
  }
  function pmParseCsv(text) {
    const rows = []; let headerSeen = false;
    String(text).split(/\r?\n/).forEach(raw => {
      const s = raw.replace(/^\ufeff/, '');
      if (!s.trim() || s.trim().startsWith('#')) return;
      const cols = pmSplitCsv(s);
      if (!headerSeen) {
        headerSeen = true;
        const first = (cols[0] || '').trim().toUpperCase();
        if (first === 'NAME' || first === 'POINT' || (cols[0] || '').trim() === '点名') return;
      }
      rows.push({
        name: (cols[0] || '').trim(), table: (cols[1] || '').trim(),
        address: (cols[2] || '').trim(), encoding: (cols[3] || '').trim(),
        word_order: (cols[4] || '').trim(), scale: (cols[5] || '').trim(),
        writable: (cols[6] || '').trim(), unit: (cols[7] || '').trim(),
        note: (cols[8] || '').trim(),
      });
    });
    return rows;
  }

  /* 设备接入配置卡：通信参数 + 点表编辑/导入/导出/保存。
     两条「配置通道」的界面前半段 ——
       通道① 点表：界面填表 ──PUT──▶ 14/ 写库 + 原子落盘 <项目根>/config/point-map/active.csv
       通道② 接入参数：同样一步 ──▶ 同一目录的 active.conn（IP/端口/从站号）
       13/ 07/ 现场进程启动时读这两个文件（不改 C++、不重编译、不手敲 IP） */
  /* 浏览器直连下载（CSV 导出走带 token 的直链，不经 fetch ——
     走 fetch 要自己拼 Blob，而且 token 会在 JS 内存里多留一会儿）。 */
  function downloadFile(url, name) {
    const a = document.createElement('a');
    a.href = url; a.download = name;
    document.body.appendChild(a); a.click(); a.remove();
  }

  /* hook: { onId(st.id), onOpen() } —— 编辑器被塞进弹层时用。
     「正在改哪一台」与「弹层开合」都交给弹层，编辑器自己不管。 */
  function pointAccess(devices, ctx, hook) {
    const HK = hook || {};
    const canWrite = ['operator', 'admin'].includes(ctx.user && ctx.user.role);
    const st = { id: (devices[0] || {}).device_id || null, pm: null, rows: [], names: [] };
    const root = h('div');
    const shell = h('div');
    const editorHost = h('div');

    /* ---------------- 编辑器 ---------------- */
    function editor() {
      const thead = h('thead', null, h('tr', null, ['#', '点名（不可改）', '表', '地址',
        '编码', '字序', '缩放', '可写', '预检']
        .map(x => h('th', { text: x }))));
      const tbody = h('tbody');

      st.rows.forEach((r, i) => {
        const tr = h('tr');
        const statTd = h('td');
        const refresh = () => {
          const msg = pmRowCheck(st.rows[i], i, st.names);
          tr.classList.toggle('pm-bad', !!msg);
          clear(statTd);
          statTd.appendChild(msg ? badge(msg, 'b-bad') : badge('ok', 'b-ok'));
        };
        const cell = (node) => { const td = h('td'); td.appendChild(node); return td; };
        const sel = (opts, val, key) => {
          const s = h('select', { class: 'pm-in', disabled: !canWrite });
          opts.forEach(o => s.appendChild(h('option', { value: o, text: o })));
          s.value = val;
          s.addEventListener('change', () => { st.rows[i][key] = s.value; refresh(); });
          return s;
        };
        const num = (val, key, step) => {
          const inp = h('input', { class: 'pm-in', type: 'number', step: step || 'any',
                                   disabled: !canWrite });
          inp.value = val;
          inp.addEventListener('input', () => { st.rows[i][key] = inp.value; refresh(); });
          return inp;
        };

        tr.appendChild(h('td', { class: 'num', text: String(i + 1) }));
        tr.appendChild(h('td', { class: 'mono', text: r.name }));
        tr.appendChild(cell(sel(PM_TABLES, r.table, 'table')));
        tr.appendChild(cell(num(r.address, 'address', 1)));
        tr.appendChild(cell(sel(PM_ENCODINGS, r.encoding, 'encoding')));
        tr.appendChild(cell(sel(PM_WORD_ORDERS, r.word_order, 'word_order')));
        tr.appendChild(cell(num(r.scale, 'scale')));
        tr.appendChild(cell(sel(PM_WRITABLE, r.writable, 'writable')));
        tr.appendChild(statTd);
        refresh();
        tbody.appendChild(tr);
      });

      return h('div', { class: 'tbl-wrap pm-wrap' },
        h('table', { class: 'tbl pm-tbl' }, [thead, tbody]));
    }

    /* ---------------- 工具栏动作 ---------------- */
    async function savePointMap() {
      const bad = st.rows.map((r, i) => pmRowCheck(r, i, st.names));
      const k = bad.findIndex(Boolean);
      if (k >= 0) {
        toast(`预检未通过（${bad.filter(Boolean).length} 处），第一处：${bad[k]}`, 'bad');
        return;
      }
      try {
        const res = await API.savePointMap(st.id, st.rows);
        toast(`已保存 ${res.rows} 行 → ${res.saved.path}（13/ 下次启动即读到此表）`, 'ok');
        await load();
      } catch (e) { toast(e.message, 'bad'); }
    }

    async function saveConn() {
      try {
        const res = await API.saveConn(st.id, {
          source_kind: $('#pm-kind').value,
          host: $('#pm-host').value.trim(),
          sim_host: $('#pm-simhost').value.trim(),
          sim_port: Number($('#pm-simport').value),
          port: Number($('#pm-port').value),
          unit_id: Number($('#pm-unit').value),
          poll_period_ms: Number($('#pm-poll').value),
          timeout_ms: Number($('#pm-timeout').value),
          enabled: $('#pm-enabled').checked,
          note: $('#pm-note').value.trim(),
        });
        st.pm.conn = res.conn;
        const c2 = res.conn || {};
        const t2 = res.target || {};
        // ★ 提示语必须同时说清两件事，否则用户会以为连的还是他在框里填的那台：
        //   ① **实际连谁**（形态是 sim 时连的是模拟器，不是设备 IP）；
        //   ② 落盘了没有、落哪儿了 —— 13/ 07/ 是**从文件**读参数的。
        //   只说"已保存"会让人以为只写进了库，于是仍然手敲 --host，
        //   那正是这个功能要消灭的重复真相。
        const where = c2.source_kind === 'sim'
          ? `模拟源 ${t2.host}:${t2.port}（★ 不是真机）`
          : `真实源 ${t2.host || '未填地址'}:${t2.port}`;
        const cf = res.conn_file || {};
        toast(`接入参数已保存（${where}，从站 ${c2.unit_id}，`
              + `${c2.enabled ? '已启用' : '未启用'}）`
              + (cf.removed
                  ? '；地址已清空，端侧配置文件同时删除'
                  : `${cf.path ? `；已落盘 → ${cf.path}` : ''}（13/ 07/ 启动时读它）`),
              'ok');
      } catch (e) { toast(e.message, 'bad'); }
    }

    /* ---------------- 主渲染 ---------------- */
    async function load() {
      clear(shell);
      clear(root);
      // ★ 「正在改哪一台」由弹层页头说，这里**不重复一份**：屏幕上同时挂两个
      //   「逐台编辑 · DEV-XXX」，人不知道该看哪个，也不知道改的是不是同一个。
      //   这条信息必须有且只有一处 —— 放在弹层页头，因为它跟着弹层一起开关。
      if (HK.onId) HK.onId(st.id);
      // ★ 不再套一层 card()：弹层本身就是一张"页"，页内再放白卡外壳就是
      //   白盒子套白盒子 —— 两道边框看不出哪一层是内容。卡片留给 ①② 那两张。
      shell.appendChild(root);

      /* 新增设备的三件套**必须建在 st.id 判空之前**：库是空的时候，
         「②点表 里点编辑」没有行可点、弹层里又只会停在"请先新增一台" ——
         两条路都进不来。而"库里一台设备都没有"正是新装一台机器的第一天。 */
      const newId = h('input', { class: 'pm-in', type: 'text',
                                 placeholder: '新设备号', style: 'width:130px' });
      const newType = h('select', { class: 'pm-in' }, ['PCS', 'BMS', 'METER', 'PV',
        'TRANSFORMER', 'LOAD', 'GATEWAY'].map(x => h('option', { value: x, text: x })));
      const addBtn = h('button', { class: 'btn-sm primary', text: '新增设备',
                                   disabled: !canWrite });
      addBtn.addEventListener('click', async () => {
        try {
          const r = await API.createDevice({ device_id: newId.value.trim(),
                                            device_type: newType.value });
          toast(`设备 ${r.device.device_id} 已创建`, 'ok');
          devices.push(r.device);
          st.id = r.device.device_id;
          await load();
        } catch (e) { toast(e.message, 'bad'); }
      });
      // 返回节点数组而不是一整条 bar：空库时包进自己的 bar，正常时并进 head 那一条
      const newCtl = () => [h('span', { class: 'muted', text: '新增' }),
                            newId, newType, addBtn];

      if (!st.id) {
        root.appendChild(card('还没有设备', '先新增一台，再配它的接入参数与点表',
          h('div', null, [
            h('div', { class: 'pm-bar' }, newCtl()),
            h('p', { class: 'pm-note', text:
              '设备号自己起（如 DEV-BMS-01），类型决定额定值的默认口径。'
              + '建好之后这里就是这一台的 ① 接入参数 + ② 点表。' }),
          ]), 'pm-card'));
        return;
      }
      let pm;
      try { pm = await API.pointMap(st.id); }
      catch (e) { root.appendChild(h('div', { class: 'empty', text: `读取点表失败：${e.message}` })); return; }
      st.pm = pm;
      // ★ 词表归一：服务端的 writable 是**布尔**（normalize 后的 bool），
      //   而编辑区用的是 CSV 词表 'rw'/'ro'。不归一会同时踩两个坑：
      //     ① <select> 的 value 设成 true 匹配不到任何 option → 显示空白；
      //     ② 预检里 `r.writable !== 'rw'` 恒真 → 三个指令点被**误判**为不可写。
      //   （这两条都是 15/tests/e2e_probe.html 实跑抓出来的，不是推演。）
      st.rows = (pm.rows || []).map(r => Object.assign({}, r, {
        writable: (r.writable === true || r.writable === 1 || r.writable === 'rw') ? 'rw' : 'ro',
      }));
      st.names = st.rows.map(r => r.name);
      const sm = pm.summary || {};

      /* --- 设备选择 + 新增 --- */
      const picker = h('select', { class: 'pm-in' });
      devices.forEach(d => picker.appendChild(
        h('option', { value: d.device_id, text: `${d.device_id}（${d.device_type}）` })));
      picker.value = st.id;
      picker.addEventListener('change', () => { st.id = picker.value; load(); });

      const head = h('div', { class: 'pm-bar' }, [
        h('span', { class: 'muted', text: '设备' }), picker,
        badge(pm.configured ? '已配置点表' : '未配置（回落内置默认表）',
              pm.configured ? 'b-ok' : 'b-warn'),
        pm.configured ? badge(pm.synced ? '盘上文件与库内一致' : '盘上文件与库内不一致',
                              pm.synced ? 'b-ok' : 'b-bad') : null,
        h('span', { class: 'muted', text: `${sm.rows || 0} 点 · 全表扫描 ${sm.requests || 0} 次请求` }),
        h('span', { class: 'pm-grow' }),
        ...newCtl(),
      ]);

      /* --- 通信参数 --- */
      const c = pm.conn || {};
      const field = (label, node) => h('label', { class: 'pm-field' }, [
        h('span', { text: label }), node,
      ]);
      const txt = (id, val, ph, w) => {
        const i = h('input', { class: 'pm-in', type: 'text', placeholder: ph || '',
                               style: `width:${w || 150}px`, disabled: !canWrite });
        i.id = id; i.value = val === null || val === undefined ? '' : val; return i;
      };
      const numf = (id, val, lo, hi) => {
        const i = h('input', { class: 'pm-in', type: 'number', min: lo, max: hi,
                               style: 'width:104px', disabled: !canWrite });
        i.id = id; i.value = val === null || val === undefined ? '' : val; return i;
      };
      const enBox = h('input', { type: 'checkbox', disabled: !canWrite });
      enBox.id = 'pm-enabled'; enBox.checked = !!c.enabled;

      // ★ 对端形态（v1.5）。默认 real —— 没有接入记录的设备就是"真机还没填地址"，
      //   不能因为缺省而显示成"模拟源"（那是凭空造事实）。
      const kindSel = h('select', { class: 'pm-in', disabled: !canWrite });
      kindSel.id = 'pm-kind';
      [['real', '真实源 · 现场设备'], ['sim', '模拟源 · 设备模拟器']]
        .forEach(([v, t]) => kindSel.appendChild(h('option', { value: v, text: t })));
      kindSel.value = c.source_kind === 'sim' ? 'sim' : 'real';
      const simHost = txt('pm-simhost', c.sim_host || '127.0.0.1', '127.0.0.1', 140);
      const simPort = numf('pm-simport', c.sim_port || 1502, 1, 65535);
      // 模拟源的两栏只在 sim 时出现。**设备地址那一栏不隐藏**：
      // 切到模拟源时我们刻意**不清空** host（现场常是"临时接模拟器联调，
      // 之后切回真机"），藏起来会让人以为地址被删了。
      const simFields = [field('模拟源地址', simHost), field('模拟源端口', simPort)];
      const syncKind = () => simFields.forEach(
        x => x.classList.toggle('hidden', kindSel.value !== 'sim'));
      kindSel.addEventListener('change', syncKind);
      syncKind();

      const connCard = card('① 接入参数', 'EMS 作为 Modbus 主站 · 每台一份', h('div', null, [
        h('div', { class: 'pm-fields' }, [
          field('对端形态', kindSel),
          field('设备地址（真实源用）', txt('pm-host', c.host, '192.168.1.20', 170)),
        ]),
        h('div', { class: 'pm-fields' }, [
          simFields[0], simFields[1],
          field('端口', numf('pm-port', c.port, 1, 65535)),
          field('从站号', numf('pm-unit', c.unit_id, 1, 247)),
        ]),
        h('div', { class: 'pm-fields' }, [
          field('轮询周期 (ms)', numf('pm-poll', c.poll_period_ms, 10, 60000)),
          field('超时 (ms)', numf('pm-timeout', c.timeout_ms, 10, 60000)),
          field('启用', enBox),
          // ★ 这一栏必须真的渲染出来：saveConn() 读的是 $('#pm-note')，
          //   缺了它会在点击时抛「Cannot read properties of null」——
          //   而且被 saveConn 自己的 try/catch 吞成一个 toast，很容易漏掉。
          field('备注（给人看，程序不解析）', txt('pm-note', c.note, '如：1#PCS 柜', 220)),
        ]),
        h('div', { class: 'pm-bar' }, [
          (() => {
            const b = h('button', { class: 'btn-sm primary', text: '保存接入参数',
                                    disabled: !canWrite });
            b.addEventListener('click', saveConn); return b;
          })(),
          h('span', { class: 'muted', text: c.updated_at ? `上次更新 ${c.updated_at}` : '尚未配置' }),
          // 落盘状态：端侧（13/ 07/）读的是**文件**，库里有值不等于端侧看得到。
          // 没填地址时不报 warn —— 那本来就是"这台先不接"的正常状态。
          ((c.host || c.source_kind === 'sim')
            ? (pm.conn_file_exists
                ? badge(pm.conn_synced ? '已下发到端侧文件' : '端侧文件与库内不一致',
                        pm.conn_synced ? 'b-ok' : 'b-bad')
                : badge('尚未下发到端侧文件', 'b-warn'))
            : null),
        ]),
        h('p', { class: 'pm-note', html:
          '从站号取值范围 <span class="mono">1..247</span>（0 是广播地址，不能作为正常从站）。'
          + '填了设备地址即视为启用 —— 这是现场直觉，不必再去勾选。' }),
        h('p', { class: 'pm-note', html:
          '<strong>对端形态</strong>：真实源 = 现场设备（用它填的 IP/端口）；'
          + '模拟源 = 设备模拟器（用它自己的地址，默认 '
          + '<span class="mono">127.0.0.1:1502</span>）。'
          + '两种走的是<strong>同一套 Modbus 主站代码</strong>，差别只在连谁 —— '
          + '所以调试期常见的「PCS 接模拟器、电表接真机」不需要两套实现。'
          + '切到模拟源<strong>不会</strong>清空真实地址，切回来不用重填。' }),
        h('p', { class: 'pm-note', html:
          '保存后接入参数会与点表一起原子落盘到 '
          + '<span class="mono">config/point-map/active.conn</span>，'
          + '现场进程启动时直接读它 —— '
          + '<span class="mono">main_field.exe --device modbus</span> 与 '
          + '<span class="mono">modbus_probe.exe</span> 都**不必再手敲 IP**。'
          + '把地址清空会同时删掉该文件，避免端侧拿到一个已作废的地址。' }),
      ]));

      /* --- 点表工具栏 --- */
      const fileInput = h('input', { type: 'file', accept: '.csv,text/csv',
                                     style: 'display:none' });
      fileInput.addEventListener('change', () => {
        const f = fileInput.files && fileInput.files[0];
        if (!f) return;
        const rd = new FileReader();
        rd.onload = () => {
          const parsed = pmParseCsv(rd.result);
          if (parsed.length !== st.names.length) {
            toast(`CSV 有 ${parsed.length} 行，应为 ${st.names.length} 行 —— `
                  + '行序即索引，不能删行或插行', 'bad');
            return;
          }
          st.rows = parsed;
          clear(editorHost);
          editorHost.appendChild(editor());
          toast(`已导入 ${parsed.length} 行到编辑区（尚未保存，点「保存点表」才落盘）`, 'ok');
        };
        rd.readAsText(f, 'utf-8');
        fileInput.value = '';
      });

      const tool = h('div', { class: 'pm-bar' }, [
        (() => {
          const b = h('button', { class: 'btn-sm', text: '下载模板' });
          b.title = '内置默认表，可直接当模板改';
          b.addEventListener('click', () => downloadFile(API.pointMapCsvUrl(st.id, true),
                                                     `point-map_template.csv`));
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '导出当前表' });
          b.addEventListener('click', () => downloadFile(API.pointMapCsvUrl(st.id),
                                                     `point-map_${st.id}.csv`));
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '导入 CSV',
                                  disabled: !canWrite });
          b.addEventListener('click', () => fileInput.click()); return b;
        })(),
        h('span', { class: 'pm-grow' }),
        (() => {
          const b = h('button', { class: 'btn-sm primary', text: '保存点表',
                                  disabled: !canWrite });
          b.addEventListener('click', savePointMap); return b;
        })(),
      ]);

      clear(editorHost);
      editorHost.appendChild(editor());

      root.appendChild(head);
      // 通信参数放整宽横条（字段横向铺开、按需换行），把宽度让给下面的点表编辑器 ——
      // 点表有 9 列，挤在 2/3 宽里每个输入框都要缩到看不清。
      root.appendChild(h('div', { class: 'grid' }, [connCard, card(
        '② 点表', `共 ${st.rows.length} 行 · 每台一份`, h('div', null, [
          tool, fileInput, editorHost,
          h('p', { class: 'pm-note', html:
            '行序即索引：<strong>不能删行、不能插行、不能换序</strong>，'
            + '<span class="mono">name</span> 列是系统点名，只能改它后面的五列。'
            + '指令点 <span class="mono">CMD.P_BAT / CMD.P_UPPER / CMD.P_LOWER</span> '
            + '必须同表且地址连续 —— 它们靠一次 FC16 写 6 个寄存器原子下发，'
            + '拆开会短暂出现「新功率 + 旧权限区间」的中间态。' }),
          h('p', { class: 'pm-note', html:
            `保存路径 <span class="mono">${pm.path}</span>；`
            + `活动表 <span class="mono">${pm.active_path}</span>。`
            + '保存成功即写这两个文件，13/ 现场进程下次启动读活动表 —— '
            + '标「未配置」的设备会回落内置默认表继续跑。' }),
        ]), 'pm-card')]));
      // ★ 这里**不重写**页面副标题：那是 devices() 的职责（两路输入源 · 每端两份）。
      //   原来这行写「台账 + 设备接入（…）」，把上面设好的副标题盖掉了 ——
      //   编辑器是被嵌进这一页的一块，不该替整页说它是干什么的。
    }

    // 点上表里的「编辑」→ 选到那一台并**打开弹层**。
    //   原来是滚到页面末尾的编辑器：滚过去之后人还得在长页面里找它在哪，
    //   而且 32 行点表被挤在页尾，滚动位置一乱就分不清「滚到哪了」。
    // ★ 不自己再实现一遍选中：st.id 只有 pointAccess 一个所有者，
    //   两处各改一遍就会出现"弹层开了但点表还是上一台"。
    async function select(id) {
      if (!id) return;
      st.id = id;
      await load();
      if (HK.onOpen) HK.onOpen();
    }

    return { node: shell, load, select, setDevices: (ds) => { devices = ds; } };
  }

  /* ======================= 9. 设备管理 =======================
     v1.5：从「台账 + 一份 Modbus 参数」改成**两端接入**。

       上端（调度侧）：EMS 是 IEC104 **从站**，等上级调度在 2404 连进来；
       下端（设备侧）：EMS 是 Modbus **主站**，主动去连设备。

     两端都可能是「真实源」，也可能是「模拟源」：
       上端 sim = P3/iec104_master.exe 当模拟调度主站
       下端 sim = 设备模拟器（默认 127.0.0.1:1502）

     ★ 形态不是第二条代码路径：两端各只有一套协议栈，形态只决定"跟谁对接"。
       正因为如此，它必须写在配置里、并且**在界面上看得见** —— 调试期用模拟源
       联调、投运时切真源，中间只要有人忘了改回来，界面看起来一切正常，
       而数据来自一个模拟器。这条记录让"我以为是真机"变成一件查得到的事。
     ★ 平台**不回采**上端连接状态（P3 网关不向平台回报连了几个主站），
       所以上端卡显示的是**配置**，不是"已连接"。这里不点绿灯：
       点一个查不到的灯，比不点更坏。
     ========================================================== */

  function kindBadge(state) {
    if (state === 'real') return badge('真实源', 'b-ok');
    if (state === 'sim') return badge('模拟源', 'b-warn');
    // ★ 「查不到」和「未配置」是**两件事**：前者是平台没给出这个信息
    //   （接口缺失 / 请求失败），后者是查到了、确实没配。都画成「未配置」
    //   等于替平台编了一个结论 —— 本项目在这个坑里栽过（§63 措辞事故）。
    if (state === 'unknown') return badge('未知', 'b-mute');
    // ★ 「已停用」也不能塌进「未配置」：那一行**地址是齐的**，只是这一条
    //   没启用。塌进去之后会跟「目标」列打架（目标显示着一个能连的地址，
    //   形态却说未配置）—— 两列矛盾时，人不知道该信哪一列。
    if (state === 'off') return badge('已停用', 'b-mute');
    return badge('未配置', 'b-mute');
  }

  /* ============ 设备管理页：两大部分 × 每端两份输入 ============
     ★ 为什么是两大部分，而不是一个开关：
       调度主站接入（EMS 作从站）与站内设备接入（EMS 作主站）是**两条独立
       通道**，对端各接各的。「设备已到货接真机、调度链路还没开通」是现场
       最常见的状态；一个开关写两端，等于逼着人把两件不相干的事一起改，
       改完屏幕上只多出几个小标签 —— 谁也不敢动它。
     ★ 一部分的名字用「跟谁对接」来说，不用「上端/下端」：
       那是我们内部的方位词，现场看的是对端是谁。
     ★ 两端形态的组合里只有一种是危险的（下端模拟 → 上端真实：替身算出来的
       数被当现场值上送调度），所以提示由后端算好（`ds.mix`）挂在这两块下面，
       而不是让每个看界面的人自己推。
     ★ v1.7.x 撤掉了每一部分里的「真实输入源 / 模拟输入源」并列卡片：
       它们的内容与 ① 接入参数 是同一份东西，抄一遍只是把同一张表画两次，
       还多出一处需要人自己对齐的"当前生效"标记。现在：
         形态     —— 页尾「模拟输入源」面板（一个面板管两端）+ 每台的形态列
         参数     —— ① 接入参数（真/模各占一列/一栏，两列并存互不覆盖）
       这里只留「按台整片切」的按钮（SRC_KIND 仍用它出按钮文案）。
  */
  const SRC_KIND = {
    real: { name: '真实输入源', hint: '对端是现场的' },
    sim:  { name: '模拟输入源', hint: '对端由本机模拟' },
  };

  const DS_STATE = {
    starting: { t: '启动中', k: 'b-info' },
    running:  { t: '采集中', k: 'b-ok' },
    stopped:  { t: '未启动', k: 'b-mute' },
    stale:    { t: '已停写', k: 'b-warn' },
    failed:   { t: '异常',   k: 'b-bad' },
  };

  function endOf(ds, id) {
    const arr = (ds && ds.ends) || [];
    for (let i = 0; i < arr.length; i += 1) if (arr[i].id === id) return arr[i];
    return null;
  }

  /* ★ sourceBlock() 与 switchSrcBtn() 已随「真实/模拟输入源」两块一起删掉
     （v1.7.x）。留着它们就是两个没人调用的入口 —— 下一个人会以为页面上
     还有"切到这一块"的按钮，然后去找它为什么没显示。 */

  /* 组合提示：level=bad 是"会骗人"的那种，warn 是"还没配全"，
     info 与空是正常态。★ 不在这里重新推一遍逻辑 —— 判据只写在
     api._mix_note 一处，否则界面说的和接口给的两套话迟早分叉。 */
  function mixBanner(mix) {
    if (!mix || !mix.text) return null;
    const cls = mix.level === 'bad' ? 'mix-bad'
      : (mix.level === 'warn' ? 'mix-warn' : 'mix-info');
    return h('div', { class: cls, text: mix.text });
  }

  /* 「这一端要你填两份输入」的显式分节。
     ★ 为什么必须有这个标题：两端各要两份输入（① 接入参数 先连得上 /
       ② 点表·转发表 再读得对），但它们的形态完全不同 —— 上端是
       「全站一份 IOA 转发表」，下端是「每台一份寄存器表 + 每台一份地址」。
       表单散在卡片里靠人自己认，就会出现"我照着图配的，页面上找不到第二份"。
       标题只用一次，不重复解释 —— 解释放在各区里的 pm-note。 */
  function phase(no, title, hint) {
    return h('div', { class: 'phase-hd' }, [
      h('span', { class: 'phase-no', text: no }),
      h('span', { class: 'phase-t', text: title }),
      hint ? h('span', { class: 'phase-hint', text: hint }) : null,
    ]);
  }

  /* ---------------- 一 · 调度主站接入（EMS 作从站） ----------------
     一个从站只有一条链路、一个监听口 —— 所以链路参数只出现一次。
     真实与模拟的差别**不是两套参数**，而是"谁来连、要盯什么"：
     真调度要盯 CA 对不对得上；模拟主站是 P3/ 的 iec104_master。 */
  /* ======================= 「接入调度主站」九步向导（v1.14） =======================

     ★ 为什么把原来那张卡换成向导
       原来「一 · 调度主站接入」把接入参数、IOA 转发表摊在页面上，人看到的是
       两组输入框。现场真正会问的是「还差什么」—— 那不是输入框能回答的。

     ★ 九步里只有第 6 步写文件
       监听口 / CA → active.upstream，转发表 → active.pointmap.csv，网关在
       **下次启动**时才读。其余八步只进草稿（meta.access_draft）。
       所以只有第 6 步标「会改配置」—— 八步都标一次，人反而不看了；
       不标第 6 步，"向导走完"就会被读成"调度能连上了"，而通道和证书根本不在软件里。

     ★ 字段清单不在这里写死
       GET /api/access-draft 连 schema 一起返回；完成校验也只由后端判
       （PUT completed=true，缺项回 400 并说明第几步缺什么）。前端算的那颗
       必填星号只用于提示、不用于放行 —— 放行判据只有一份。
     ========================================================================= */
  function accessWizard(opts) {
    const o = opts || {};
    const canWrite = !!o.canWrite;

    let schema = [], draft = null, prog = null, missing = [];
    let cur = 1, timer = null, pendingUndone = false, dirty = false;
    /* 上次成功保存时用的 current_step。翻页本身不改内容，但"停在哪一页"
       也得存 —— 否则断点续填永远回到保存内容时那一页。 */
    let lastStep = 0;
    let resumeNote = '', stars = [];

    const barHost  = h('div', { class: 'wz-bar' });
    const mainHost = h('div');
    const footHost = h('div', { class: 'wz-foot' });
    const node = h('div', { class: 'wz' }, [barHost, mainHost, footHost]);

    /* Esc 关弹层走的是 modal 自己的监听，不经过「退出」按钮 ——
       刚敲进去、还没到 500ms 防抖窗口的那一下就会丢。
       这里在捕获阶段先 flush：弹层关掉不影响已经发出的 PUT。 */
    node.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') flush();
    }, true);

    const stepAt  = n => schema[Math.max(0, Math.min(schema.length - 1, n - 1))];
    const curStep = () => stepAt(cur);
    const curRow  = () => draft.steps[curStep().key];

    /* 必填星号：这里算一次只为"标出来"，**不用来放行**。
       放行判据在后端（PUT completed=true）。星号算错最多标错一颗，
       不会让不该过的过 —— 这不是"两套算法"，说清楚免得后来人误会。 */
    function depValue(k) {
      const row = curRow();
      if (k in row) return String(row[k] == null ? '' : row[k]);
      for (const s of schema) {
        const r = draft.steps[s.key] || {};
        if (k in r) return String(r[k] == null ? '' : r[k]);
      }
      return '';
    }
    const isReq = f => !!f.req && (!f.dep
      || (f.dep.in || []).map(String).includes(depValue(f.dep.k)));
    const refreshStars = () => stars.forEach(([f, el]) => {
      el.style.display = isReq(f) ? '' : 'none';
    });

    /* ---------- 保存 ----------
       输入即存（防抖 500ms），不设「保存草稿」按钮：多一个按钮就多一个
       忘点的理由，而"忘了点保存就退出"正是这一页最不能出的事。

       ★ 保存回来后**不替换 draft.steps**：输入框的闭包握着当前这一行的
         对象引用，一换引用，之后的输入就写进一个不再被保存的旧对象里。 */
    function scheduleSave() {
      if (!canWrite) return;
      if (timer) clearTimeout(timer);
      timer = setTimeout(() => { flush(); }, 500);
    }

    async function flush() {
      if (timer) { clearTimeout(timer); timer = null; }
      if (!canWrite || !draft) return true;
      if (!dirty && lastStep === cur) return true;
      const body = { steps: draft.steps, current_step: cur };
      if (pendingUndone) body.completed = false;
      try {
        const r = await API.saveAccessDraft(body);
        dirty = false;
        lastStep = cur;
        pendingUndone = false;
        prog = r.progress || prog;
        missing = r.missing || [];
        if (r.draft) {
          draft.updated_at = r.draft.updated_at;
          draft.completed = r.draft.completed;
          draft.completed_at = r.draft.completed_at;
          draft.username = r.draft.username;
        }
        paintBar();
        return true;
      } catch (e) {
        toast('草稿没存上：' + e.message, 'bad');
        return false;
      }
    }

    function touch() {
      if (!canWrite) return;
      dirty = true;
      /* 已经标过"完成"，又改了内容 —— 那条断言就失效了。
         不自动降级的话，界面会一边写着"已完成"、一边躺着没填的格。 */
      if (draft.completed || pendingUndone) {
        draft.completed = false;
        pendingUndone = true;
      }
      refreshStars();
      scheduleSave();
    }

    /* ---------- 字段 ---------- */
    const TYPE_ATTR = { text: 'text', date: 'date', time: 'datetime-local', int: 'number' };

    function fieldNode(f, row) {
      /* ★ 用 div 不用 label：label 包住一组 checkbox 时，点标签文字会切换
         第一个框 —— 那是个很难解释的行为。 */
      const wrap = h('div', { class: 'wz-f' + (f.type === 'textarea' ? ' wide' : '') });
      wrap.dataset.f = f.k;
      const lb = h('span', { class: 'wz-lb' }, [h('span', { text: f.label })]);
      const star = h('i', { class: 'rq', text: '*' });
      lb.appendChild(star);
      stars.push([f, star]);
      wrap.appendChild(lb);

      if (f.type === 'multi') {
        const box = h('div', { class: 'wz-multi' });
        (f.opts || []).forEach(([v, t]) => {
          const cb = h('input', { type: 'checkbox', disabled: !canWrite });
          cb.checked = (row[f.k] || []).map(String).includes(String(v));
          cb.addEventListener('change', () => {
            const set = new Set((row[f.k] || []).map(String));
            if (cb.checked) set.add(String(v)); else set.delete(String(v));
            row[f.k] = Array.from(set);
            touch();
          });
          box.appendChild(h('label', { class: 'wz-cb' }, [cb, h('span', { text: t })]));
        });
        wrap.appendChild(box);
      } else if (f.type === 'select' || f.type === 'yesno') {
        const opts = f.type === 'yesno' ? [['yes', '是'], ['no', '否']] : (f.opts || []);
        const sel = h('select', { class: 'pm-in', disabled: !canWrite });
        /* ★ 空选项必须在：不预填等于不替用户断言。尤其「联调结果」「遥控结果」
           这类 —— 预填一个"通过"，就是系统替人签了字。 */
        sel.appendChild(h('option', { value: '', text: '— 请选择 —' }));
        opts.forEach(([v, t]) => sel.appendChild(
          h('option', { value: String(v), text: t })));
        sel.value = row[f.k] == null ? '' : String(row[f.k]);
        sel.addEventListener('change', () => { row[f.k] = sel.value; touch(); });
        wrap.appendChild(sel);
      } else if (f.type === 'textarea') {
        const ta = h('textarea', { class: 'pm-in', rows: 3, disabled: !canWrite });
        ta.value = row[f.k] || '';
        ta.addEventListener('input', () => { row[f.k] = ta.value; touch(); });
        wrap.appendChild(ta);
      } else {
        const inp = h('input', { class: 'pm-in', type: TYPE_ATTR[f.type] || 'text',
                                  disabled: !canWrite });
        if (f.ph) inp.placeholder = f.ph;
        inp.value = row[f.k] == null ? '' : String(row[f.k]);
        inp.addEventListener('input', () => { row[f.k] = inp.value; touch(); });
        wrap.appendChild(inp);
      }
      if (f.hint) wrap.appendChild(h('span', { class: 'wz-hint', text: f.hint }));
      return wrap;
    }

    /* ---------- 第 6 步的配置区（唯一写文件的地方） ---------- */
    function fld(label, inp, hint) {
      return h('label', { class: 'pm-field' }, [
        h('span', { text: label }), inp,
        hint ? h('span', { class: 'wz-hint', text: hint }) : null,
      ]);
    }

    async function drawLink(host) {
      let up = {};
      try {
        const ds = await API.dataSource();
        up = (ds && ds.upstream) || {};
      } catch (e) {
        clear(host);
        host.appendChild(h('div', { class: 'empty',
          text: '读不到接入参数：' + e.message }));
        return;
      }
      const lk = up.link || {};
      clear(host);

      const bind = h('input', { class: 'pm-in', type: 'text', style: 'width:132px',
                                disabled: !canWrite });
      bind.value = lk.bind_host || '0.0.0.0';
      const port = h('input', { class: 'pm-in', type: 'number', min: 1, max: 65535,
                                style: 'width:104px', disabled: !canWrite });
      port.value = lk.listen_port == null ? 2404 : lk.listen_port;
      const ca = h('input', { class: 'pm-in', type: 'number', min: 1, max: 65534,
                              style: 'width:104px', disabled: !canWrite });
      ca.value = lk.common_addr == null ? 1 : lk.common_addr;
      const en = h('input', { type: 'checkbox', disabled: !canWrite });
      en.checked = lk.enabled !== 0;
      const simHost = h('input', { class: 'pm-in', type: 'text', style: 'width:132px',
                                   disabled: !canWrite });
      simHost.value = lk.master_host || '127.0.0.1';

      const btn = h('button', { class: 'btn-sm primary', text: '保存链路参数',
                                disabled: !canWrite });
      btn.addEventListener('click', async () => {
        try {
          /* 这里不传 kind：形态由页尾面板管。顺手带上可能把形态悄悄改回去。 */
          const r = await API.saveUpstream({
            bind_host: bind.value.trim(),
            listen_port: Number(port.value),
            common_addr: Number(ca.value),
            master_host: simHost.value.trim(),
            enabled: en.checked,
          });
          const L = (r.upstream && r.upstream.link) || {};
          toast(`链路参数已保存（监听 ${L.bind_host}:${L.listen_port}，CA ${L.common_addr}）`
                + `；已落盘 → ${(r.upstream || {}).file || '—'}（网关下次启动生效）`, 'ok');
          if (o.onChanged) o.onChanged();
        } catch (e) { toast(e.message, 'bad'); }
      });

      host.appendChild(h('div', { class: 'pm-fields' }, [
        fld('监听网卡', bind), fld('监听端口', port),
        fld('公共地址 CA', ca), fld('启用', en),
        fld('模拟主站地址（模拟源时生效）', simHost),
      ]));
      const hp = () => port.value || 2404;
      const hb = () => bind.value || '0.0.0.0';
      const ha = () => ca.value || 1;
      host.appendChild(h('p', { class: 'pm-note', html:
        '对端是<strong>上级调度主站</strong>：由它发起连接，本机在监听口等它。'
        + '公共地址 CA 必须与调度的转发表一致 —— 对不上时调度侧的表现是'
        + '「<strong>连上了但收不到点</strong>」，很像是网关坏了。' }));
      host.appendChild(h('p', { class: 'pm-note', html:
        '「模拟主站地址」由页尾「模拟输入源」面板自动填好，也可以在这里手改 —— '
        + '改的是模拟值（真实监听口与 CA 是链路属性，模拟态下不动）。'
        + '模拟主站连进来的命令：<span class="mono">'
        + `P3/build/iec104_master.exe --host ${hb()} --port ${hp()} --ca ${ha()}`
        + '</span>；它跑的是同一套 IEC 104，但上送出去的是'
        + '<strong>本机算的数，不是真调度要的数</strong>。' }));
      host.appendChild(h('div', { class: 'pm-bar', style: 'margin-top:10px' }, [
        btn,
        h('div', { class: 'pm-grow' }),
        (up.file_exists ? badge('已落盘给网关', 'b-ok')
                        : badge('尚未落盘（网关用内置默认 2404 / CA 1）', 'b-warn')),
      ]));
      host.appendChild(h('p', { class: 'pm-note', html:
        '端侧读取点 <span class="mono">'
        + String(up.file || 'config/point-map/active.upstream')
            .replace(/[&<>]/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[ch]))
        + '</span>，由 <span class="mono">' + (up.reader || 'P3/build/iec104_gateway.exe')
        + '</span> 在<strong>启动时</strong>读取。平台保存后不会热更新已跑着的网关 —— '
        + '这是有意的：改监听口意味着这一端要断一次，不能悄悄发生。' }));
      host.appendChild(h('p', { class: 'pm-note', html:
        '<strong>平台不回采上端连接状态</strong>：网关不向平台回报"现在有几个主站连着"，'
        + '所以这里显示的是<strong>配置</strong>。要确认调度真的连上了，'
        + '看网关自己的启动横幅与 <span class="mono">[CONN]</span> 行。' }));
    }

    async function drawPm(host) {
      let pm = null;
      try { pm = await API.upstreamPointMap(); }
      catch (e) { pm = { ok: false, error: e.message }; }
      clear(host);
      if (!pm || pm.ok === false) {
        host.appendChild(h('div', { class: 'empty',
          text: '转发表读不到：' + ((pm && pm.error) || '接口不可用（后端低于 v1.7.x）') }));
        return;
      }
      if (pm.read_error) host.appendChild(h('p', { class: 'pm-err', text: pm.read_error }));
      const c = pm.counts || {};
      const isPlat = pm.source === 'platform';
      host.appendChild(h('div', { class: 'pm-bar' }, [
        badge(`上送 ${c.up || 0} 点`, 'b-info'),
        badge(`下行 ${c.down || 0} 点`, 'b-warn'),
        h('span', { class: 'muted', text: `共 ${c.total || 0} 行` }),
        isPlat ? badge('已保存 · 网关下次启动读这份', 'b-ok')
               : badge('未保存 · 网关用内置默认表', 'b-warn'),
        pm.saved_at ? h('span', { class: 'muted',
          text: `${pm.saved_at}${pm.username ? ' · ' + pm.username : ''}` }) : null,
        h('div', { class: 'pm-grow' }),
        (pm.dup_ioa && pm.dup_ioa.length)
          ? badge(`IOA 重复：${pm.dup_ioa.join(',')}`, 'b-bad') : null,
        (() => {
          const b = h('button', { class: 'btn-sm primary', text: '编辑转发表',
                                  disabled: !canWrite });
          b.addEventListener('click', () => { if (o.onEditPm) o.onEditPm(); });
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '导出当前表' });
          b.addEventListener('click', () => downloadFile(API.upstreamPointMapCsvUrl(),
                                                         'upstream-point-map.csv'));
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '下载模板' });
          b.addEventListener('click', () => downloadFile(API.upstreamPointMapCsvUrl(true),
                                                         'upstream-point-map_template.csv'));
          return b;
        })(),
      ]));
      host.appendChild(table([
        { title: 'IOA', key: 'ioa', mono: true, num: true },
        { title: '方向', render: r => badge(r.direction === 'down' ? '下行' : '上送',
            r.direction === 'down' ? 'b-warn' : 'b-info') },
        { title: '名称', key: 'name' },
        { title: '类型', key: 'kind', mono: true },
        { title: 'EMS 点名', key: 'src', mono: true },
        { title: '系数', key: 'scale', mono: true, num: true },
        { title: '单位', key: 'unit' },
        { title: '说明', key: 'note' },
      ], pm.rows || [], { empty: '转发表是空的' }));
      const fp = String(pm.file || '—').replace(/[&<>]/g,
        ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[ch]));
      host.appendChild(h('p', { class: 'pm-note', html:
        '这份现在是 <span class="mono">' + fp + '</span>'
        + (isPlat
            ? ' —— 它由平台写出，<strong>就是网关下次启动会读的那张表</strong>。'
            : ' —— 这是 P3 随包的默认底本，网关用的是<strong>编译进去的同一张</strong>；'
              + '要改 IOA 就点「编辑转发表」保存一份。')
        + ' 网关的加载顺序是 <span class="mono">--point-map</span> → 约定路径 → '
        + '内置默认表；已经在跑的那个网关<strong>不会热更新</strong>，改完要重启它 —— '
        + 'IOA 一改，调度那边的解码全变了，悄悄换比断了更危险。' }));
    }

    function configPanel() {
      const host1 = h('div', { class: 'pm-note', text: '正在读取接入参数…' });
      const host2 = h('div', { class: 'pm-note', text: '正在读取转发表…' });
      drawLink(host1);
      drawPm(host2);
      /* 返回数组：由 render() 统一往同一个 .wz-cards 里塞，卡片间距只由那一个
         容器控制。套两层 .wz-cards 会让 gap 叠两次，第 6 步的卡间距和别的步不一样。 */
      return [
        card('① 接入参数', '全站唯一一份，网关真正读的就是它', host1),
        card('② IOA 转发表', '默认 24 上送 + 6 下行，每个值装在哪个 IOA、乘多少系数', host2),
      ];
    }

    /* ---------- 步骤条 / 底部按钮 ---------- */
    function paintBar() {
      clear(barHost);
      schema.forEach((s, i) => {
        const p = prog && prog.steps ? prog.steps[i] : null;
        const cls = s.num === cur ? ' cur' : (p && p.ok ? ' done' : '');
        const b = h('button', {
          class: 'wz-pill' + cls,
          text: String(s.num),
          title: `${s.num} · ${s.title}`
                 + (p ? `（必填 ${p.filled}/${p.required}）` : ''),
        });
        b.addEventListener('click', () => { if (s.num !== cur) goto(s.num); });
        barHost.appendChild(b);
        if (i < schema.length - 1) barHost.appendChild(h('span', { class: 'wz-conn' }));
      });
      if (prog) {
        barHost.appendChild(h('span', { class: 'wz-prog',
          text: `必填 ${prog.filled}/${prog.required}` }));
      }
    }

    function paintFoot() {
      clear(footHost);
      const last = cur >= schema.length;
      const st = curStep();          // 导出按钮的文案在 schema 里，见下

      const bExit = h('button', { class: 'btn-sm', text: '退出' });
      bExit.addEventListener('click', async () => {
        await flush();
        if (o.close) o.close();
      });

      const bPrev = h('button', { class: 'btn-sm', text: '上一步',
                                  disabled: cur <= 1 });
      bPrev.addEventListener('click', () => goto(cur - 1));

      /* 最后一页不是「下一步」而是「完成填报」。
         ★ 不写「已完成」：九步里通道、证书、联调、传动都不在软件里，
           按钮写「已完成」等于软件替人宣布接入完成。完成之后弹出的提示
           也把这条边界写明。 */
      const bNext = h('button', { class: 'btn-sm primary',
                                  text: last ? '完成填报' : '下一步' });
      bNext.addEventListener('click', () => {
        if (last) tryComplete();
        else goto(cur + 1);
      });

      const hint = h('span', { class: 'wz-hintline',
        text: last
          ? '「完成填报」要求九步必填全齐；缺项会提示缺在哪一步，页面不动。'
          : '填了就存，可以带着没拿到的资料往下走 —— 但「完成」要必填全齐。' });

      /* ★ 导出按步走（v1.15）：每一份的列顺序由后端 export_step 决定 ——
         第 2 步导的是《远动信息参数表》（逐点 + 变比/满度空列），
         第 3 步是号段对照表，第 8 步是传动记录，第 9 步带九步汇总。
         前端只拿 label，不拼文件名也不拼列。
         九步总台账仍然留着：一页看全貌的场合还是它好用。 */
      const ex = st.export || {};
      const bCsv = h('button', { class: 'btn-sm', text: ex.label || '导出本步' });
      bCsv.addEventListener('click', async () => {
        /* 先 flush：导出的是库里的草稿，不先存就会导出上一版 ——
           那种"导出件少了刚填的两格"最容易被当成软件丢数据。 */
        await flush();
        downloadFile(API.accessDraftCsvUrl(st.num), (ex.name || 'access') + '.csv');
      });
      const bAll = h('button', { class: 'btn-sm', text: '九步总台账',
        title: '九步全部字段 + 必填状态 + 缺项，一页看全貌' });
      bAll.addEventListener('click', async () => {
        await flush();
        downloadFile(API.accessDraftCsvUrl(), 'access-to-dispatch-ledger.csv');
      });

      footHost.appendChild(bExit);
      footHost.appendChild(bCsv);
      footHost.appendChild(bAll);
      footHost.appendChild(h('span', { class: 'wz-grow' }));
      footHost.appendChild(hint);
      footHost.appendChild(bPrev);
      footHost.appendChild(bNext);
      if (!canWrite) { bPrev.disabled = true; bNext.disabled = true; }
    }

    function markMissing() {
      const s = curStep();
      const bad = new Set(missing.filter(m => m.step === s.num).map(m => m.field));
      if (!bad.size) return;
      mainHost.querySelectorAll('.wz-f').forEach(el => {
        if (bad.has(el.dataset.f)) el.classList.add('wz-bad');
      });
      const first = mainHost.querySelector('.wz-bad');
      if (first && first.scrollIntoView) {
        first.scrollIntoView({ block: 'center', behavior: 'smooth' });
      }
    }

    /* ---------- 每步的材料清单 / 建议值 ----------
       ★ 这两块是 v1.15 加的：原来八步只有几个输入框，看着像问卷。
         材料清单回答「这一步要交什么、按什么口径、现在有没有」——
         第三列「现状」里的数字全是后端从库里读的，前端不自己算。

       ★ v1.16 换成整站的 card() + table()：原来这里另起了一套 .wz-mats /
         .wz-tb，表头带灰底、字号 12.5px、外面再套一圈圆角框 —— 而整站所有
         表格都是「卡片里直接放 table.tbl，表头无底色、11px 大写小字距」。
         两套并排放在同一页上，差异一眼就看得出来，这才是"风格不匹配"
         的具体来源（不是配色，是**同一件东西画了两遍**）。 */
    function matsBlock(doc) {
      const m = doc.mats || {};
      const cols = m.cols || [];
      const rows = m.rows || [];
      const tips = doc.tips || [];
      const tcols = cols.map((c, i) => ({
        title: c,
        render: r => r[i],
      }));
      const box = card(
        m.title || '这一步要看的事实',
        `${rows.length} 项` + (tips.length ? ' · 悬停看口径' : ''),
        table(tcols, rows, { wrap: false, empty: '这一步没有要交的东西' })
      );
      if (tips.length) box.title = tips.join('\n');
      return box;
    }

    function assistBlock(list) {
      const s = curStep();
      const nameOf = k => {
        const f = s.fields.find(x => x.k === k);
        return f ? f.label : k;
      };
      const box = h('div', { class: 'wz-assist' });
      const btn = h('button', { class: 'btn-sm', text: '按建议填好',
                                disabled: !canWrite });
      btn.addEventListener('click', () => {
        const r = curRow();
        list.forEach(a => { r[a.k] = a.v; });
        touch();
        render();
        toast(`已按建议填 ${list.length} 项 —— 都可以再改`);
      });
      box.appendChild(h('div', { class: 'wz-assist-lb' }, [
        h('span', { text: list.map(a => `${nameOf(a.k)} = ${a.v}`).join(' · '),
                    title: list.map(a => `${nameOf(a.k)}：${a.why}`).join('\n\n') }),
        btn,
      ]));
      return box;
    }

    function render() {
      const s = curStep();
      const row = curRow();
      stars = [];
      clear(mainHost);

      mainHost.appendChild(h('div', { class: 'wz-head' }, [
        /* 步骤标题上只留编号 + 名字 + 一句短标。
           ★ 2026-10-20 应要求去掉了两段：`s.why`（原来那段灰底解释）与
             「只记台账 / 会改配置」整行。前者挪到标题的 title 上（悬停才出，
             不占版面），后者的实质信息只留第 6 步那一条「会改配置」——
             九步里只有它真的写文件，其余八步不写反而不必逐页声明。 */
        h('h3', { text: `${s.num} · ${s.title}`, title: s.why || '' }),
        s.sub ? h('p', { class: 'wz-sub', text: s.sub }) : null,
        resumeNote ? h('p', { class: 'wz-resume', text: resumeNote }) : null,
        s.config
          ? h('p', { class: 'wz-scope' }, [
              badge('会改配置', 'b-warn'),
              h('span', { text: '这一步写的是真配置：监听口 / CA 落到 active.upstream，'
                + '转发表落到 active.pointmap.csv，网关下次启动才读。' }),
            ])
          : null,
      ]));
      resumeNote = '';   // 只在刚打开时提示一次；翻页不必重复

      /* ★ v1.16 版式统一：每步都是同一个「卡片栈」——
           ① 要看的事实（材料清单，后端给的）
           ② 要动的配置（只有第 6 步有，两张卡）
           ③ 填报（字段网格）
         原来八步只有一张自造的框 + 一堆输入框，第 6 步却是「圆角框里套两张表」，
         两边的卡高、边距、表头都不一边高，所以翻页时版面会"跳"。
         现在间距只由 .wz-cards 一个 gap 控制，翻页时节奏是稳的。 */
      const stack = h('div', { class: 'wz-cards' });

      if (s.doc && s.doc.mats && (s.doc.mats.rows || []).length) {
        stack.appendChild(matsBlock(s.doc));
      }

      if (s.config) configPanel().forEach(c => stack.appendChild(c));

      const grid = h('div', { class: 'wz-grid' });
      s.fields.forEach(f => grid.appendChild(fieldNode(f, row)));
      {
        /* 建议值紧贴在字段网格下面、同一张卡里 —— 它填的就是上面这些格子，
           放在卡片外会变成页脚附近一条浮着的条，人得滚到底才看见。
           只读账号不给这个按钮：点了也存不下去。 */
        const body = h('div', null, [grid]);
        if (canWrite && (s.assist || []).length) {
          body.appendChild(assistBlock(s.assist));
        }
        /* 步骤号 1..9 连续（后端自检守着），所以 num-1 就是 prog.steps 的下标 ——
           与 paintBar() 里的 prog.steps[i] 是同一个位置。 */
        const p = prog && prog.steps ? prog.steps[s.num - 1] : null;
        stack.appendChild(card(
          '填报',
          p ? `必填 ${p.filled}/${p.required}` : `${s.fields.length} 项`,
          body));
      }

      mainHost.appendChild(stack);

      if (!canWrite) {
        mainHost.appendChild(h('p', { class: 'wz-hint',
          text: '当前账号是只读权限：能看，不能改。' }));
      }
      /* ★ 首屏也要收敛一次星号。
         星号是"每个字段先挂一颗、再按 isReq 隐藏"，而函数原来只在 touch()
         里调 —— 没人敲过键的那一屏，`req:false` 与 dep 未激活的格子
         全都挂着假星号（实测「备注」「文件名或编号」都被标成必填）。 */
      refreshStars();
      paintBar();
      paintFoot();
      markMissing();
    }

    async function goto(n) {
      if (n === cur || !schema.length) return;
      /* 先把 cur 挪过去再 flush：flush 记的 current_step 取的是 cur，
         顺序反了就会把"翻页后的位置"存成"翻页前的"。 */
      cur = Math.max(1, Math.min(schema.length, n));
      await flush();
      render();
    }

    /* ---------- 完成 ---------- */
    async function tryComplete() {
      const okSaved = await flush();
      if (!okSaved) return;
      try {
        const r = await API.saveAccessDraft({
          steps: draft.steps, current_step: cur, completed: true,
        });
        draft = r.draft || draft;
        toast('填报完成 —— 九步必填已齐。这只是我方资料填完，'
              + '不代表调度侧已接入（通道、证书、联调、传动仍在软件外面）。', 'ok');
        if (o.onCompleted) o.onCompleted();
        if (o.close) o.close();
      } catch (e) {
        /* 400 时重新取一次精确缺项，跳到第一个缺的步骤并标出来 ——
           只弹一句"还有没填的"，人要自己一页页找，等于没帮上忙。 */
        let list = [];
        try { const r2 = await API.accessDraft(); list = r2.missing || []; } catch (e2) { list = []; }
        if (list.length) {
          missing = list;
          cur = list[0].step;
          render();
          toast(`还有 ${list.length} 项必填没填：第 ${list[0].step} 步`
                + `「${list[0].step_title}」的${list[0].label}`
                + (list.length > 1 ? '…' : '') + '（已跳到那一页）', 'bad');
        } else {
          toast(e.message, 'bad');
        }
      }
    }

    /* ---------- 载入 ---------- */
    async function load() {
      /* 先落地再读：上次改完没到防抖窗口就关了弹层（Esc / 切页），
         这里若直接 GET，读回来的旧草稿会把刚敲的那一下盖掉。 */
      await flush();
      const r = await API.accessDraft();
      schema = r.steps || [];
      draft = r.draft || null;
      prog = r.progress || null;
      missing = r.missing || [];
      dirty = false;
      pendingUndone = false;
      if (!draft || !schema.length) {
        clear(mainHost);
        mainHost.appendChild(h('div', { class: 'empty',
          text: '拿不到九步定义（/api/access-draft）—— 后端版本低于 v1.14。' }));
        return;
      }
      /* ★ 每次打开都按草稿定位，不只第一次。
         否则"关掉弹层再打开"落在哪一页全看内存里的 cur，看起来对、
         理由却不对；而换台电脑/换个浏览器就完全不是这个行为。 */
      const touched = !!draft.updated_at;
      cur = Math.max(1, Math.min(schema.length, Number(draft.current_step) || 1));
      /* 断点续填：**不静默跳**。静默跳会被读成"上次填的丢了"，
         所以要明说"从第几步继续、什么时候填的、谁填的"。 */
      resumeNote = (touched && !draft.completed && cur > 1)
        ? `上次填到第 ${cur} 步（${draft.updated_at}`
          + `${draft.username ? ' · ' + draft.username : ''}），已从这一步继续。`
          + '前面几页没填完也可以往下走，但「完成填报」要求必填全齐。'
        : '';
      lastStep = cur;
      render();
    }

    return { node, load, goto, flush,
             /* 转发表编辑器保存后要刷这一页的摘要 —— 否则第 6 步还显示
                改之前那 30 行，人以为没存上。 */
             refresh: () => { if (draft) render(); },
             get draft() { return draft; } };
  }
  /* ======================= 上端 IOA 转发表编辑器（v1.6） =======================
     ⚠ 与下端点表编辑器（pointAccess）不是一回事，别互相套：
       · 下端  **行序即索引** —— 不能删行 / 插行 / 换序，点名列不可改
       · 上端  IEC104 **靠 IOA 定位** —— 行序没有意义，九列都能改，可任意增删
     两者只有"保存后由平台原子落盘到约定路径"这一点是共同的。

     IOA 区间（调度按段决定怎么解码，落在错段会被当成另一种量读走）：
       1001..1999 遥信   4001..4999 遥测   5001..5999 遥控
       6001..6999 遥调   8001..8999 累计量
     ========================================================================== */
  const UP_KINDS = [
    ['MEAS_FLOAT',      '遥测 · 短浮点（最常用）', '上送', 4],
    ['MEAS_SCALED',     '遥测 · 标度化 int16',     '上送', 4],
    ['MEAS_NORMALIZED', '遥测 · 归一化 -1..1',     '上送', 4],
    ['SINGLE_POINT',    '遥信 · 单点',             '上送', 1],
    ['DOUBLE_POINT',    '遥信 · 双点',             '上送', 1],
    ['COUNTER',         '累计量 · 电度',           '上送', 8],
    ['SET_FLOAT',       '遥调 · 浮点设点',         '下行', 6],
    ['CMD_SINGLE',      '遥控 · 单点命令',         '下行', 5],
    ['CMD_DOUBLE',      '遥控 · 双点命令',         '下行', 5],
  ];
  const UP_KIND_OF = {};
  UP_KINDS.forEach(k => { UP_KIND_OF[k[0]] = { label: k[1], dir: k[2], seg: k[3] }; });
  const UP_SEG_NAME = { 1: '遥信 1xxx', 4: '遥测 4xxx', 5: '遥控 5xxx',
                        6: '遥调 6xxx', 8: '累计量 8xxx' };

  /* 上端转发表 CSV 解析 —— 必须与 14/src/upmapconf.parse 同口径。
     这是**不能各写一套**的一处：前端比后端宽容，就会出现「界面导进来 30 行、
     点保存被拒在第 7 行」；比后端严格，就把后端认得的表挡在门外。
     三条（与后端逐条对应）：剥 UTF-8 BOM；跳过空行与 `#` 注释；首列小写后
     等于 `ioa` 的那行当表头跳过；前 8 个逗号切字段，剩下的整体当 note。
     ★ 与下端点表最大的不同：**行序无意义**（IEC104 按 IOA 定位），所以这里
       既不校验"行数必须相等"，也不按行号去绑点名 —— 那两个约束是下端
       device_point_map「行序即索引」才有的。 */
  function upPmParseCsv(text) {
    const rows = [];
    const errors = [];
    const src = String(text || '').replace(/^\ufeff/, '');
    let filled = 0;          // 补过默认值的行数（下面解释为什么必须补）
    let first = true;
    src.split(/\r?\n/).forEach((raw, k) => {
      const line = raw.trim();
      if (!line || line.startsWith('#')) return;
      const parts = line.split(',');
      const head = (parts[0] || '').replace(/[ \t]/g, '').toLowerCase();
      if (first) {
        first = false;
        if (head === 'ioa') return;
      }
      /* 列数不足**不静默丢**：记下来并告诉人是第几行 —— 静默丢行的后果是
         「导进来 24 行、实际 30 行」，而界面看起来一切正常。 */
      if (parts.length < 9) {
        errors.push({ line: k + 1, msg: `只有 ${parts.length} 列，应为 9 列` });
        return;
      }
      const f = parts.slice(0, 8).concat([parts.slice(8).join(',')]).map(x => x.trim());
      const kind = (f[1] || '').toUpperCase();
      const upSide = !(UP_KIND_OF[kind] && UP_KIND_OF[kind].dir === '下行');
      /* 空列补默认值 —— 这三列在调度那张《远动信息表》里常常是空的
         （尤其「总召分组」是我们这边才有的列，调度表里根本没有），而服务端
         `_as_int` / `_as_float` 对空字符串是**直接报错**、不走默认值。
         不在这儿补，用户看到的就是「导进来 30 行、点保存被拒在第 1 行」。 */
      let did = false;
      if (f[2] === '') { f[2] = upSide ? '1' : '0'; did = true; }   // 分组
      if (f[5] === '') { f[5] = '1'; did = true; }                  // 系数
      if (f[6] === '') { f[6] = '0'; did = true; }                  // 偏置
      if (did) filled++;
      rows.push({
        ioa: f[0], kind, group: f[2], name: f[3],
        src: f[4], scale: f[5], offset: f[6], unit: f[7], note: f[8],
      });
    });
    return { rows, errors, filled };
  }

  function upPmEditor(canWrite, hook) {
    const HK = hook || {};
    const st = { rows: [], saved: null };
    const shell = h('div');
    const editorHost = h('div');
    const msgHost = h('div', { class: 'muted', style: 'font-size:12px;margin-top:8px' });

    /* 一行的"内容签名"，用来判断编辑区跟已保存那份是否真的不同 ——
       导入要覆盖编辑区，只在"有未保存改动"时拦一次，不是每次导入都弹窗。
       ★ 先按签名排序再比：**行序无意义**（IEC104 按 IOA 定位），
         所以只是拖了顺序不算改动，不该让用户确认一次。
       ★ scale/offset 走 Number 归一：CSV 里的 "1.0" 与后端回来的 1
         是同一个值，用原字符串比会永远判成"有改动"。 */
    function pmSig(r) {
      const t = v => String(v === null || v === undefined ? '' : v).trim();
      const n = v => { const s = t(v); return s === '' ? '' : String(Number(s)); };
      return [t(r.ioa), t(r.kind).toUpperCase(), n(r.group), t(r.name),
              t(r.src), n(r.scale), n(r.offset), t(r.unit), t(r.note)].join('|');
    }
    function dirtyPM() {
      const cur = st.rows.map(pmSig).sort().join('\n');
      const sav = (((st.saved || {}).rows) || []).map(pmSig).sort().join('\n');
      return cur !== sav;
    }

    /* 行内预检。**不是**权威判据 —— 真正的校验在服务端（upmapconf.normalize，
       逐条镜像 P3 的 load_point_map_csv + self_check_tables）。这里只是让打字
       过程中就能看见红条，而不是等点「保存」才被拒一次。
       ★ 故意做得比服务端**浅一点**：只查当场能查的（重复、段落、空点名），
       点名是不是存在那件事交给后端 —— 前端不持有一份点名表（那又是一份
       需要同步的东西）。 */
    function rowCheck(i) {
      const rows = st.rows, r = rows[i];
      const meta = UP_KIND_OF[r.kind];
      if (!meta) return 'type 不是九种之一';
      const ioa = Number(r.ioa);
      if (!Number.isInteger(ioa) || ioa <= 0) return 'IOA 应为正整数';
      if (Math.floor(ioa / 1000) !== meta.seg)
        return `IOA ${ioa} 应落在 ${UP_SEG_NAME[meta.seg]}（按 ioa/1000 判段）`;
      if (meta.dir === '上送' && !String(r.src || '').trim())
        return '上送点必须填 EMS 点名';
      const dup = rows.findIndex((x, j) => j !== i && Number(x.ioa) === ioa);
      if (dup >= 0) return `IOA 与第 ${dup + 1} 行重复`;
      return '';
    }

    function editor() {
      const thead = h('thead', null, h('tr', null,
        ['IOA', '类型 (kind)', '方向', '分组', '名称', 'EMS 点名 (src)',
         '系数', '偏置', '单位', '说明', '预检', '']
          .map(x => h('th', { text: x }))));
      const tbody = h('tbody');

      st.rows.forEach((r, i) => {
        const tr = h('tr');
        const statTd = h('td');
        const dirTd = h('td');

        const sync = () => {
          const msg = rowCheck(i);
          tr.classList.toggle('pm-bad', !!msg);
          clear(statTd);
          statTd.appendChild(msg ? badge(msg, 'b-bad') : badge('ok', 'b-ok'));
          const meta = UP_KIND_OF[r.kind] || {};
          clear(dirTd);
          dirTd.appendChild(badge(meta.dir || '—',
                                  meta.dir === '下行' ? 'b-warn' : 'b-info'));
        };

        const cell = (node) => { const td = h('td'); td.appendChild(node); return td; };
        const txt = (val, key, w, ph) => {
          const el = h('input', { class: 'pm-in', type: 'text',
                                  style: `width:${w}px`, disabled: !canWrite,
                                  placeholder: ph || '' });
          el.value = val === null || val === undefined ? '' : val;
          el.addEventListener('input', () => { st.rows[i][key] = el.value; sync(); });
          return el;
        };
        const num2 = (val, key, w, step) => {
          const el = h('input', { class: 'pm-in', type: 'number', step: step || 'any',
                                  style: `width:${w}px`, disabled: !canWrite });
          el.value = val === null || val === undefined ? '' : val;
          el.addEventListener('input', () => {
            st.rows[i][key] = (key === 'ioa' || key === 'group')
              ? el.value : (el.value === '' ? '' : Number(el.value));
            sync();
          });
          return el;
        };
        const kindSel = () => {
          const sel = h('select', { class: 'pm-in', disabled: !canWrite });
          UP_KINDS.forEach(([v, t]) => sel.appendChild(h('option', { value: v, text: t })));
          sel.value = r.kind;
          sel.addEventListener('change', () => { st.rows[i].kind = sel.value; sync(); });
          return sel;
        };
        const delBtn = (() => {
          const b = h('button', { class: 'btn-sm', text: '删', disabled: !canWrite });
          b.title = '删掉这一行（行序无意义，删了不影响别的行）';
          b.addEventListener('click', () => { st.rows.splice(i, 1); draw(); });
          return b;
        })();

        tr.appendChild(cell(num2(r.ioa, 'ioa', 82, 1)));
        tr.appendChild(cell(kindSel()));
        tr.appendChild(dirTd);
        tr.appendChild(cell(num2(r.group, 'group', 60, 1)));
        tr.appendChild(cell(txt(r.name, 'name', 130)));
        tr.appendChild(cell(txt(r.src, 'src', 150, 'MEAS.P_GRID')));
        tr.appendChild(cell(num2(r.scale, 'scale', 74)));
        tr.appendChild(cell(num2(r.offset, 'offset', 74)));
        tr.appendChild(cell(txt(r.unit, 'unit', 62)));
        tr.appendChild(cell(txt(r.note, 'note', 190, '可含逗号')));
        tr.appendChild(statTd);
        tr.appendChild(cell(delBtn));
        sync();
        tbody.appendChild(tr);
      });

      return h('div', { class: 'tbl-wrap pm-wrap' },
        h('table', { class: 'tbl pm-tbl' }, [thead, tbody]));
    }

    function nextIoa() {
      const used = new Set(st.rows.map(r => Number(r.ioa)));
      for (let a = 4100; a < 4999; ++a) if (!used.has(a)) return a;
      return 4999;
    }

    function draw() {
      clear(editorHost);
      editorHost.appendChild(editor());
    }

    async function save() {
      const msgs = st.rows.map((r, i) => rowCheck(i));
      const k = msgs.findIndex(Boolean);
      if (k >= 0) {
        toast(`预检未通过（${msgs.filter(Boolean).length} 处），`
              + `第一处在第 ${k + 1} 行：${msgs[k]}`, 'bad');
        return;
      }
      try {
        const res = await API.saveUpstreamPointMap(st.rows);
        const c = res.counts || {};
        toast(`已保存 ${c.total} 行（上送 ${c.up} / 下行 ${c.down}）→ ${res.path}。`
              + '网关下次启动读它 —— 正在跑的那个不会热更新。', 'ok');
        if (HK.onSaved) HK.onSaved(res);
        await load();
      } catch (e) { toast(e.message, 'bad'); }
    }

    /* 导入 CSV —— 与设备侧（下端点表）那个按钮是同一件事，但**故意有三处不同**：
       ① **不校验行数**：上端行序无意义（按 IOA 定位），多一行少一行都合法；
          下端那句"应为 N 行"在这儿是错的约束，照抄会把人挡在门外。
       ② **只进编辑区、不落盘**：「导入」是编辑动作，不是提交。落盘只走
          「保存转发表」—— 这样导错了还能在表格里改，不必从文件重来。
       ③ **导进来立刻跑一遍行内预检并说出结果**：否则用户会以为导完就好了，
          点保存才被后端拒一次，那时人已经离开这个上下文了。
       覆盖编辑区只在"真有未保存改动"时确认一次（dirtyPM），不是每次都弹。 */
    function importCsv(file) {
      if (!file) return;
      if (dirtyPM() && !window.confirm(
        `编辑区还有未保存的改动（当前 ${st.rows.length} 行）。\n\n`
        + '导入会用文件内容整体替换它，未保存的改动会丢。继续？')) return;
      const rd = new FileReader();
      rd.onload = () => {
        let out;
        try { out = upPmParseCsv(rd.result); }
        catch (e) { toast('解析失败：' + e.message, 'bad'); return; }
        const skipped = out.errors.length;
        if (!out.rows.length) {
          toast(skipped
            ? `一行都没导进来：${skipped} 行列数不对（第 ${out.errors[0].line} 行`
              + `${out.errors[0].msg}）`
            : '这个文件里没有数据行（只有注释或表头？）—— 应为 9 列：'
              + 'ioa,kind,group,name,src,scale,offset,unit,note', 'bad');
          return;
        }
        st.rows = out.rows;
        draw();
        const bad = [];
        st.rows.forEach((r, i) => { const m = rowCheck(i); if (m) bad.push(`第 ${i + 1} 行 ${m}`); });
        const parts = [`已导入 ${st.rows.length} 行到编辑区（尚未保存）`];
        if (skipped) parts.push(`另有 ${skipped} 行列数不对已跳过（第 ${out.errors[0].line} 行起）`);
        if (out.filled) parts.push(`${out.filled} 行的空分组/系数/偏置已按默认填`);
        if (bad.length) parts.push(`${bad.length} 行预检未过 · ${bad[0]}`);
        toast(parts.join(' · '), (bad.length || skipped) ? 'bad' : 'ok');
      };
      rd.readAsText(file, 'utf-8');
    }

    async function reset() {
      if (!window.confirm(
        '删除平台保存的这份转发表？\n\n'
        + '删掉之后网关下次启动用**内置默认表**（上送 24 / 下行 6）。\n'
        + '要留备份请先点「导出当前表」。')) return;
      try {
        const res = await API.resetUpstreamPointMap();
        toast(res.removed ? '已恢复内置默认表（platform 那份已删除）'
                          : '本来就没有平台保存的表', 'ok');
        if (HK.onSaved) HK.onSaved(null);
        await load();
      } catch (e) { toast(e.message, 'bad'); }
    }

    async function load() {
      let pm = null;
      try { pm = await API.upstreamPointMap(); }
      catch (e) {
        clear(shell);
        shell.appendChild(h('div', { class: 'empty',
          text: '转发表读不到：' + e.message }));
        return;
      }
      st.saved = pm;
      st.rows = (pm.rows || []).map(r => ({
        ioa: r.ioa, kind: r.kind, group: Number(r.group) || 0, name: r.name || '',
        src: r.src || '', scale: r.scale === '' || r.scale === undefined ? 1 : Number(r.scale),
        offset: r.offset === '' || r.offset === undefined ? 0 : Number(r.offset),
        unit: r.unit || '', note: r.note || '',
      }));

      clear(shell);
      const fileInput = h('input', { type: 'file', accept: '.csv,text/csv',
                                     style: 'display:none' });
      fileInput.addEventListener('change', () => {
        importCsv(fileInput.files && fileInput.files[0]);
        fileInput.value = '';
      });

      const tool = h('div', { class: 'pm-bar' }, [
        (() => {
          const b = h('button', { class: 'btn-sm', text: '新增一行', disabled: !canWrite });
          b.addEventListener('click', () => {
            st.rows.push({ ioa: nextIoa(), kind: 'MEAS_FLOAT', group: 1, name: '',
                           src: '', scale: 1, offset: 0, unit: '', note: '' });
            draw();
          });
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '导入 CSV', disabled: !canWrite });
          b.title = '从调度那张表（或导出的当前表）读回编辑区 —— 只进编辑区，'
                  + '点「保存转发表」才落盘；行序无意义，不要求行数相等';
          b.addEventListener('click', () => fileInput.click());
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '下载模板' });
          b.title = 'P3 随包的默认表，可照着它改成现场那张';
          b.addEventListener('click', () => downloadFile(
            API.upstreamPointMapCsvUrl(true), 'upstream-point-map_template.csv'));
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '导出当前表' });
          b.addEventListener('click', () => downloadFile(
            API.upstreamPointMapCsvUrl(), 'upstream-point-map.csv'));
          return b;
        })(),
        h('span', { class: 'pm-grow' }),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '恢复默认', disabled: !canWrite });
          b.title = '删掉平台这份，网关回头去用内置默认表';
          b.addEventListener('click', reset);
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm primary', text: '保存转发表',
                                  disabled: !canWrite });
          b.addEventListener('click', save);
          return b;
        })(),
      ]);

      const head = h('div', { class: 'pm-bar' }, [
        pm.source === 'platform'
          ? badge('当前这份是网关下次启动会读的', 'b-ok')
          : badge('当前没有平台表 · 网关用内置默认表', 'b-warn'),
        h('span', { class: 'muted', text: `共 ${st.rows.length} 行` }),
        pm.saved_at ? h('span', { class: 'muted',
          text: `上次保存 ${pm.saved_at}${pm.username ? ' · ' + pm.username : ''}` }) : null,
        canWrite ? null : badge('只读（需 operator）', 'b-mute'),
      ]);

      draw();
      clear(msgHost);
      msgHost.appendChild(h('p', { class: 'pm-note', style: 'margin:0 0 6px', html:
        '保存后原子写到 <span class="mono">' + String(pm.active_path || 'config/point-map/active.pointmap.csv')
          .replace(/[&<>]/g, ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[ch])) +
        '</span>，P3 网关<strong>启动时</strong>读它（先 <span class="mono">--point-map</span>，'
        + '再约定路径，都没有才用内置默认表）。' }));
      msgHost.appendChild(h('p', { class: 'pm-note', style: 'margin:0', html:
        '<strong>行序没有意义</strong> —— IEC104 按 IOA 找点，所以可以任意增删排序；'
        + '这跟下端 <span class="mono">device_point_map</span>「行序即索引」正好相反。'
        + '改 IOA 等于让调度那边的解码全变，所以要<strong>重启网关</strong>才生效，'
        + '平台不会偷偷给它热更新。' }));

      shell.appendChild(h('div', null, [head, tool, fileInput, editorHost, msgHost]));
    }

    return { node: shell, load };
  }

  /* ---------------- 一 · 调度主站接入（EMS 作从站） ----------------
     这张卡现在是**入口**，不再是配置面板：九步向导是唯一入口，点进去才有
     监听口 / CA / 转发表。

     为什么改：原来把「接入参数」和「IOA 转发表」摊在页面上，人看得见
     "要填什么"，看不见"还差什么"。而现场真正会问的是后者 —— 九步里
     有八步不在软件里（通道、证书、联调、传动），输入框回答不了。

     ★ 一个从站只有一条链路、一个监听口 —— 所以链路参数只出现一次，
       在向导第 6 步里，不在这张卡上重复。
     ★ 真实与模拟的差别**不是两套参数**，而是"谁来连、要盯什么"：
       真调度要盯 CA 对不对得上；模拟主站是 P3/ 的 iec104_master。 */
  function accessUpSection(ds, canWrite, redraw, onEditPm, onOpenWizard) {
    const up = ds.upstream || {};
    const lk = up.link || {};
    const end = endOf(ds, 'upstream') || {};

    /* 进度放在卡片右上角：不进向导也能看见"走到第几步、必填多少没填"。
       取不到就明说取不到 —— 别拿一个空白的"0/0"糊过去。 */
    const progHost = h('span', { class: 'muted', style: 'font-size:12.5px',
                                 text: '读取填报进度…' });
    (async () => {
      try {
        const r = await API.accessDraft();
        const p = r.progress || {};
        const d = r.draft || {};
        clear(progHost);
        if (!d.updated_at) {
          progHost.appendChild(badge('尚未开始', 'b-mute'));
          progHost.appendChild(h('span', { class: 'muted',
            text: `必填 ${p.required || 0} 项` }));
        } else if (d.completed) {
          progHost.appendChild(badge('资料填报完成', 'b-ok'));
          progHost.appendChild(h('span', { class: 'muted',
            text: d.completed_at || '' }));
        } else {
          progHost.appendChild(badge(`第 ${d.current_step || 1}/${(r.steps || []).length || 9} 步`,
                                     'b-info'));
          progHost.appendChild(h('span', { class: 'muted',
            text: `必填 ${p.filled || 0}/${p.required || 0} · 未完成` }));
        }
      } catch (e) {
        clear(progHost);
        progHost.appendChild(h('span', { class: 'muted',
          text: '填报进度读不到：' + e.message }));
      }
    })();

    const btnOpen = h('button', { class: 'btn-sm primary', text: '接入调度主站' });
    btnOpen.addEventListener('click', () => { if (onOpenWizard) onOpenWizard(); });

    /* 重置只清「九步填报」，**不动**链路参数与转发表。
       这两件事破坏力完全不同：一个是台账，一个是线上正在生效的配置。
       所以确认框里必须把这句话写出来 —— 不写就会被当成"恢复默认配置"。 */
    const btnReset = h('button', { class: 'btn-sm', text: '重置', disabled: !canWrite });
    btnReset.addEventListener('click', async () => {
      const yes = window.confirm(
        '清空「接入调度主站」九步的填报记录？\n\n'
        + '只清这份台账。链路参数（监听口 / CA）与 IOA 转发表不会被改动 ——\n'
        + '那两样是已经生效的配置，要改请进向导第 6 步。\n\n'
        + '清空后无法恢复，需要重新逐项填写。');
      if (!yes) return;
      try {
        const r = await API.resetAccessDraft();
        toast(r.note || '已清空九步填报记录', 'ok');
        if (redraw) redraw();
      } catch (e) { toast(e.message, 'bad'); }
    });

    const mono = s => h('span', { class: 'mono', text: String(s) });

    return card('一 · 调度主站接入', 'EMS 作从站 · IEC 104 · 等调度连进来',
      h('div', null, [
        h('p', { class: 'pm-note', html:
          '接入这件事分九步走完：<strong>管辖权 → 报送资料 → 号段下发 → '
          + '通信通道 → 安全防护 → 站内配置 → 三级联调 → 遥控遥调传动 → '
          + '报告与投运</strong>。九步里只有「站内配置」会改本机的配置'
          + '（监听口 / CA / 转发表），其余八步是台账 —— 通道、证书、联调、'
          + '传动都发生在软件外面，软件替不了。' }),
        h('div', { class: 'pm-bar', style: 'margin-top:12px' }, [
          btnOpen, btnReset,
          h('div', { class: 'pm-grow' }),
          progHost,
        ]),
        h('p', { class: 'pm-note' }, [
          h('span', { text: '当前生效：监听 ' }),
          mono(`${lk.bind_host || '0.0.0.0'}:${lk.listen_port == null ? 2404 : lk.listen_port}`),
          h('span', { text: ' · CA ' }),
          mono(lk.common_addr == null ? 1 : lk.common_addr),
          h('span', { text: ' · ' }),
          (up.file_exists ? badge('已落盘给网关', 'b-ok')
                          : badge('尚未落盘（网关用内置默认 2404 / CA 1）', 'b-warn')),
          end.registered ? null : badge('库里还没有这条链路', 'b-mute'),
        ]),
      ]), 'pm-card');
  }

  /* ---------------- 二 · 站内设备接入（EMS 作主站） ----------------
     与上端不对称是**真实**的：下端一台设备一份接入参数，形态也**按台记**
     （"PCS 已到货接真机、电表还没来先接模拟器"是调试常态）。
     所以这里给的是「整片切」的快捷方式 + 逐台入口，不是一个整站开关。 */
  // rtPower: { device_id: { power_kw } } —— 当前功率，从 /api/realtime 来。
  function accessDownSection(ds, acc, devices, canWrite, redraw, onEdit, rtPower) {
    const dn = ds.downstream || {};
    // ★ 这里原来还有 `end` / `cur` 两个变量，只服务于已删掉的「真实/模拟
    //   输入源」两块卡片。现在这一端的形态由页尾面板与 ① 表的形态列表达，
    //   函数里不需要自己再判一次 —— 留着就是两个算了没人用的值。
    const sum = (dn.summary && dn.summary.ok) ? dn.summary : {};
    const col = ds.collector || {};
    const running = !!col.running;
    const st = DS_STATE[col.state] || { t: col.state || '—', k: 'b-mute' };
    const cnt = (acc && acc.downstream && acc.downstream.counts) || {};
    const msg = h('div', { class: 'muted',
      style: 'font-size:12px;margin-top:8px;line-height:1.7' });
    const locked = !canWrite || running;

    const ids = (devices || []).map(x => x.device_id);

    const simHost = h('input', { class: 'pm-in', type: 'text', style: 'width:130px',
                                 disabled: !canWrite });
    simHost.id = 'dn-simhost';
    simHost.value = sum.sim_host || '127.0.0.1';
    const simPort = h('input', { class: 'pm-in', type: 'number', min: 1, max: 65535,
                                 style: 'width:104px', disabled: !canWrite });
    simPort.id = 'dn-simport';
    simPort.value = sum.sim_port == null ? 1502 : sum.sim_port;

    const field = (label, node) => h('label', { class: 'pm-field' }, [
      h('span', { text: label }), node,
    ]);

    /* 整片切：改 N 行。部分失败必须照实说 —— 现场会以为整站都切过去了，
       实际有几台还连着另一边。 */
    async function applyAll(kind) {
      if (!ids.length) { toast('还没有设备 —— 在「逐台编辑」弹层里新增一台', 'bad'); return; }
      if (kind === 'sim' && !window.confirm(
        `把 ${ids.length} 台设备全部改为模拟输入源？\n\n`
        + '改完之后站内数据的来源是设备替身，不是现场装置。'
        + '这是"看起来最正常"的一种错，所以再问一次。')) return;
      let ok = 0;
      const fails = [];
      for (const id of ids) {
        try {
          await API.saveConn(id, {
            source_kind: kind,
            sim_host: simHost.value.trim(),
            sim_port: Number(simPort.value),
            enabled: true,
          });
          ok += 1;
        } catch (e) { fails.push(`${id}: ${e.message}`); }
      }
      toast(`已改为${kind === 'sim' ? '模拟' : '真实'}输入源 ${ok}/${ids.length} 台`
            + (fails.length ? `；${fails.length} 台失败（${fails[0]}）` : ''),
            fails.length ? 'bad' : 'ok');
      if (redraw) redraw();
    }

    const mkApply = (kind) => {
      const b = h('button', { class: 'btn-sm', disabled: locked || !ids.length,
        text: '全部切到' + SRC_KIND[kind].name,
        title: `把全部 ${ids.length} 台设备的接入形态整片改为${SRC_KIND[kind].name}` });
      b.addEventListener('click', () => applyAll(kind));
      return b;
    };

    /* ★ v1.7.x：两端都不再有「真实输入源 / 模拟输入源」那种并列卡片。
       理由与上端相同：那两块的内容与 ① 的表格是同一份东西 ——
       每台的目标地址、从站号、轮询周期本来就在表里，"已接入 N 台真实源"
       也只是把表里的形态列数了一遍。

       ★ 留下的是「整片切」按钮：它改的是**每台设备**自己的接入形态，
         与"整端切"不是一回事 —— 调试期"PCS 接真机、电表先接模拟器"是常态，
         页尾那个总开关表达不了这种混接，也不该表达。 */

    /* 采集器只属于这一部分：它是下端的 Modbus 主站，上端跟它没关系。
       放到上端去会让人以为"停采集就断了调度"。 */
    async function doIt(fn, okText) {
      msg.textContent = '';
      try {
        await fn();
        toast(okText, 'ok');
        await redraw();
      } catch (e) {
        msg.textContent = e.message;
        toast(e.message, 'bad');
      }
    }
    const startBtn = h('button', { class: 'btn-sm primary', text: '启动采集',
                                   disabled: !canWrite });
    startBtn.addEventListener('click',
      () => doIt(() => API.liveStart({}), '采集已启动，数据开始逐拍入库'));
    const stopBtn = h('button', { class: 'btn-sm', text: '停止采集',
                                  disabled: !canWrite });
    stopBtn.addEventListener('click',
      () => doIt(() => API.liveStop(), '采集已停止，剩余数据已收尾入库'));
    const resetBtn = h('button', { class: 'btn-sm', text: '清空实时数据',
                                   disabled: !canWrite || running,
                                   title: '清掉已入库的实时数据（不动其它数据集与设备台账）' });
    resetBtn.addEventListener('click', () => {
      if (!window.confirm('清空已入库的全部实时数据？此操作不可撤销。')) return;
      doIt(() => API.liveReset(), '实时数据已清空');
    });

    /* ---------------- ① 每台的接入参数 ----------------
       为什么单列一张表：这部分的两份输入都是**按台**的（一台一份地址参数、
       一台一份点表），而上面的真/模两块说的是"整片切到哪一边"。
       只有表才看得出"6 台里有 1 台配了地址、5 台还没填" —— 而那种状态
       正是现场开机第一天的样子，也最容易让人以为"都接好了"。 */
    const editBtn = (id) => {
      const b = h('button', { class: 'btn-sm', text: '编辑', disabled: !canWrite });
      b.title = `打开「逐台编辑」弹层，改 ${id} 的接入参数与点表`;
      b.addEventListener('click', () => { if (onEdit) onEdit(id); });
      return b;
    };
    const accItems = (acc && acc.downstream && acc.downstream.items) || [];

    /* 台账列：设备号 / 类型 / 接入形态 / 额定功率 / 额定容量 / 状态 / 当前功率 / 说明。
       ★ 这些是**清册**（这台是什么、现在多少功率），与点表（读哪些寄存器）是两件
       事，但合进同一张表：现场判断"这台的点表该不该改"时，要看的就是"这台是什么
       型号、现在有没有功率"。分成两张表，人就只能靠设备号在脑子里对。
       acc 缺失时（老后端没有 /api/access）用设备清册兜底，形态与点数显示「未知」，
       不能降级成「未配置」 —— 那会把"平台没告诉我"说成"确实没配"。 */
    const devById = {};
    (devices || []).forEach(x => { devById[x.device_id] = x; });
    const rt = rtPower || {};
    const devOf = (r) => devById[r.device_id] || {};
    const LEDGER_COLS = [
      { title: '设备号', key: 'device_id', mono: true },
      { title: '类型', render: r => devOf(r).device_type || '' },
      { title: '接入形态', render: r => kindBadge(r.state || 'unknown') },
      { title: '额定功率', num: true,
        render: r => { const v = devOf(r).rated_power_kw;
                       return (v === null || v === undefined) ? '—' : fmt.num(v, 1) + ' kW'; } },
      { title: '额定容量', num: true,
        render: r => { const v = devOf(r).capacity_kwh;
                       return (v === null || v === undefined) ? '—' : fmt.num(v, 1) + ' kWh'; } },
      { title: '状态', render: r => stateBadge((devOf(r).status || '').toUpperCase()) },
      { title: '当前功率', num: true,
        render: r => { const x = rt[r.device_id];
                       return (x && x.power_kw !== null && x.power_kw !== undefined)
                         ? fmt.num(x.power_kw, 2) + ' kW' : '—'; } },
      { title: '说明', render: r => devOf(r).note || '' },
    ];

    const paramTable = acc
      ? table([
        { title: '设备号', key: 'device_id', mono: true },
        { title: '形态', render: r => kindBadge(r.state) },
        { title: '目标（端侧实际连的）', mono: true,
          render: r => (r.target && r.target.error)
            ? badge('参数不完整', 'b-warn')
            : `${(r.target || {}).host || '—'}:${(r.target || {}).port || '—'}` },
        { title: '从站号', num: true, key: 'unit_id' },
        { title: '轮询周期', num: true,
          render: r => r.poll_period_ms == null ? '—' : `${r.poll_period_ms} ms` },
        { title: '动作', render: r => editBtn(r.device_id) },
      ], accItems, { empty: '还没有设备' })
      : h('div', { class: 'empty', text: '接入明细不可用' });

    /* ---------------- ② 每台的点表 ----------------
       点数用 COUNT 现场算，不看 device_conn.point_map_rows 缓存列 ——
       缓存列一旦和真表分叉，界面就会说"已配 32 点"而库里一行都没有。 */
    // acc 缺失时用设备清册兜底：点数与形态显示「未知」，不能降级成「未配置」。
    const pmRows = accItems.length ? accItems
      : (devices || []).map(x => Object.assign({}, x, { state: 'unknown' }));
    const pmTable = table(LEDGER_COLS.concat([
      { title: '点数', num: true,
        render: r => (r.point_rows === null || r.point_rows === undefined
          ? '—' : (r.point_rows ? `${r.point_rows} 点` : '0 点')) },
      { title: '点表状态',
        render: r => (r.point_rows === null || r.point_rows === undefined
          ? badge('未知（/api/access 不可用）', 'b-mute')
          : (r.point_rows ? badge('已配置（库内有表）', 'b-ok')
                          : badge('未配置 · 回落内置默认表', 'b-warn'))) },
      { title: '动作', render: r => editBtn(r.device_id) },
    ]), pmRows, { empty: '还没有设备 —— 在「逐台编辑」弹层里新增一台' });

    return card('二 · 站内设备接入', 'EMS 作主站 · Modbus TCP · 主动连设备',
      h('div', null, [
        h('div', { class: 'pm-bar' }, [
          badge(st.t, st.k),
          h('span', { class: 'muted', text: `已入库 ${col.rows || 0} 行` }),
          // ★ 采集器名字只在"跑过"之后才有意义：没跑过时它是 SOURCES 的
          //   兜底项（本机模型 · 降级），写出来等于报告一个没发生过的降级。
          (running || col.rows > 0) && col.name
            ? h('span', { class: 'muted', text: col.name }) : null,
          col.started_at ? h('span', { class: 'muted', text: `启动于 ${col.started_at}` })
            : null,
          h('div', { class: 'pm-grow' }),
          running ? stopBtn : startBtn,
          resetBtn,
          canWrite ? null : h('span', { class: 'muted', style: 'font-size:12px',
                                        text: '只读角色' }),
        ]),
        running
          ? h('p', { class: 'pm-note', style: 'margin-top:0', html:
              '采集中不许切这一端的输入源 —— 那等于把采集器脚下的对端换掉，'
              + '而它还在按旧参数读写。要换先停。' })
          : null,
        // ★ 采集告警原先挂在页面顶部的状态条上（v1.9 已整条删除）。
        //   它不是被删掉了，而是挪到这里：启停就在这一页，异常也该在同一处 ——
        //   否则采集进程退出后，别的页面上只剩"数字不再变化"，没人知道是为什么。
        col.error ? h('p', { class: 'pm-err', text: `采集告警：${col.error}` }) : null,
        phase('①', '接入参数',
          `每台一份 · 共 ${accItems.length} 份 · IP / 端口 / 从站号 / 节奏`),
        paramTable,
        h('p', { class: 'pm-note', html:
          '「目标」是端侧<strong>实际会去连</strong>的地址：真实源用设备地址、'
          + '模拟源用替身地址 —— 不是把两栏都列一遍。'
          + `共 ${accItems.length} 台，改哪一台点哪一行的「编辑」（打开弹层）。` }),
        acc ? null : h('p', { class: 'pm-note', text:
          '接入明细接口不可用（/api/access）—— 每台的目标与点数看不到。' }),
        // 整片切：按设备逐台记形态，切完仍可单台改。
        h('div', { class: 'pm-bar', style: 'margin-top:12px' }, [
          h('span', { class: 'muted', style: 'font-size:12px',
                      text: '整片切（按设备逐台记，切完仍可单台改）' }),
          mkApply('real'), mkApply('sim'),
          h('div', { class: 'pm-grow' }),
        ]),
        h('div', { class: 'pm-fields' }, [
          field('替身地址（模拟源时生效）', simHost),
          field('替身端口（模拟源时生效）', simPort),
        ]),
        /* ★ v1.11：这两栏下面原本还有两段常驻说明（"模拟态下会填进 ①"、
           "模拟源下页面上的数字来自设备替身"）。它们说的都是**模拟源**的事，
           按"模拟源的表达全部收进页尾那张卡"的要求撤掉了：
           怎么填的、按哪一套填，看页尾面板的「端口计划」；
           "这是替身算出来的数、不能当运行记录"并进「启用模拟源」的 title。 */
        msg,
        h('div', { class: 'pm-bar', style: 'margin-top:12px' }, [
          h('div', { class: 'pm-grow' }),
          (dn.file_exists
            ? badge('已落盘给端侧', 'b-ok')
            : badge('尚未落盘（端侧回落内置默认表）', 'b-warn')),
          h('span', { class: 'muted', text:
            `未接入 ${cnt.unset || 0} 台` }),
          // 「已停用」要单独报出来：它是地址齐了但这一条不生效，与"没配过"
          // 的处置完全不同（前者去勾掉启用，后者去填地址）。
          (cnt.off ? h('span', { class: 'muted', text: `已停用 ${cnt.off} 台` })
                   : null),
        ]),
        h('p', { class: 'pm-note', html:
          '形态<strong>按设备逐台记</strong>（在下面 ②点表 里点「编辑」逐台改）：'
          + '调试期"PCS 接真机、电表先接模拟器"是常态，整站一个开关表达不了。'
          + '上面两个按钮只是整片切，切完仍可单台调整。' }),
        h('p', { class: 'pm-note', html:
          '端侧读取点 <span class="mono">'
          + String(dn.file || 'config/point-map/active.conn').replace(/[&<>]/g,
              ch => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[ch]))
          + '</span>，由 <span class="mono">' + (dn.reader || '07/build/main_field.exe')
          + '</span> 在<strong>启动时</strong>读取。' }),
        phase('②', '点表',
          '每台一份 · 同时是设备清册 · 寄存器 ↔ 点名 · 字序是最容易踩的坑'),
        pmTable,
        // 空库时这张表没有行可点 ⇒ 得在这里留一个入口，否则"新建第一台设备"
        // 这件事只能靠改库。弹层里本来就有新增三件套（见 pointAccess 的 load）。
        pmRows.length ? null : h('div', { class: 'pm-bar', style: 'margin-top:10px' }, [
          (() => {
            const b = h('button', { class: 'btn-sm primary', text: '新增第一台设备',
                                    disabled: !canWrite });
            b.title = '打开「逐台编辑」弹层，在里面新增设备';
            b.addEventListener('click', () => { if (onEdit) onEdit(null); });
            return b;
          })(),
          h('span', { class: 'muted', text:
            '库里还没有设备 —— 这一页没有行可点，新增入口在弹层里' }),
        ]),
        h('p', { class: 'pm-note', html:
          '前八列是<strong>清册</strong>（这台什么型号、额定多少、现在多少功率），'
          + '后面才是<strong>点表</strong>（读哪些寄存器）。合在一行，'
          + '是因为判断"这台的点表该不该改"要先看"这台是什么、有没有在采" —— '
          + '拆成两张表，人就只能靠设备号自己在脑子里对。' }),
        h('p', { class: 'pm-note', html:
          '「接入形态」是<strong>每台设备</strong>自己的接入源，与上面两部分的'
          + '当前形态可能不一致 —— 不一致时以每台自己的为准（采集器按设备逐台连）。' }),
        h('p', { class: 'pm-note', html:
          '表本身的列（表 / 地址 / 编码 / <strong>字序</strong> / 缩放 / 可写）'
          + '不在这一页铺开：9 列 × 32 行会把整页撑成一条缝。'
          + '点「编辑」打开<strong>跟页面一样大</strong>的「逐台编辑」弹层去改，'
          + '改完关掉就回到这一行。行序即索引，不能删行或换序。' }),
      ]), 'pm-card');
  }

  /* ---------------- 模拟输入源（页尾总开关，v1.7） ----------------
     ★ 为什么移到页尾、并且只有**一个**面板：
       模拟源是**整页的一个状态**，不是"上端的一个选项 + 下端的一个选项"。
       放在两端各自那块里，人会以为可以只开一端 —— 而「下端模拟 + 上端真实」
       正是最危险的一种组合：上送调度的是替身算出来的数，屏幕上却一切正常
       （后端 `_mix_note()` 就是为这个组合亮红灯的）。

     ★ 三件事：
       启用   —— 两端一起切到模拟源，并把①接入参数按「一套」填好，拉起整条链路。
       换一套 —— 端口段整体换一档（1502 → 1602 → 1702 → 1802 → 循环）。
       关闭   —— 两端一起切回真实源，并把链路收掉。

     ★ v1.11：模拟源的**一切表达都收在这张卡里** —— 状态徽章、套名、端口
       计划表、进程表、告警，都在这儿。上面的表（上端 ①② / 下端 ①②）不再
       换配色，正文里也不再铺大段说明：那些话该由按钮的 title 承担，
       想在页面上常驻的只有"现在是什么状态"。

     ★ 这一页唯一不能破的承诺：**真实值一列都不动**。
       下端 host / port、上端 listen_port / CA 是现场配置的，联调完关掉
       模拟源还要用；模拟只写 sim_host / sim_port 与 master_host 三列。
       这句话写在「启用/关闭」按钮的 title 上 —— 用户最担心的就是
       "点了启用，我填的地址没了"。 */
  function simSourcePanel(canWrite, redraw) {
    const host = h('div');
    /* ★ 动态内容与常驻节点分开（2026-10-19）：
       `err` 要跨重建活下来 —— 请求回来时先画面板再写告警，若重建把 err
       一起清掉，那句话就写进了一个已经不在文档里的节点（屏幕上什么都没有）。 */
    const bodyHost = h('div');
    const err = h('div', { class: 'pm-err' });
    host.appendChild(bodyHost);
    /* ★ 在飞期间不许再点（2026-10-19 体检发现）：
       拉起整条链路要 5~10 s，这段时间按钮一直是可点的。用户看没反应就会连点，
       实测有一次「启用模拟源」在同一秒落了 **5 条一模一样的审计**、写了 5 遍配置
       （下拉链路的锁让后 4 次秒返回，所以几乎同时结束）。
       后果不算致命（start 是幂等的），但审计里出现 5 条相同记录，
       看日志的人会以为点了 5 次不同的事 —— 这正是"操作记录不可信"的开端。 */
    let busy = false;
    let btns = [];
    let progEl = null;       // 「进行中」条：act 期间插在面板最上面
    let progTimer = null;

    /* ★ 点击后必须**立刻**看到"点到了"（2026-10-19 现场反馈）。
       原来只有 `disabled = true` —— 灰按钮不构成反馈，而拉起整条链路要
       5~10 s（实时库 → 替身 ×N → 采集器 → 网关 → 主站），这段时间页面
       一动不动，用户的原话是「点了没反应」。
       所以两件事一起做：按钮文字换成"正在…"，面板顶上挂一条带秒表的进度条。 */

    /* 每个动作在按钮上的说法。想弄清"到底在等什么"，悬停进度条有顺序。 */
    function actText(body, useProc) {
      if (useProc) {
        if (body.action === 'restart') return '按当前这一套重启链路';
        if (body.action === 'stop')    return '停止模拟链路';
        return '启动模拟链路';
      }
      if (body.rotate) return '切换到下一套模拟数据';
      return body.enable ? '启用模拟源并拉起链路' : '关闭模拟源并收掉链路';
    }

    /* 进度条里那句"在等什么"：把五段顺序写出来，用户才知道还要等几步。 */
    const STEP_HINT = '实时库 → 设备替身 → 采集器 → 上端网关 → 模拟主站';

    /* 后台 job 的轮询节奏（§92b）。后端现在是"收下就算"，要自己盯着。
       ★ 700 ms 是量出来的取舍：拉链路约 7 s，太快是白刷接口，太慢则在
         "已经好了"和"界面知道好了"之间留下肉眼可见的延迟。
       ★ 90 s 上限：真卡住时不许无限轮询 —— 到点就如实说"还在办"，
         而不是一直转下去让用户以为有希望。 */
    const JOB_GAP_MS = 700;
    const JOB_WAIT_MS = 90000;
    const JOB_GAP = () => new Promise(r => setTimeout(r, JOB_GAP_MS));

    function progShow(msg) {
      // 每次 render() 都会把面板重建一遍，进度条随之离开文档 —— 必须重造并
      // **重新插入**，不能只改节点内容（那样改的是一个不在文档里的 div，
      // 屏幕上什么都没有，点击依旧"没反应"）。
      if (!progEl) {
        progEl = h('div', { class: 'sim-prog' }, [
          h('span', { class: 'sim-spin' }),
          h('span', { class: 'sim-t' }),
          h('span', { class: 'sim-sec' }),
        ]);
        progEl.title = '按顺序拉起：' + STEP_HINT;
      }
      if (!progEl.isConnected) host.insertBefore(progEl, host.firstChild);
      progEl.querySelector('.sim-t').textContent = msg + '（' + STEP_HINT + '）';
      progEl.hidden = false;
      const t0 = Date.now();
      const tick = () => {
        const sec = (Date.now() - t0) / 1000;
        progEl.querySelector('.sim-sec').textContent =
          `已 ${sec < 10 ? sec.toFixed(1) : Math.round(sec)} 秒 · 通常 5~10 秒`;
      };
      tick();
      clearInterval(progTimer);
      progTimer = setInterval(tick, 200);
      // 面板在页尾：点完如果已经滚出视口，把它滚回来（只滚到"能看到按钮"）。
      if (progEl.getBoundingClientRect().top < 0) {
        progEl.scrollIntoView({ block: 'center', behavior: 'smooth' });
      }
    }

    function progHide() {
      clearInterval(progTimer);
      progTimer = null;
      if (progEl) progEl.hidden = true;
    }

    async function act(body, useProc) {
      if (busy) return;                 // 连点直接吞掉，不发请求
      busy = true;
      err.textContent = '';
      const msg = actText(body, useProc);
      /* 按钮文字改成"正在…"：这是唯一一个**用户视线本来就在**的位置。
         注意只改 toggle 那一颗 —— 另外两颗的文字是名词（换一套 / 换一个进程），
         改成"正在…"会读不通。 */
      btns.forEach(b => {
        if (b.dataset.role === 'toggle') b.textContent = msg + '…';
        b.disabled = true;
      });
      progShow(msg);
      try {
        // ★ 两条通道分工：
        //   /api/sim-source       改**表**（形态 / 端口段）
        //   /api/sim-source/proc  改**进程**（谁在跑）
        //   混成一个按钮就会再次出现"点了启用，表变了、进程没变"。
        let r = useProc ? await API.simProc(body) : await API.setSimSource(body);
        /* ★ 后端现在是"收下就算"（HTTP 202，§92b）：请求几十毫秒就回来了，
           但链路还在后台拉起。所以**再跟一段**：轮询到 job 自己说"完了"为止。
           直接用第一次的返回画面板会出现"进度条刚收、下面写着还在拉起"。
           ★ 轮询失败不算失败 —— 后端可能正忙（在写库 / 起进程），
             一次 GET 没回应不代表这件事黄了，所以只保留上一次的结果继续等。 */
        const jev = v => v && v.chain && v.chain.job;
        if (jev(r) && jev(r).running) {
          const deadline = Date.now() + JOB_WAIT_MS;
          while (jev(r) && jev(r).running && Date.now() < deadline) {
            await JOB_GAP();
            try {
              const nx = await API.simSource();
              if (nx) r = nx;
            } catch (e) { /* 保持上一次 */ }
            // 让状态行跟着走：链路一台台起来时用户看得到数字在变。
            SIM_VIEW = r;
            render(r, '');
          }
          const j = jev(r);
          if (j && j.running) {
            /* 超时还没完：不要假装成功，也不要假装失败 —— 如实说"还在办"，
               并把进度条留着（它会继续走秒）。用户可以自己再刷面板。 */
            SIM_VIEW = r;
            render(r, '链路操作还没结束（已等 '
              + Math.round(JOB_WAIT_MS / 1000) + ' 秒）—— 它还在后台跑，'
              + '刷新页面可以看最新状态。');
            progHide();
            toast('还在继续拉起链路，稍后看面板状态', 'warn');
            busy = false;
            btns.forEach(b => { b.disabled = !canWrite; });
            return;
          }
        }
        SIM_VIEW = r;
        /* ★ 用**这一次的返回**画面板，不再回身发一次 GET：
           返回体与 GET 同构（都是 `_sim_view()`），多一次往返只是把"有反应"
           又往后推一秒 —— 而用户等的那 5~10 秒正是这一版要修的。 */
        SIM_VIEW = r;
        /* 告警交给 render 一起画：`err` 在面板里的位置是固定的（状态行下面），
           事后再 appendChild 会把它甩到两张表后面去。 */
        const jf = jev(r);
        /* ★ job.ok === false 才是"办砸了"（§92b）—— 以前进程在这个请求里同步跑，
           失败会变成 HTTP 错误；现在它在后台跑，成败写在 job 里，得自己取。 */
        const jerr = (jf && jf.ok === false && jf.error) || '';
        render(r, r.chain_error || (r.chain && r.chain.error) || jerr || '');
        /* 状态行已经更新成结果，进度条就该立刻收 —— 否则会出现"进度条说正在关闭，
           下面一行已经写着真实源"的瞬间矛盾（redraw 还要 1~2 s）。 */
        progHide();
        if (jerr) toast(jerr, 'bad');
        else toast(r.acted || '已切换', 'ok');
        if (redraw && !jerr) await redraw();   // 上面的表要跟着换（模拟列 / 形态）
      } catch (e) {
        /* 失败时**不要**清掉面板：链路/采集器可能是好的，只是这一次动作被拒
           （比如采集中动下端会 409）。用上次的视图重画，把这次的错误挂上去。 */
        render(SIM_VIEW, e.message);
        toast(e.message, 'bad');
      } finally {
        busy = false;
        progHide();
        // render() 会把按钮整块重建（btns 已换成新节点），这里按角色恢复即可；
        // 失败时 btns 还是原来那几个，同样能恢复。
        btns.forEach(b => {
          b.disabled = !canWrite;
          if (b.dataset.role === 'toggle') {
            b.textContent = (SIM_VIEW && SIM_VIEW.enabled) ? '关闭模拟源' : '启用模拟源';
          }
        });
      }
    }

    async function load() {
      /* ★ 先用上次的视图顶一下（2026-10-19）：
         切完模拟源要 `redraw()` 整页，本面板随之重建。原先是先 clear 再等
         GET 回来 —— 面板会空一下，看起来像"又出问题了"。 */
      if (SIM_VIEW) render(SIM_VIEW);
      let s = null;
      try { s = await API.simSource(); } catch (e) { s = null; }
      if (s) { SIM_VIEW = s; render(s); }
      else if (!SIM_VIEW) render(null);
    }

    function render(s, errText) {
      clear(bodyHost);
      btns = [];                       // 旧节点已经随 clear 一起扔掉，别再引用
      err.textContent = errText || '';
      if (!s) {
        bodyHost.appendChild(h('div', { class: 'empty',
          text: '平台没有提供模拟源接口（/api/sim-source）—— 后端版本低于 v1.7，'
              + '两端的模拟源无法从这里一键切换。' }));
        if (errText) bodyHost.appendChild(err);
        return;
      }
      const on = !!s.enabled;
      const plan = (s.plan && s.plan.downstream) || [];

      // ---- 两个按钮：启用/关闭 + 换一套 ----
      const toggle = h('button', {
        class: 'btn-sm' + (on ? '' : ' primary'),
        text: on ? '关闭模拟源' : '启用模拟源',
        disabled: !canWrite,
        // act() 靠这个认"哪一颗按钮该改成 正在…"（另两颗的文字是名词）
        dataset: { role: 'toggle' },
      });
      /* ★ v1.11：原先铺在面板正文里的四段灰字，全部收进 title。
         理由不是"字太多"——那四段各自都在回答一个真问题（换套与换进程
         的区别、关掉会不会丢配置、上端为什么只有一个量、表为什么不跟着换）。
         问题是**常驻**：一张面板底下挂四段解释，人每天看四遍，两遍之后
         就只剩下一片灰，真出事时反而找不到那句要紧的告警。
         所以：解释挂在对应按钮上（想弄清的人悬停就有），
         正文只留状态（徽章 / 表 / 红字告警）。 */
      toggle.title = on
        ? '两端一起切回真实输入源，并把链路收掉 —— 真实源填的地址 / 端口 / '
          + '监听口 / CA 一直都在，不会被清零：模拟只改「模拟那一列」'
          + '（下端 sim_host / sim_port，上端 master_host）'
        : '两端一起切到模拟输入源，按当前这套把①接入参数自动填好，并把整条链路拉起来'
          + '。★ 模拟源下页面上的数字来自设备替身（13/ modbus_slave.py），'
          + '采集器连的是它，不是现场装置，也不是真机：报文与解析跟真机是同一套，'
          + '所以联调结论对真机成立；但把这段时间的数据当运行记录留下来，'
          + '就是把编的数当实测值';
      toggle.addEventListener('click', () => act({ enable: !on }));

      const rot = h('button', { class: 'btn-sm btn-ico', disabled: !canWrite });
      rot.appendChild(iconCycle());
      rot.appendChild(h('span', { text: '换一套' }));
      rot.title = `换一套模拟数据（当前第 ${(s.preset || 0) + 1} 套，共 ${s.preset_count} 套）`
        + '—— 它改的是**表**（端口段），不是进程：跑着的替身不会自己搬过去，'
        + '要点「换一个进程」。上端只有「模拟主站地址」跟着换，'
        + '监听口与公共地址 CA 是链路属性（真实调度与模拟主站连的是同一个口）'
        + (on ? '' : '。先启用模拟源再换');
      rot.addEventListener('click', () => act({ rotate: true }));

      // ---- 「换一个进程」：改的是**进程**，不是表 ----
      // 与「换一套」分开是这版的核心：换一套只改端口段（表），进程还跑在旧段上；
      // 点这里才把替身/采集/网关/主站按**当前这一套**重新拉起。
      const chain = s.chain || { running: false, procs: [],
        counts: { up: 0, total: 0 }, preset_matches: true };
      const cn = chain.counts || { up: 0, total: 0 };
      const stale = !!chain.running && chain.preset_matches === false;

      const chg = h('button', { class: 'btn-sm btn-ico', disabled: !canWrite });
      chg.appendChild(iconSwap());
      chg.appendChild(h('span', { text: '换一个进程' }));
      chg.title = '按当前这一套把整条链路重新拉起：'
        + '设备替身 ×N → 采集器（07/）→ 上端网关（P3/）→ 模拟主站';
      chg.addEventListener('click', () => act({ action: 'restart' }, true));

      // ★ 分母用 expected（不含「该台停用 / 程序未构建」）：那几行本来就不该起，
      //   算进去会让 9/10 看起来像"掉了一个"，而实际什么都不用做。
      const exp = (cn.expected === undefined || cn.expected === null)
        ? cn.total : cn.expected;

      /* ---- 一句话状态：到底有没有在跑（2026-10-19 现场反馈） ----
         ★ 为什么需要这一行：v1.11 把「模拟态整片换配色」撤掉之后，页面上再没有
           任何地方说"现在看到的是替身还是现场"。用户只能靠"表变没变紫"判断 ——
           而紫色按设计已经不出现了，于是"没变紫"被读成"没生效"。
           补的不是颜色，是**一句能读的话**。
         ★ 三个数缺一不可：
             形态   —— 表按哪一套填的
             链路   —— 进程在不在
             采集器 —— 有没有人真的在读数（进程全在、采集器没起 = 页面不更新）
           只报前两个就会出现"9/9 全绿但页面一动不动"。 */
      const coll = s.collector || {};
      const cstate = String(coll.state || '');
      const COLL_TXT = {
        running:  '采集器在采',
        starting: '采集器正在启动',
        // ★ 说「数据已停止更新」而不是「采集器在采」：stale 现在承载两种原因
        //   —— 实录文件不长了（采集侧）、写库失败在重试（入库侧）。
        //   写「在采」会和后面的 error「写库失败」自相矛盾。具体原因看 error。
        stale:    '数据已停止更新',
        failed:   '采集器失败',
      };
      const cTxt = COLL_TXT[cstate] || '采集器未运行';
      const cOk = cstate === 'running';
      const upOk = !!chain.running && cn.up === exp;

      let sCls = 'is-mute', sHead = '真实源', sTail = [];
      if (!on) {
        sTail = ['表里是现场装置的读数'];
      } else if (!chain.running) {
        sCls = 'is-bad';
        sHead = '模拟源已启用，但链路没起来';
        sTail = ['表已切到模拟那几列，进程一个都没跑 —— 点「换一个进程」'];
      } else if (!upOk) {
        sCls = 'is-bad';
        sHead = `模拟源已启用，链路 ${cn.up}/${exp}`;
        sTail = ['有进程掉了，见下面「链路进程」'];
      } else if (!cOk) {
        // failed（进程退出 / 写库彻底断）是红的；stale / starting 还可能是过渡态。
        // ★ 一定要把后端给的**原因**带出来：只写「采集器失败」，用户能看到的
        //   只有"没反应"—— 而真正要修的那件事藏在 error 里（2026-10-19 实测：
        //   OperationalError: attempt to write a readonly database，界面上一字未提）。
        sCls = (cstate === 'failed') ? 'is-bad' : 'is-warn';
        sHead = `模拟源运行中 · 链路 ${cn.up}/${exp}`;
        sTail = [cTxt + ' —— 页面上的数不会更新'];
        const cerr = String(coll.error || '');
        if (cerr) sTail.push(cerr);
      } else {
        sCls = 'is-ok';
        sHead = `模拟源运行中 · 链路 ${cn.up}/${exp}`;
        sTail = [cTxt];
        if (coll.rows) sTail.push(`实时库 ${Number(coll.rows).toLocaleString('zh-CN')} 行`);
        const at = String(coll.imported_at || '');
        if (at.length >= 19) sTail.push(`更新于 ${at.slice(11, 19)}`);
        sTail.push('表里是替身的数');
      }

      const stateRow = h('div', { class: 'sim-state ' + sCls },
        [h('span', { class: 'sim-dot' }), h('strong', { text: sHead })]
          .concat(sTail.reduce((acc, t) => {
            acc.push(h('span', { class: 'sim-sep', text: '·' }));
            acc.push(h('span', { text: t }));
            return acc;
          }, [])));

      /* 过渡态补刷：链路在跑、采集器还没起 —— 2.5 s 后再看一眼。
         为什么不反过来也补：采集器在采、链路却报没跑是真异常，
         不能被"自动刷新"掩盖成一次偶发。 */
      if (on && chain.running && !cOk && SIM_RETRY_N < 4) {
        SIM_RETRY_N++;
        clearTimeout(SIM_RETRY);
        SIM_RETRY = setTimeout(() => { SIM_RETRY = null; load(); }, 2500);
      } else if (cOk) {
        SIM_RETRY_N = 0;
        clearTimeout(SIM_RETRY);
        SIM_RETRY = null;
      }

      btns = [toggle, rot, chg];       // 在飞期间统一禁用/恢复

      /* 状态行单独占一行，不再挤在按钮右边：那里原来放「模拟中 / 链路 9/9」
         两个小徽章，字太小，而且"链路 9/9"和"采集器在不在采"是两件事。 */
      const bar = h('div', { class: 'pm-bar' }, [
        toggle, rot, chg,
        h('div', { class: 'pm-grow' }),
        badge(`${s.preset_name} · 端口段 ${s.port_base} 起`, 'b-info'),
      ]);

      // ---- 这一套给每台设备排的端口 ----
      const planCard = plan.length
        ? table([
            { title: '设备号', key: 'device_id', mono: true },
            { title: '模拟源地址', key: 'sim_host', mono: true },
            { title: '模拟源端口', key: 'sim_port', num: true, mono: true },
          ], plan, { wrap: false })
        : h('p', { class: 'muted', style: 'margin:10px 0 0',
            text: '台账里还没有设备 —— 先在上面「②点表」里新增一台。' });

      // ---- 链路进程：谁真的在跑 ----
      // ★ 这张表存在的意义：上面那张「这一套给每台设备排的端口」是**计划**，
      //   这里是**现实**。两者不一致时，人不该靠 ping 才发现。
      // ★ 链路没在跑（关掉了模拟源 / 从没起过）就**不列这张表**：那时每个
      //   角色都显示「掉了」，一屏红字看着像坏了，其实只是"没开"——
      //   「链路未启动」这个事实由上面的徽章说，不需要九行红字再说一遍。
      const procRows = chain.running ? (chain.procs || []) : [];
      const procCard = procRows.length ? table([
        { title: '角色', key: 'role_name' },
        { title: '对象', key: 'label' },
        { title: '目标', key: 'target', mono: true },
        { title: '状态', render: r => r.alive
            ? badge('运行中', 'b-ok')
            : (r.skip ? badge('不适用', 'b-mute') : badge('掉了', 'b-bad')) },
        { title: '说明', key: 'note' },
      ], procRows, { wrap: false }) : null;

      bodyHost.appendChild(h('div', null, [
        bar,
        stateRow,
        err,
        chain.error ? h('div', { class: 'pm-err', text: chain.error }) : null,
        stale ? h('div', { class: 'pm-err', text:
          `表已经切到${s.preset_name}（端口段 ${s.port_base} 起），`
          + `但跑着的进程还在${chain.preset_name}。点「换一个进程」才会搬过去。` }) : null,
        /* ★ 这里原本有四段常驻说明（换套 vs 换进程、关掉不丢配置、上端只有
           一个可换的量、点表不跟着换套）。v1.11 起全部收进按钮 title：
           它们回答的是"我点这个会发生什么"，而不悬停也该看见的只有状态。
           两张表改成各带一个小标题 —— 「计划 / 现实」的分工由标题承担，
           不再靠一段灰字解释。 */
        planCard ? h('p', { class: 'phase-hd' }, [
          h('span', { class: 'phase-t', text: '端口计划' }),
          h('span', { class: 'phase-hint', text: '这一套给每台设备排的模拟源端口' }),
        ]) : null,
        planCard,
        procCard ? h('p', { class: 'phase-hd' }, [
          h('span', { class: 'phase-t', text: '链路进程' }),
          h('span', { class: 'phase-hint',
            text: '谁真的在听 · 采集器一次只连一台，其余替身已监听未被采集' }),
        ]) : null,
        procCard,
      ]));
    }

    load();
    return card('模拟输入源', '一个开关管两端 · 连进程一起拉起 · 换一套 / 换进程 · 关掉不留痕', host);
  }

  /* 「换一套」的循环箭头。手写 SVG（本项目不引图标库）——
     `h()` 用的是 createElement，进不了 SVG 命名空间，所以这里直接建。 */
  function iconCycle() {
    const NS = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(NS, 'svg');
    svg.setAttribute('viewBox', '0 0 16 16');
    svg.setAttribute('width', '13');
    svg.setAttribute('height', '13');
    svg.setAttribute('class', 'ic-cycle');
    svg.setAttribute('aria-hidden', 'true');
    const mk = (d, extra) => {
      const p = document.createElementNS(NS, 'path');
      p.setAttribute('d', d);
      p.setAttribute('fill', 'none');
      p.setAttribute('stroke', 'currentColor');
      p.setAttribute('stroke-width', '1.7');
      p.setAttribute('stroke-linecap', 'round');
      p.setAttribute('stroke-linejoin', 'round');
      if (extra) Object.entries(extra).forEach(([k, v]) => p.setAttribute(k, v));
      return p;
    };
    svg.appendChild(mk('M13.2 8A5.2 5.2 0 1 1 11.6 4.2'));   // 大半个圆
    svg.appendChild(mk('M11.9 1.3v3.1H8.8'));                // 箭头
    return svg;
  }

  /* 「换一个进程」的双向箭头。与「换一套」的圆箭头配成一对：
     圆箭头 = 换这一套的**内容**，双向箭头 = 换**跑着的那一组进程**。 */
  function iconSwap() {
    const NS = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(NS, 'svg');
    svg.setAttribute('viewBox', '0 0 16 16');
    svg.setAttribute('width', '13');
    svg.setAttribute('height', '13');
    svg.setAttribute('class', 'ic-cycle');
    svg.setAttribute('aria-hidden', 'true');
    const mk = (d) => {
      const p = document.createElementNS(NS, 'path');
      p.setAttribute('d', d);
      p.setAttribute('fill', 'none');
      p.setAttribute('stroke', 'currentColor');
      p.setAttribute('stroke-width', '1.7');
      p.setAttribute('stroke-linecap', 'round');
      p.setAttribute('stroke-linejoin', 'round');
      return p;
    };
    svg.appendChild(mk('M2.6 5.6h10.2M10.4 3.2l2.4 2.4-2.4 2.4'));   // 上行
    svg.appendChild(mk('M13.4 10.4H3.2M5.6 8l-2.4 2.4L5.6 12.8'));   // 下行
    return svg;
  }

  async function devices(ctx) {
    ctx.setSub('两路输入源 · 调度主站接入 / 站内设备接入 · 设备清册并入 ②点表');
    const canWrite = ['operator', 'admin'].includes(ctx.user && ctx.user.role);
    const d = await API.devices();
    let rt = { items: [] };
    try { rt = await API.realtime(ctx.scenario); } catch (e) { rt = { items: [] }; }
    const byId = Object.fromEntries((rt.items || []).map(x => [x.device_id, x]));

    /* 逐台编辑**不在页面上铺开**，平时是隐藏的一层：它有 9 列 × 32 行的点表，
       铺在页尾会把整页撑到几千像素高，且滚动位置一乱就分不清"滚到哪了"。
       ★ key 复用：页面来回切不会每切一次就往 body 上多挂一层。 */
    const editorModal = modal({
      key: 'dev-editor',
      title: '逐台编辑',
      sub: '① 接入参数 + ② 点表 都是这一台的 · Esc 或「关闭」返回',
    });
    const access = pointAccess(d.items.slice(), ctx, {
      onId: (id) => editorModal.setTitle(`逐台编辑 · ${id || '未选中'}`),
      onOpen: () => editorModal.open(),
    });

    /* 上端 IOA 转发表的编辑弹层 —— 与下端那个同规格（全页、按 key 复用、
       挂在 body 上所以换页要显式关），但它改的是**全站一份**的表。 */
    /* 「接入调度主站」九步向导 —— 全页弹层，与逐台编辑同规格（按 key 复用）。
       ★ 创建顺序**有意**放在 upPmModal 之前：两个 .mdl 的 z-index 相同，
         后挂到 body 的盖在上面。向导第 6 步要打开转发表编辑器，
         所以转发表那一层必须在向导之后创建，否则会被向导压住点不到。 */
    const wzModal = modal({
      key: 'access-wizard',
      title: '接入调度主站',
      sub: '九步填报 · 只有第 6 步会改本机配置 · Esc 或「退出」返回',
    });
    const wizard = accessWizard({
      canWrite,
      onEditPm: () => openUpPm(),
      onChanged: () => { redrawAll(); },
      onCompleted: () => { redrawAll(); },
      close: () => wzModal.close(),
    });
    wzModal.body.appendChild(wizard.node);
    ctx.onDispose(() => { wizard.flush(); wzModal.close(); });
    // 先读到草稿再开：否则会闪一下空壳，人以为是"填的东西没了"。
    const openWizard = () => { wizard.load().then(() => wzModal.open()); };

    const upPmModal = modal({
      key: 'up-pm-editor',
      title: '编辑 IOA 转发表',
      sub: '全站一份 · IOA ↔ EMS 点名 · Esc 或「关闭」返回',
    });
    const upPm = upPmEditor(canWrite, {
      onSaved: () => { upPmModal.close(); redrawAll(); wizard.refresh(); },
    });
    upPmModal.body.appendChild(upPm.node);
    ctx.onDispose(() => upPmModal.close());
    // ★ 先读到数据再开：否则会闪一下空壳，人以为是"表没了"。
    const openUpPm = () => { upPm.load().then(() => upPmModal.open()); };
    editorModal.body.appendChild(access.node);
    // ★ 换页时必须把弹层收起来：它挂在 document.body 上，不在 #content 里 ——
    //   不显式关，切到别的页之后它还盖在上面，人看着像"点菜单没反应"。
    ctx.onDispose(() => editorModal.close());
    await access.load();

    // 两端输入源：**失败也不让整页挂掉** —— 老后端没有 /api/data-source，
    // 台账与点表仍然要看得到（那是这一页更基础的功能）。
    let ds = null;
    const fetchDs = async () => {
      try { ds = await API.dataSource(); } catch (e) { ds = null; }
    };
    // 接入明细（每台设备的目标与形态）：同样允许失败。
    let acc = null;
    const fetchAccess = async () => {
      try { acc = await API.access(); } catch (e) { acc = null; }
    };

    const srcHost = h('div');

    /* ★ v1.11：模拟态**不再给上面的表换配色**。
       原做法是给 srcHost 贴 .sim-mode，让上端 ①② / 下端 ①② 四张卡整体
       变紫。它的出发点是"眼睛落到表上就知道不是真数据"，但代价是：
       ① 一个页尾开关的状态被画到了两张与它无关的表上，人分不清"这一列
          是模拟值"和"整张表都在模拟"；
       ② 模拟源的信息散在两处（表变了色 + 面板上有徽章），说清"模拟源
          到底改了哪几列"反而更难。
       现在模拟态只由页尾面板自己表达：模拟中徽章 + 套名 + 链路徽章。
       上面的表始终是"你要连的东西"，不再兼职当状态灯。 */

    // 所有会改配置的动作最后都走这里：两路输入源重画一次。
    // ★ 清册列跟着一起重画：它在 ②点表 的每一行里，重画的是同一张表。
    async function redrawAll() {
      await Promise.all([fetchDs(), fetchAccess()]);
      clear(srcHost);
      if (!ds) {
        srcHost.appendChild(card('两路输入源', null, h('div', { class: 'empty',
          text: '平台没有提供数据源接口（/api/data-source）—— 后端版本低于 v1.7，'
              + '两端的输入源无法从这里设置。' })));
      } else {
        const mix = mixBanner(ds.mix);
        srcHost.appendChild(h('div', { class: 'grid' }, [
          mix,
          accessUpSection(ds, canWrite, redrawAll, openUpPm, openWizard),
          // id 为空 = 空库时那个「新增第一台设备」按钮：直接开弹层（里面有新增三件套）
          accessDownSection(ds, acc, d.items || [], canWrite, redrawAll,
                            (id) => { if (id) access.select(id); else editorModal.open(); },
                            byId),
        ]));
      }
    }
    await redrawAll();

    // 模拟输入源：**页尾一个面板管两端** —— 见 simSourcePanel 的注释。
    const simPanel = simSourcePanel(canWrite, redrawAll);

    /* 逐台编辑不在这里 —— 它在弹层里（editorModal），点任一行的「编辑」打开。 */
    return h('div', { class: 'grid' }, [
      srcHost,
      card('点表口径', null, h('div', { style: 'font-size:12.5px;line-height:1.9' }, [
        h('p', { style: 'margin:0 0 8px', html:
          '点名与索引取自 <span class="mono">07/src/rtdb/ems_point_table.h</span>，' +
          '<strong>本项目不另抄一份</strong>。' }),
        h('p', { class: 'muted', style: 'margin:0', html:
          '本项目曾因手抄品质常量出过事故（<span class="mono">P3/docs/README.md §7.1</span>：' +
          '值全对、测试全绿，只是调度画面上 24 个点全灰）。' }),
      ])),
      simPanel,
    ]);
  }

  /* ======================= 10. 用户与权限 ======================= */
  async function users(ctx) {
    ctx.setSub('用户、角色与操作审计（仅 admin）');
    const host = h('div');

    async function draw() {
      const [us, au] = await Promise.all([API.users(), API.audit(100)]);
      clear(host);
      host.appendChild(h('div', { class: 'grid g-1-2' }, [
        card('用户', `共 ${us.items.length} 个`, h('div', null, [
          table([
            { title: 'ID', key: 'user_id', num: true },
            { title: '用户名', key: 'username', mono: true },
            { title: '角色', render: r => badge(r.role, r.role === 'admin' ? 'b-bad' : (r.role === 'operator' ? 'b-warn' : 'b-info')) },
            { title: '姓名', key: 'display_name' },
            { title: '启用', render: r => badge(r.enabled ? '是' : '否', r.enabled ? 'b-ok' : 'b-mute') },
          ], us.items, { wrap: false }),
          h('div', { style: 'display:flex;gap:8px;margin-top:14px;flex-wrap:wrap' }, [
            (() => { const i = h('input', { type: 'text', placeholder: '用户名', style: 'width:110px' }); i.id = 'nu-name'; return i; })(),
            (() => { const i = h('input', { type: 'password', placeholder: '口令(≥6位)', style: 'width:130px' }); i.id = 'nu-pass'; return i; })(),
            (() => { const s = h('select', null, [
              h('option', { value: 'viewer', text: 'viewer' }),
              h('option', { value: 'operator', text: 'operator' }),
              h('option', { value: 'admin', text: 'admin' })]); s.id = 'nu-role'; return s; })(),
            (() => {
              const b = h('button', { class: 'btn-sm primary', text: '新建用户' });
              b.addEventListener('click', async () => {
                try {
                  await API.createUser({
                    username: $('#nu-name').value, password: $('#nu-pass').value,
                    role: $('#nu-role').value,
                  });
                  toast('用户已创建', 'ok');
                  draw();
                } catch (e) { toast(e.message, 'bad'); }
              });
              return b;
            })(),
          ]),
        ])),
        card('操作审计', '最近 100 条', table([
          { title: '时间', key: 'ts', mono: true },
          { title: '用户', key: 'username', mono: true },
          { title: '动作', key: 'action', mono: true },
          { title: '对象', key: 'target', mono: true },
          { title: '详情', key: 'detail' },
        ], au.items)),
      ]));
    }

    await draw();
    return host;
  }

  return { overview, realtime, curves, alarms, strategies, economics, report,
           structure, wiring, devices, users };
})();
