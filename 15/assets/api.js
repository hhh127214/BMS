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
      throw new Error((data && data.error) || '未登录');
    }
    if (!res.ok) {
      throw new Error((data && data.error) || `HTTP ${res.status}`);
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
    alarmSummary:    (sc)        => get('/api/alarms/summary', { scenario: sc }),

    devices:         ()          => get('/api/devices'),
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
