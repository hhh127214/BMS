/* =====================================================================
   15/ — 各页面渲染
   每个页面是一个 async 函数，接收
     ctx = { scenario, user, mode, modes, engine, engineInfo, t,
             setT, setSub, onDispose, reloadEngine, openDataset, goPage }
   返回一个 DOM 节点。数据全部来自 14/ 的 REST 接口。
   ===================================================================== */

const Pages = (() => {

  /* 页面局部偏好的跨渲染记忆。
     ★ 运行模式下页面每 5 s 自动重渲染一次，如果每次都把用户选的曲线、
       报表视角重置回默认，界面会"自己乱跳"——用户刚点掉一条曲线，
       下一秒它又回来了。所以这类纯展示偏好要记在页面之外。 */
  const PREFS = {};

  /* ======================= 1. 总览（首页） ======================= */
  async function overview(ctx) {
    // 跟随「最近一次在实时监控里看的那一拍」；没去过实时监控则取最后一拍（默认）。
    const d = await API.overview(ctx.scenario, ctx.t);
    const mode = (ctx.modes || []).find(m => m.mode_id === ctx.mode) || d.mode || {};
    const engName = d.engine === 'run' ? '运行模式' : '仿真模式';
    ctx.setSub(`${engName} · 运行模式「${mode.name || '—'}」 · 数据集「${d.scenario_name || d.scenario}」`
      + ` · ${d.time_base === 'wall' ? '实时时刻' : '仿真时刻'} ${d.time}`
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
    return card('经济性', '仿真日核算', h('div', null, [
      ratioRow('净收益', eco.net_benefit_cny, Math.max(eco.saving_total_cny, 1), 'var(--ok)', ' 元'),
      ratioRow('节省电费', eco.saving_total_cny, Math.max(eco.cost_total_base_cny, 1), 'var(--accent)', ' 元'),
      ratioRow('实际电费', eco.cost_total_cny, Math.max(eco.cost_total_base_cny, 1), 'var(--c-grid)', ' 元'),
      ratioRow('无储能基线', eco.cost_total_base_cny, Math.max(eco.cost_total_base_cny, 1), 'var(--text-3)', ' 元'),
      h('p', { class: 'muted', style: 'font-size:11.5px;margin:10px 0 0' ,
        text: '口径：10/ 的经济性核算（含电池衰减成本）。当前为仿真结果，非实测账单。' }),
    ]));
  }

  function tipCard(d) {
    return card('数据来源', null, h('div', { style: 'font-size:12.5px;line-height:1.9' }, [
      h('p', { style: 'margin:0 0 8px', html:
        '本平台的现场量有 <strong>两个来源</strong>：<span class="mono">10/</span> 模块的 24 h 离线仿真（默认场景），' +
        '以及 <span class="mono">07/</span> 现场进程 <code>--record</code> 的实时实录（live 场景，墙钟口径）。' +
        '<br>仿真场景「实时」= 仿真时标，8640 个采样点（10 s 粒度）可任意速度回放。' }),
      h('p', { class: 'muted', style: 'margin:0', html:
        '接上真设备（<span class="mono">13/</span> Modbus 主站 / <span class="mono">P3/</span> IEC104 从站）后，' +
        '<strong>表结构与接口都不用改</strong>。' }),
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
    ctx.setSub('按仿真时标回放；拖动滑块或播放查看任意时刻的全站快照');
    const total = 86400;
    // ★ 回到本页时停在上次看的那一拍：app 层的 state.t 跨页共享（方案 A），
    //   没看过（null）才给最后一拍。原实现每次进页都从 total 起，切走再回来就被拉回末尾。
    let t = (ctx.t === null || ctx.t === undefined)
      ? total : Math.max(0, Math.min(total, Number(ctx.t)));
    let speed = 4;

    /* ---- 回放控制台：播放钮 + 大时钟 + 滑块 + 倍速 ---- */
    const icoPlay = h('span', { class: 'rt-ico',
      html: '<svg viewBox="0 0 24 24" width="15" height="15"><path d="M8 5v14l11-7z" fill="currentColor"/></svg>' });
    const icoPause = h('span', { class: 'rt-ico hidden',
      html: '<svg viewBox="0 0 24 24" width="15" height="15"><path d="M7 5h4v14H7zM13 5h4v14h-4z" fill="currentColor"/></svg>' });
    const playBtn = h('button', { class: 'rt-play', title: '播放 / 暂停' }, [icoPlay, icoPause]);
    const clock = h('span', { class: 'rt-clock', text: fmt.hhmm(t) });
    const stateHost = h('span');
    const slider = h('input', { type: 'range', min: '0', max: String(total), step: '10', value: String(t) });

    const SPEEDS = [[1, '1×'], [4, '4×'], [16, '16×']];
    /* ★ 倍速选择器要带蓝色滑块指示条：.seg button.active 只把文字刷白，
       底色全靠 .seg-ind —— 不接上的话白字落在浅灰底上，等于看不见。
       对位时机：页面挂载后由 app.js 的 placeSegs() 统一摆一次（此处还没布局），
       这里只负责点击时把滑块滑过去。 */
    const speedSeg = h('div', { class: 'seg' });
    const segBtns = SPEEDS.map(([s, name]) => {
      const b = h('button', { text: name, class: s === speed ? 'active' : '' });
      b.addEventListener('click', () => {
        speed = s;
        segBtns.forEach(x => x.classList.toggle('active', x === b));
        moveSegInd(speedSeg);
      });
      return b;
    });
    segBtns.forEach(b => speedSeg.appendChild(b));

    const consoleCard = h('div', { class: 'card rt-console' }, [
      playBtn,
      h('div', { class: 'rt-clock-box' }, [clock, h('span', { class: 'rt-sub', text: '仿真时刻' })]),
      stateHost,
      h('div', { class: 'rt-slider' }, [
        h('span', { class: 'rt-sub', text: '00:00' }), slider, h('span', { class: 'rt-sub', text: '24:00' }),
      ]),
      speedSeg,
      h('span', { class: 'rt-sub', text: '24 h · 10 s/格（日志粒度）' }),
    ]);

    const pwHost = h('div');
    /* ---- 能量流：整张卡只建一次，回放时只 patch 数字 ----
       ★ 为什么不能每拍重建：粗箭头杆里的流动光带是 CSS 动画（stroke-dashoffset
         在跑），元素一旦被整段换掉，新元素就从相位 0 重新开始 ——
         每 120 ms 换一次，看着就是"胶囊被反复拽回起点"，也就是卡顿。
        ★ 所以：结构签名（哪几路有功率、朝哪边）不变时只改文本与 SOC 条宽度，
         元素原地保留，动画一直连着跑；签名变了才整张重建（那时"方向"本身
         变了，重画是对的）。
        ★ 签名口径必须与 ui.js 里 arrow() 的 active/方向判定逐条对上，
         否则会出现"读数变了但箭头没跟着变"。
        ★ 卡片标题（t=… · 时刻）每拍都变，用 flowSub 就地改文本，不动卡片结构。 */
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

    /* ---- 全天 SOC 走势：填「储能与闭环」下方那块空地 ----
       取一次（145 点 = 每 10 min 一个），之后回放只移动游标 ——
       拖动滑块时不重复请求，也不整卡重建。 */
    const dayHost = h('div');
    let dayT = [], daySoc = [], dayLabels = [];
    try {
      const dv = await API.series(ctx.scenario, ['soc'], 145);
      dayT = dv.t_s || [];
      daySoc = ((dv.series || {}).soc || []).map(v => (v === null || v === undefined) ? null : v * 100);
      dayLabels = dv.time || [];
    } catch (e) { dayT = []; }
    // 当前拍 → 最近采样点下标（145 次比较，每拍都算也不心疼）
    const dayIdx = (tt) => {
      let bi = 0, bd = Infinity;
      for (let i = 0; i < dayT.length; i++) {
        const d = Math.abs(dayT[i] - tt);
        if (d < bd) { bd = d; bi = i; }
      }
      return bi;
    };

    // 设备类型 → 色点（与图表序列色一致，眼睛不用重新学一套颜色）
    const TYPE_COLOR = {
      PCS: 'var(--accent)', BMS: 'var(--c-bat)', 电表: 'var(--info)',
      光伏: 'var(--c-pv)', 变压器: 'var(--c-grid)', 负荷: 'var(--c-load)',
    };

    // ★ 请求序号防乱序：拖滑块 / 播放会并发多次 render，慢的旧响应若最后
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
      ctx.setT(tt);                    // 记录当前时刻，供总览页跟随（方案 A）
      clock.textContent = rt.time;
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

      // 能量流随滑块走：拖滑块时数字表格不容易看出"谁在供谁"，
      // 一张图能立刻回答"这拍是电网在供、还是电池在放"。
      // ★ 只 patch：结构没变就不换元素，杆内光带的动画不会被打断（见上面 flowSig 注释）。
      flowSub.textContent = `t=${fmt.num(ov.t_s, 0)} s · ${ov.time}`;
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
          marker: { i: dayIdx(tt), label: fmt.hhmm(tt) },
        })));
        const lo = Math.min(...daySoc.filter(v => v !== null));
        const hi = Math.max(...daySoc.filter(v => v !== null));
        const now = daySoc[dayIdx(tt)];
        battHost.appendChild(card('全天 SOC 走势',
          `当前 ${fmt.num(now, 1)} % · 全天 ${fmt.num(lo, 1)}~${fmt.num(hi, 1)} %`, dayHost));
      }

      /* ---- 设备实时量（全宽） ---- */
      clear(devHost);
      devHost.appendChild(card('设备实时量', `t=${fmt.num(rt.t_s, 0)} s · ${rt.time}`,
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

    function setPlaying(on) {
      icoPlay.classList.toggle('hidden', on);
      icoPause.classList.toggle('hidden', !on);
    }

    let playing = null;
    playBtn.addEventListener('click', () => {
      if (playing) {
        clearInterval(playing); playing = null; setPlaying(false); return;
      }
      setPlaying(true);
      // ★ 播放推进不盲目 setInterval：上一拍还没渲染完就不推进，避免请求
      //   堆积（异步响应慢时，定时器会越叠越多）。用 inFlight 做闸门。
      playing = setInterval(() => {
        // ★ 切页后本页节点已从文档摘除，定时器必须自己停 ——
        //   否则它在后台一直空转、持续发请求（页面没了但闭包还活着）。
        if (!slider.isConnected) { clearInterval(playing); playing = null; setPlaying(false); return; }
        if (inFlight) return;
        let v = Number(slider.value) + 600 * speed;   // 每 120 ms 推进 10 分钟 × 倍速
        if (v > total) v = 0;
        slider.value = String(v);
        render(v);
      }, 120);
    });

    // ★ 滑块拖动节流：input 事件每动一下都 render 会瞬间并发一堆请求，
    //   用 requestAnimationFrame 合并同一帧内的多次拖动，只发最后一次。
    let rafPending = false, lastV = t;
    let inFlight = false;
    const rawRender = render;
    render = async (tt) => {
      inFlight = true;
      try { await rawRender(tt); } finally { inFlight = false; }
    };
    slider.addEventListener('input', () => {
      if (playing) return;
      lastV = Number(slider.value);
      if (rafPending) return;
      rafPending = true;
      requestAnimationFrame(() => {
        rafPending = false;
        render(lastV);
      });
    });

    await render(t);
    return h('div', { class: 'grid' }, [
      consoleCard, pwHost,
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

  /* ======================= 4. 告警中心 ======================= */
  async function alarms(ctx) {
    ctx.setSub('等级筛选 + 来源统计 + 24 h 分布');
    let level = '';
    let offset = 0;
    const LIMIT = 50;
    const listHost = h('div');
    const sumHost = h('div');
    const pageLabel = h('span', { class: 'muted' });

    async function draw() {
      const [sum, list] = await Promise.all([
        API.alarmSummary(ctx.scenario),
        API.alarms(ctx.scenario, { level, limit: LIMIT, offset }),
      ]);
      clear(sumHost);
      sumHost.appendChild(h('div', { class: 'grid g3' }, [
        card('按等级', null, h('table', { class: 'tbl' }, h('tbody', null,
          (sum.by_level || []).map(r => h('tr', null, [
            h('td', null, levelBadge(r.level)),
            h('td', { class: 'num', text: String(r.n) }),
          ])).concat([h('tr', null, [h('td', { class: 'muted', text: '合计' }),
            h('td', { class: 'num', text: String((sum.by_level || []).reduce((a, b) => a + b.n, 0)) })])])))),
        card('按来源', null, h('table', { class: 'tbl' }, h('tbody', null,
          (sum.by_source || []).slice(0, 8).map(r => h('tr', null, [
            h('td', { text: r.source || '—' }),
            h('td', { class: 'num', text: String(r.n) }),
          ]))))),
        card('24 h 分布', '按小时', h('div', {
          html: barChart({
            labels: (sum.timeline || []).map(r => `${String(r.hour).padStart(2, '0')}`),
            values: (sum.timeline || []).map(r => r.n), height: 170, color: 'var(--warn)',
          }),
        })),
      ]));

      clear(listHost);
      listHost.appendChild(h('div', { class: 'pager' }, [
        h('button', { class: 'btn-sm', text: '上一页', disabled: offset <= 0,
          onclick: () => { offset = Math.max(0, offset - LIMIT); draw(); } }),
        h('button', { class: 'btn-sm', text: '下一页', disabled: offset + LIMIT >= list.total,
          onclick: () => { offset += LIMIT; draw(); } }),
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

    const filter = h('div', { style: 'display:flex;gap:8px;flex-wrap:wrap' },
      [['', '全部'], ['INFO', 'INFO'], ['WARNING', 'WARNING'], ['DERATED', 'DERATED'],
       ['FAULT', 'FAULT'], ['EMERGENCY', 'EMERGENCY']].map(([lv, name]) => {
        const b = h('button', { class: `btn-sm ${lv === level ? 'primary' : ''}`, text: name });
        b.addEventListener('click', () => {
          level = lv; offset = 0;
          $$('button', filter).forEach(x => x.classList.remove('primary'));
          b.classList.add('primary');
          draw();
        });
        return b;
      }));

    await draw();
    return h('div', { class: 'grid' }, [
      card('筛选', '告警等级', filter),
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
  async function report(ctx) {
    ctx.setSub('日 / 周 / 月聚合视角');
    let period = PREFS.report || 'day';
    const host = h('div');

    async function draw() {
      const d = await API.report(ctx.scenario, period);
      clear(host);
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
    [['day', '日报'], ['week', '周报'], ['month', '月报']].forEach(([k, n]) => {
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
      h('div', { style: 'display:flex;justify-content:space-between;align-items:center' }, [
        seg,
        h('div', { style: 'display:flex;gap:9px' }, [
          h('button', { class: 'btn-sm', text: '导出时序', onclick: () => API.download('timeseries', ctx.scenario) }),
          h('button', { class: 'btn-sm', text: '导出告警', onclick: () => API.download('alarms', ctx.scenario) }),
        ]),
      ]),
      host,
    ]);
  }

  /* ======================= 8. 仿真（日 / 周 / 月批量） ======================= */

  /* 仿真运行状态 → 徽章（与 14/src/simrun.py 的 sim_run.status 一一对应） */
  const SIM_STATE = {
    running: { t: '运行中', k: 'b-info' },
    ok:      { t: '完成',   k: 'b-ok' },
    failed:  { t: '失败',   k: 'b-bad' },
  };

  async function sim(ctx) {
    const canW = ['operator', 'admin'].includes(ctx.user && ctx.user.role);
    let kind = PREFS.simKind || 'day';
    let fault = !!PREFS.simFault;
    let kk = null;              // 最近一次 /api/sim/runs 返回的 kinds 元数据
    let timer = null;
    let busy = false;
    const pollHost = h('div');

    /* --- 时长分段（建一次，别在轮询里重建：重建会把滑动指示条的宽度量成 0） --- */
    const kindSeg = h('div', { class: 'seg' });
    const faultSeg = h('div', { class: 'seg' });
    const specLine = h('div', { class: 'sim-spec' });
    const warnLine = h('div', { class: 'sim-warn' });

    function kOf(id) {
      return (kk || []).find(x => x.id === id) || { name: id, short: id };
    }

    /* 当前选择 → 一行参数说明（时长 / 记录粒度 / 预计记录条数 / 预计墙钟耗时） */
    function syncSpec() {
      const k = kOf(kind);
      const durDays = (k.duration_s || 0) / 86400;
      const grain = (k.dt_s || 1) * (k.log_every || 1);
      const rows = Math.round((k.duration_s || 0) / (grain || 1));
      clear(specLine);
      specLine.appendChild(h('span', null, [
        h('b', { text: k.name || kind }),
        ` · 仿真时长 ${fmt.num(durDays, 0)} 天（${fmt.int(k.duration_s)} s）`,
        ` · 记录粒度 ${fmt.int(grain)} s`,
        ` · 约 ${fmt.int(rows)} 条记录`,
        ` · 预计墙钟约 ${fmt.num(k.est_wall_s, 0)} s`,
      ]));
      clear(warnLine);
      warnLine.appendChild(h('span', { text:
        '★ 诚实边界：10/ 的负荷与光伏是 96 点「典型日」曲线并对 24 h 取模回绕 —— '
        + '周 / 月仿真得到的是同一个典型日重复 N 次，不是真实多日天气序列；'
        + (fault ? '故障窗按绝对时间轴定义，注入只发生在第 0 天。' : '本次为正常运行，无故障注入。') }));
    }

    function buildSegs() {
      clear(kindSeg);
      (kk || []).forEach(k => {
        const b = h('button', { class: k.id === kind ? 'active' : '',
          title: k.name, text: k.short || k.name });
        b.addEventListener('click', () => {
          kind = k.id;
          PREFS.simKind = k.id;
          $$('button', kindSeg).forEach(x => x.classList.toggle('active', x === b));
          moveSegInd(kindSeg);
          syncSpec();
        });
        kindSeg.appendChild(b);
      });
      clear(faultSeg);
      [['normal', '正常工况', false], ['fault', '故障工况', true]].forEach(([id, name, f]) => {
        const b = h('button', { class: (!!fault === f) ? 'active' : '', title: name, text: name });
        b.addEventListener('click', () => {
          fault = f;
          PREFS.simFault = f;
          $$('button', faultSeg).forEach(x => x.classList.toggle('active', x === b));
          moveSegInd(faultSeg);
          syncSpec();
        });
        faultSeg.appendChild(b);
      });
      moveSegInd(kindSeg, false);
      moveSegInd(faultSeg, false);
    }

    /* ---------------------------- 发起一次仿真 ---------------------------- */
    async function fire() {
      if (!canW) { toast('当前角色为只读，无法发起仿真（需 operator 及以上）', 'bad'); return; }
      if (busy) return;
      busy = true;
      try {
        const r = await API.simStart({ kind, fault });
        toast(`已发起 ${kOf(kind).name}（${r.run_id}）：${fault ? '故障' : '正常'}工况，`
          + '后台离线跑，跑完自动入库', 'ok');
        if (ctx.engine !== 'sim') {
          toast('当前是运行模式，跑完点结果卡里的「查看结果」会切到仿真模式', 'ok');
        }
        await draw();
        // 台账里已经有 running 行，draw() 会自己把轮询挂上
      } catch (e) {
        toast(e.message, 'bad');
      } finally { busy = false; }
    }

    /* ------------------------------ 删除一次 ------------------------------ */
    async function remove(r) {
      if (!canW) { toast('当前角色为只读，无法删除仿真记录', 'bad'); return; }
      if (!window.confirm(`删除 ${r.run_id}（${r.kind_name || r.kind}）？\n`
        + '会同时清掉它的时序数据、经济性、不变量与产物目录，不可撤销。')) return;
      try {
        const d = await API.simDelete(r.run_id);
        toast(`已删除 ${r.run_id}（清了 ${d.removed && d.removed.step_record || 0} 行时序）`, 'ok');
        await draw();
      } catch (e) { toast(e.message, 'bad'); }
    }

    /* 切到仿真模式并打开某个数据集 —— 由 app.js 提供的 ctx.openDataset 完成，
       它会重建顶栏分段器，所以这里不要自己改 location.hash。 */
    async function open(scenarioId) {
      try {
        await ctx.openDataset(scenarioId);
        toast(`已切到仿真模式，数据集 ${scenarioId}`, 'ok');
      } catch (e) { toast(e.message, 'bad'); }
    }

    /* ------------------------------ 渲染主体 ------------------------------ */
    function runCard(r) {
      const st = SIM_STATE[r.status] || { t: r.status, k: 'b-mute' };
      const est = r.est_wall_s || 0;
      const el = r.elapsed_s || 0;
      const pct = est > 0 ? Math.min(99, el / est * 100) : 0;
      return card('进行中', `${r.run_id} · ${r.kind_name} · ${r.fault ? '故障工况' : '正常工况'}`,
        h('div', null, [
          h('div', { class: 'sim-runrow' }, [
            badge(st.t, st.k),
            h('span', { class: 'mono', text: r.started_at || '' }),
            h('span', { class: 'muted', text: `已跑 ${fmt.num(el, 1)} s / 预计约 ${fmt.num(est, 0)} s` }),
          ]),
          h('div', { class: 'bar', style: 'margin:10px 0 6px' },
            [h('i', { style: `width:${pct.toFixed(1)}%` })]),
          h('p', { class: 'muted', style: 'font-size:12px;margin:0', text:
            '仿真器是批量程序，中途没有可看的部分结果；状态由 sim_run 台账表达。'
            + '页面每 2 s 自动刷新一次。' }),
        ]));
    }

    function lastCard(r, detail) {
      if (!r) return card('最近一次仿真', '还没有跑过仿真', h('div', { class: 'empty', text:
        '上面选好时长与工况，点「发起仿真」。日仿真约 1 s、周约 3 s、月约十几秒。' }));
      const st = SIM_STATE[r.status] || { t: r.status, k: 'b-mute' };
      const ok = r.status === 'ok';
      const days = (detail && detail.energy_days) || [];
      const nodes = [
        h('div', { class: 'sim-runrow' }, [
          badge(st.t, st.k),
          h('span', { class: 'mono', text: `${r.run_id} · ${r.scenario_id}` }),
          h('span', { class: 'muted', text: `${r.kind_name} · ${r.fault ? '故障工况' : '正常工况'}` }),
          h('span', { class: 'muted', text: `耗时 ${fmt.num(r.wall_s, 2)} s` }),
        ]),
      ];
      if (r.error) {
        nodes.push(h('div', { class: 'sim-err', text: r.error }));
      }
      if (ok) {
        nodes.push(h('div', { class: 'grid g4', style: 'margin-top:12px' }, [
          kpi('数据点数', fmt.int(r.log_rows), '行', `步数 ${fmt.int(r.steps)}`, null),
          kpi('告警', fmt.int(r.alarms_total), '条', '本数据集', (r.alarms_total ? 'warn' : 'ok')),
          kpi('净收益', fmt.money(r.net_benefit_cny, 0), '元', '套利口径',
            (r.net_benefit_cny > 0 ? 'ok' : null)),
          kpi('硬不变量', r.invariants_all_ok ? '全过' : '未过', '',
            'out_of_interval / grid_breach / soc_violation…',
            r.invariants_all_ok ? 'ok' : 'bad'),
        ]));
        if (days.length > 1) {
          nodes.push(h('div', { style: 'margin-top:12px' }, [
            h('p', { class: 'muted', style: 'font-size:12px;margin:0 0 6px', text:
              `逐日能量（${days.length} 天）—— 每天都是同一个典型日；净收益按当天吞吐量占比分摊` }),
            table([
              { title: '日期', key: 'date', mono: true },
              { title: '充电 (kWh)', num: true, render: x => fmt.num(x.charge_kwh, 1) },
              { title: '放电 (kWh)', num: true, render: x => fmt.num(x.discharge_kwh, 1) },
              { title: '净收益 (元)', num: true, render: x => fmt.num(x.net_benefit_cny, 2) },
            ], days, { wrap: false }),
          ]));
        }
        nodes.push(h('div', { style: 'margin-top:12px;display:flex;gap:9px' }, [
          h('button', { class: 'btn-sm primary', text: '查看结果（切到仿真模式）',
            onclick: () => open(r.scenario_id) }),
        ]));
      }
      return card('最近一次仿真', `${r.kind_name} · 完成于 ${r.finished_at || '—'}`, h('div', null, nodes));
    }

    function histCard(items) {
      const rows = (items || []).map(r => ({
        ...r,
        _st: SIM_STATE[r.status] || { t: r.status, k: 'b-mute' },
      }));
      return card('历史仿真', `共 ${rows.length} 次（编号只增不复用）`, table([
        { title: '编号', key: 'run_id', mono: true },
        { title: '时长', render: r => r.kind_name || r.kind },
        { title: '工况', render: r => r.fault ? badge('故障', 'b-warn') : badge('正常', 'b-ok') },
        { title: '状态', render: r => badge(r._st.t, r._st.k) },
        { title: '开始', key: 'started_at', mono: true },
        { title: '耗时 (s)', num: true, render: r => fmt.num(r.wall_s, 2) },
        { title: '记录行', num: true, render: r => fmt.int(r.rows_now) },
        { title: '净收益 (元)', num: true, render: r => fmt.money(r.net_benefit_cny, 2) },
        { title: '不变量', render: r => r.status !== 'ok' ? '—'
          : badge(r.invariants_all_ok ? '全过' : '未过', r.invariants_all_ok ? 'b-ok' : 'b-bad') },
        { title: '操作', render: r => h('div', { style: 'display:flex;gap:6px' }, [
          r.status === 'ok'
            ? h('button', { class: 'btn-sm', text: '查看', onclick: () => open(r.scenario_id) })
            : null,
          h('button', { class: 'btn-sm', text: '删除', disabled: r.status === 'running',
                        onclick: () => remove(r) }),
        ]) },
      ], rows, { empty: '还没有仿真记录' }));
    }

    async function draw() {
      const d = await API.simRuns(30);
      kk = d.kinds || kk;
      if (!kk.some(k => k.id === kind)) kind = kk[0].id;
      if (!kindSeg.childElementCount) buildSegs();
      syncSpec();

      const items = d.items || [];
      const running = items.filter(r => r.status === 'running');
      const last = items[0] || null;
      let detail = null;
      if (last && last.status === 'ok') {
        try { detail = await API.simRun(last.run_id); } catch (e) { detail = null; }
      }

      clear(pollHost);
      if (running.length) pollHost.appendChild(runCard(running[0]));
      pollHost.appendChild(lastCard(last, detail));
      pollHost.appendChild(histCard(items));

      syncTimer(running.length > 0);
    }

    /* 轮询：只在有 running 的时候挂，跑完自动摘掉（否则页面会一直 2 s 打一次接口） */
    function syncTimer(hasRunning) {
      if (hasRunning && !timer) {
        timer = setInterval(async () => {
          if (document.hidden) return;
          try { await draw(); } catch (e) { /* 单次失败不弹窗 */ }
        }, 2000);
      } else if (!hasRunning && timer) {
        clearInterval(timer);
        timer = null;
      }
    }
    ctx.onDispose(() => { if (timer) { clearInterval(timer); timer = null; } });

    /* 初始 kinds：先拉一次台账把时长清单拿到，再建按钮 */
    const first = await API.simRuns(30);
    kk = first.kinds || [];
    buildSegs();
    syncSpec();
    await draw();

    const info = ctx.engineInfo || {};
    const engNote = ctx.engine === 'run'
      ? '当前是运行模式：页面在看实时数据；仿真是离线跑，不影响实时引擎。跑完点「查看结果」会切到仿真模式。'
      : '当前是仿真模式：页面在看最近一次成功仿真的数据集。';

    return h('div', { class: 'grid' }, [
      card('发起仿真', '离线批量跑 10/ 的仿真器，产物入库成一份独立数据集',
        h('div', null, [
          h('div', { class: 'sim-fire' }, [
            h('div', { class: 'sim-fire-row' }, [
              h('span', { class: 'sim-fire-lb', text: '仿真时长' }), kindSeg,
              h('span', { class: 'sim-fire-lb', text: '工况' }), faultSeg,
              h('button', { class: 'btn-sm primary', text: '发起仿真',
                            disabled: !canW || busy, onclick: fire }),
              canW ? null : h('span', { class: 'muted', style: 'font-size:12px', text: '只读角色' }),
            ]),
            specLine, warnLine,
          ]),
          h('p', { class: 'muted', style: 'font-size:12px;margin:10px 0 0', text: engNote }),
        ])),
      pollHost,
      card('数据源说明', '平台不假装接了什么', h('div', { class: 'sim-src' }, [
        h('div', null, [badge('仿真模式', 'b-info'),
          h('span', { text: ' 数据来自本次仿真器批量跑出的产物（10/ sim_demo.exe），入库后即固定不变，不会自己增长。' })]),
        h('div', null, [badge('运行模式', 'b-ok'),
          h('span', { text: ' 数据来自实时引擎（10/ sim_live.exe）按墙钟逐拍产生并入库，界面随之刷新。' })]),
        h('div', null, [badge('都不是真机', 'b-warn'),
          h('span', { text: ' 两个模式当前跑的都是模型。接真机要走 07/ main_field.exe --device modbus，'
            + '且需先完成接入核对与现场确认；本版本不提供界面入口。' })]),
      ])),
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
  function pointAccess(devices, ctx) {
    const canWrite = ['operator', 'admin'].includes(ctx.user && ctx.user.role);
    const st = { id: (devices[0] || {}).device_id || null, pm: null, rows: [], names: [] };
    const root = h('div');
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
    function download(url, name) {
      const a = document.createElement('a');
      a.href = url; a.download = name;
      document.body.appendChild(a); a.click(); a.remove();
    }

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
          host: $('#pm-host').value.trim(),
          port: Number($('#pm-port').value),
          unit_id: Number($('#pm-unit').value),
          poll_period_ms: Number($('#pm-poll').value),
          timeout_ms: Number($('#pm-timeout').value),
          enabled: $('#pm-enabled').checked,
          note: $('#pm-note').value.trim(),
        });
        st.pm.conn = res.conn;
        // ★ 提示语必须说清"落盘了没有、落哪儿了"：这条链路的价值就在于
        //   13/ 07/ 是**从文件**读参数的。只说"已保存"，用户以为只是写进了库，
        //   于是仍然手敲 --host —— 那正是这个功能要消灭的重复真相。
        const cf = res.conn_file || {};
        toast(`接入参数已保存（${res.conn.host || '未填地址'}:${res.conn.port}，`
              + `从站 ${res.conn.unit_id}，${res.conn.enabled ? '已启用' : '未启用'}）`
              + (cf.removed
                  ? '；地址已清空，端侧配置文件同时删除'
                  : `${cf.path ? `；已落盘 → ${cf.path}` : ''}（13/ 07/ 启动时读它）`),
              'ok');
      } catch (e) { toast(e.message, 'bad'); }
    }

    /* ---------------- 主渲染 ---------------- */
    async function load() {
      clear(root);
      if (!st.id) {
        root.appendChild(h('div', { class: 'empty', text: '还没有设备，请先新增一台。' }));
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

      const newId = h('input', { class: 'pm-in', type: 'text',
                                 placeholder: '新设备号', style: 'width:130px' });
      const newType = h('select', { class: 'pm-in' }, ['PCS', 'BMS', 'METER', 'PV',
        'TRANSFORMER', 'LOAD', 'GATEWAY'].map(x => h('option', { value: x, text: x })));
      const addBtn = h('button', { class: 'btn-sm', text: '新增设备', disabled: !canWrite });
      addBtn.addEventListener('click', async () => {
        try {
          const r = await API.createDevice({ device_id: newId.value.trim(), device_type: newType.value });
          toast(`设备 ${r.device.device_id} 已创建`, 'ok');
          devices.push(r.device);
          st.id = r.device.device_id;
          await load();
        } catch (e) { toast(e.message, 'bad'); }
      });

      const head = h('div', { class: 'pm-bar' }, [
        h('span', { class: 'muted', text: '设备' }), picker,
        badge(pm.configured ? '已配置点表' : '未配置（回落内置默认表）',
              pm.configured ? 'b-ok' : 'b-warn'),
        pm.configured ? badge(pm.synced ? '盘上文件与库内一致' : '盘上文件与库内不一致',
                              pm.synced ? 'b-ok' : 'b-bad') : null,
        h('span', { class: 'muted', text: `${sm.rows || 0} 点 · 全表扫描 ${sm.requests || 0} 次请求` }),
        h('span', { class: 'pm-grow' }),
        h('span', { class: 'muted', text: '新增' }), newId, newType, addBtn,
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

      const connCard = card('通信参数', 'EMS 作为 Modbus 主站', h('div', null, [
        h('div', { class: 'pm-fields' }, [
          field('设备地址（IPv4 / 主机名）', txt('pm-host', c.host, '192.168.1.20', 170)),
          field('端口', numf('pm-port', c.port, 1, 65535)),
          field('从站号', numf('pm-unit', c.unit_id, 1, 247)),
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
          (c.host
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
          toast(`已导入 ${parsed.length} 行到编辑区（**尚未保存**，点「保存点表」才落盘）`, 'ok');
        };
        rd.readAsText(f, 'utf-8');
        fileInput.value = '';
      });

      const tool = h('div', { class: 'pm-bar' }, [
        (() => {
          const b = h('button', { class: 'btn-sm', text: '下载模板' });
          b.title = '内置默认表，可直接当模板改';
          b.addEventListener('click', () => download(API.pointMapCsvUrl(st.id, true),
                                                     `point-map_template.csv`));
          return b;
        })(),
        (() => {
          const b = h('button', { class: 'btn-sm', text: '导出当前表' });
          b.addEventListener('click', () => download(API.pointMapCsvUrl(st.id),
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
        '点表', `共 ${st.rows.length} 行`, h('div', null, [
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
      ctx.setSub('台账 + 设备接入（点表在界面上配置，写入约定路径供 13/ 现场进程读取）');
    }

    return { node: root, load, setDevices: (ds) => { devices = ds; } };
  }

  async function devices(ctx) {
    ctx.setSub('设备台账 + 设备接入配置');
    const d = await API.devices();
    const rt = await API.realtime(ctx.scenario);
    const byId = Object.fromEntries(rt.items.map(x => [x.device_id, x]));

    const access = pointAccess(d.items.slice(), ctx);
    await access.load();

    return h('div', { class: 'grid' }, [
      card('台账', `共 ${d.count} 台`, table([
        { title: '设备号', key: 'device_id', mono: true },
        { title: '类型', key: 'device_type' },
        { title: '额定功率', num: true, render: r => r.rated_power_kw === null ? '—' : fmt.num(r.rated_power_kw, 1) + ' kW' },
        { title: '额定容量', num: true, render: r => r.capacity_kwh === null ? '—' : fmt.num(r.capacity_kwh, 1) + ' kWh' },
        { title: '状态', render: r => stateBadge((r.status || '').toUpperCase()) },
        { title: '当前功率', num: true, render: r => { const x = byId[r.device_id]; return x && x.power_kw !== null ? fmt.num(x.power_kw, 2) + ' kW' : '—'; } },
        { title: '说明', key: 'note' },
      ], d.items, { wrap: false })),
      access.node,
      card('点表口径', null, h('div', { style: 'font-size:12.5px;line-height:1.9' }, [
        h('p', { style: 'margin:0 0 8px', html:
          '点名与索引取自 <span class="mono">07/src/rtdb/ems_point_table.h</span>，' +
          '<strong>本项目不另抄一份</strong>。' }),
        h('p', { class: 'muted', style: 'margin:0', html:
          '本项目曾因手抄品质常量出过事故（<span class="mono">P3/docs/README.md §7.1</span>：' +
          '值全对、测试全绿，只是调度画面上 24 个点全灰）。' }),
      ])),
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

  return { overview, realtime, curves, alarms, strategies, economics, report, sim,
           devices, users };
})();
