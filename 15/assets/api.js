/* =====================================================================
   15/ — 平台接口客户端
   与 14/ 的 REST 接口一一对应。零依赖（fetch + localStorage）。
   ===================================================================== */

const API = (() => {
  const TOKEN_KEY = 'ems_platform_token';
  const USER_KEY  = 'ems_platform_user';

  let token = localStorage.getItem(TOKEN_KEY) || '';
  let user  = null;
  try { user = JSON.parse(localStorage.getItem(USER_KEY) || 'null'); } catch (e) { user = null; }

  // 登录失效时的回调（由 app.js 注入，避免这里依赖 DOM）
  let onUnauthorized = null;
  const setUnauthorizedHandler = (fn) => { onUnauthorized = fn; };

  function setSession(t, u) {
    token = t || '';
    user  = u || null;
    if (token) localStorage.setItem(TOKEN_KEY, token);
    else localStorage.removeItem(TOKEN_KEY);
    if (user) localStorage.setItem(USER_KEY, JSON.stringify(user));
    else localStorage.removeItem(USER_KEY);
  }

  function url(path, params) {
    const u = new URL(path, window.location.origin);
    if (params) {
      Object.entries(params).forEach(([k, v]) => {
        if (v !== undefined && v !== null && v !== '') u.searchParams.set(k, v);
      });
    }
    if (token) u.searchParams.set('token', token);
    return u.toString();
  }

  async function call(method, path, { body, params } = {}) {
    const opt = { method, headers: {} };
    if (body !== undefined) {
      opt.headers['Content-Type'] = 'application/json';
      opt.body = JSON.stringify(body);
    }
    const res = await fetch(url(path, params), opt);
    let data = null;
    const ct = res.headers.get('content-type') || '';
    if (ct.includes('application/json')) {
      data = await res.json();
    } else {
      data = await res.text();
    }
    if (res.status === 401) {
      setSession('', null);
      if (onUnauthorized) onUnauthorized(data && data.error ? data.error : '会话已过期');
      const e401 = new Error((data && data.error) || '未登录');
      e401.status = 401;
      throw e401;
    }
    if (!res.ok) {
      // ★ 把 HTTP 状态挂到错误对象上（2026-10-16 补）。
      //   调用方必须能区分「这一刻没有数据（404）」与「真的坏了（5xx）」：
      //   两者该说的话完全不同 —— 前者要告诉用户"去哪拿数据"，后者才是"加载失败"。
      //   没有 status 时前端只能按 error 文案猜，而后端文案是自由文本，
      //   改一个字判断就错 —— 这正是本项目反复踩过的"靠字符串当契约"。
      const err = new Error((data && data.error) || `HTTP ${res.status}`);
      err.status = res.status;
      throw err;
    }
    return data;
  }

  const get  = (path, params) => call('GET', path, { params });
  const post = (path, body)   => call('POST', path, { body });
  const put  = (path, body)   => call('PUT', path, { body });
  const del  = (path)         => call('DELETE', path);

  // ---- 具体接口 ----
  return {
    setUnauthorizedHandler,
    setSession,
    get token() { return token; },
    get user()  { return user; },

    health:          ()          => get('/api/health'),
    login:           (u, p)      => post('/api/login', { username: u, password: p }),
    logout:          ()          => post('/api/logout', { token }),
    me:              ()          => get('/api/me'),

    overview:        (sc, t)     => get('/api/overview',  { scenario: sc, t }),
    realtime:        (sc, t)     => get('/api/realtime',  { scenario: sc, t }),
    series:          (sc, keys, points) =>
                                    get('/api/series', { scenario: sc, keys: (keys || []).join(','), points }),
    steps:           (sc, p)     => get('/api/steps',     Object.assign({ scenario: sc }, p)),
    commands:        (sc, p)     => get('/api/commands',  Object.assign({ scenario: sc }, p)),
    alarms:          (sc, p)     => get('/api/alarms',    Object.assign({ scenario: sc }, p)),
    // 交叉筛选：传 level / source 时，各维度的计数会带上**另一个**维度的筛选，
    // 但不带自己那份（否则选中 FAULT 后 WARNING 变 0，就再也点不回去了）。
    // 与后端 h_alarm_summary 的 _where() 同一口径。
    alarmSummary:    (sc, p)     => get('/api/alarms/summary', Object.assign({ scenario: sc }, p)),

    devices:         ()          => get('/api/devices'),

    // ---- 接入装配（v1.5）：上端 = 调度侧 IEC104 从站；下端 = 设备侧 Modbus 主站 ----
    access:          ()          => get('/api/access'),
    saveUpstream:    (body)      => put('/api/access/upstream', body),
    // 上端第二份输入：IOA 转发表（v1.6 起**可保存**：平台校验后原子落盘到
    // config/point-map/active.pointmap.csv，网关启动时按约定路径读它）
    upstreamPointMap:()          => get('/api/upstream/point-map'),
    saveUpstreamPointMap:(rows)  => put('/api/upstream/point-map', { rows }),
    resetUpstreamPointMap:()     => del('/api/upstream/point-map'),
    upstreamPointMapCsvUrl:(tpl) => url('/api/upstream/point-map.csv',
                                        tpl ? { template: 1 } : undefined),

    // ---- 「接入调度主站」九步向导（v1.14） ----
    // ★ 字段定义（哪步有哪些格、哪些必填）由后端 GET 一起返回，前端**不写死**。
    //   前端只按 schema 画；完成校验也只由后端判，前端把 400 的消息原样给它看。
    //   理由见 CHANGES.md §94：同一份清单两处分头维护，迟早不一致。
    accessDraft:     ()          => get('/api/access-draft'),
    saveAccessDraft: (body)      => put('/api/access-draft', body),
    resetAccessDraft:()          => del('/api/access-draft'),
    /* step = 第几步：带上是「本步那份」，不带是九步总台账。
       每一份的文件名与列都由后端 export_step 决定，前端不拼。 */
    accessDraftCsvUrl:(step)      => url('/api/access-draft.csv'
                                          + (step ? ('?step=' + step) : '')),
    device:          (id)        => get(`/api/devices/${encodeURIComponent(id)}`),

    // ---- 设备接入（schema v1.2）：通信参数 + 点表 ----
    // 点表保存是"先校验 → 落盘 → 写库"，任一步失败服务端整体回滚，
    // 所以前端只需处理"成功 / 报错"两种结果，不必自己补偿。
    createDevice:    (body)      => post('/api/devices', body),
    pointMap:        (id)        => get(`/api/devices/${encodeURIComponent(id)}/point-map`),
    savePointMap:    (id, rows)  => put(`/api/devices/${encodeURIComponent(id)}/point-map`, { rows }),
    saveConn:        (id, body)  => put(`/api/devices/${encodeURIComponent(id)}/conn`, body),
    // CSV 导出走直链（带 token 查询参数），交给浏览器下载，不经 fetch
    pointMapCsvUrl:  (id, tpl)   => url(`/api/devices/${encodeURIComponent(id)}/point-map.csv`,
                                        tpl ? { template: 1 } : undefined),

    strategies:      ()          => get('/api/strategies'),
    updateStrategy:  (id, body)  => put(`/api/strategies/${encodeURIComponent(id)}`, body),

    modes:           ()          => get('/api/modes'),
    activateMode:    (modeId)    => post('/api/modes/activate', { mode_id: modeId }),

    // ---- 引擎双模式（schema v1.4）：运行模式 = 实时引擎；仿真模式 = 离线批量仿真 ----
    // GET /api/engine 是"当前引擎模式 + 实时引擎状态 + 可选数据源"的唯一出处，
    // 也是界面上"现在跑的是模型、不是真机"这句话的出处（sources[].available/why）。
    engine:          ()          => get('/api/engine'),
    setEngine:       (eng)       => post('/api/engine', { engine: eng }),
    liveStart:       (body)      => post('/api/engine/live/start', body || {}),
    liveStop:        ()          => post('/api/engine/live/stop', {}),
    liveReset:       ()          => post('/api/engine/live/reset', {}),

    // ---- 数据源（设备管理页唯一的开关）----
    // 一次决定两端：模拟数据源 = 下端设备替身 + 上端模拟调度；
    // 现场数据源 = 下端真实设备 + 上端上级调度。
    // 其他页面**不调用**这两个接口 —— 它们只认"有没有数据"。
    dataSource:      ()          => get('/api/data-source'),
    // 两端是各自独立的开关：一次只切一端（end 必填，后端会拒掉没带 end 的请求）
    setDataSource:   (end, kind) => post('/api/data-source', { end, kind }),
    // 模拟源**总开关**（v1.7）：一个开关管两端 + 一键换一套。
    //   enable=true  两端一起切 sim，并把①的模拟专用字段按当前套填好
    //   enable=false 两端一起切回 real（真实值一直在，不会被清）
    //   rotate=true  套号 +1（循环）
    // ★ 与 setDataSource 的分工：那个是"逐端微调"，这个是"一键进入联调态"。
    simSource:       ()          => get('/api/sim-source'),
    setSimSource:    (body)      => post('/api/sim-source', body),
    // ★ 进程与配置分开两条通道：simProc 改的是"谁在跑"，setSimSource 改的是"表"。
    //   合成一条就会重演「点了启用、表变了、进程没变、页面还是空的」。
    simProc:         (body)      => post('/api/sim-source/proc', body),

    simRuns:         (limit)     => get('/api/sim/runs', { limit }),
    simStart:        (body)      => post('/api/sim/runs', body),
    simRun:          (id)        => get(`/api/sim/runs/${encodeURIComponent(id)}`),
    simDelete:       (id)        => del(`/api/sim/runs/${encodeURIComponent(id)}`),

    economics:       ()          => get('/api/economics'),
    energy:          ()          => get('/api/energy'),
    report:          (sc, period)=> get('/api/report', { scenario: sc, period }),
    invariants:      ()          => get('/api/invariants'),

    sendCommand:     (body)      => post('/api/control/command', body),

    audit:           (limit)     => get('/api/audit', { limit }),
    users:           ()          => get('/api/users'),
    createUser:      (body)      => post('/api/users', body),

    exportUrl:       (kind, sc)  => url(`/api/export/${kind}`, { scenario: sc }),
    download:        (kind, sc)  => {
      const a = document.createElement('a');
      a.href = API.exportUrl(kind, sc);
      a.download = `${kind}_${sc}.csv`;
      document.body.appendChild(a);
      a.click();
      a.remove();
    },
  };
})();
