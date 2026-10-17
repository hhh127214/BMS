/* =====================================================================
   15/ 平台界面自检（Node 静态契约校验，零依赖）

   为什么要静态自检：15/ 是纯前端，没有编译期检查。它唯一的「编译」
   是浏览器加载。所以这里把三类最容易静默出错的东西做成断言：

     A 文件齐备    —— 引用的文件在不在、顺序对不对、能不能离线打开
     B 接口契约    —— 前端调的每个 /api/... 后端必须真有（14/src/api.py）
     C 页面与权限  —— 菜单 / 页面函数 / 角色门槛 三者必须一致

   另加 D 渲染纪律（不做危险的 DOM 注入）与 E 图表函数齐备。

   ★ [L] 脚本语法守卫（2026-10-11 补）：上面那些全是**文本 / 正则**检查，
     没有一条会真的解析 JS。而一个多余的括号就能让整个 .js 加载失败 ——
     症状是登录按钮没反应、页面直接刷新，界面上**没有任何报错**。
     所以最后一段用 `node --check` 逐个解析（只解析不执行）。

   跑法：
     node 15/tests/selftest.js
   退出码 0 = 全过。
   ===================================================================== */

'use strict';

const fs = require('fs');
const path = require('path');

const ROOT15 = path.resolve(__dirname, '..');
const ROOT = path.resolve(ROOT15, '..');
const P14 = path.join(ROOT, '14', 'src', 'api.py');

/* ------------------------------ 断言器 ------------------------------ */
class T {
  constructor() { this.pass = 0; this.fail = 0; this.section = ''; }
  head(name) { this.section = name; console.log(`\n=== ${name} ===`); }
  ok(cond, label) {
    if (cond) { this.pass++; console.log(`  [ok]   ${label}`); }
    else { this.fail++; console.log(`  [FAIL] ${label}`); }
    return !!cond;
  }
  eq(a, b, label) {
    const same = JSON.stringify(a) === JSON.stringify(b);
    if (!same) console.log(`         期望 ${JSON.stringify(b)}，实际 ${JSON.stringify(a)}`);
    return this.ok(same, label);
  }
  ge(a, b, label) { return this.ok(a >= b, `${label}（${a} >= ${b}）`); }
  report() {
    console.log('\n----- 断言统计 -----');
    console.log(`  通过 ${this.pass} / 失败 ${this.fail}`);
    if (this.fail === 0) console.log('\n=== ALL TESTS PASSED ===');
    else console.log('\n=== TESTS FAILED ===');
    return this.fail === 0;
  }
}

const t = new T();

function read(p) {
  if (!fs.existsSync(p)) throw new Error(`文件不存在：${p}`);
  return fs.readFileSync(p, 'utf8');
}

/* =====================================================================
   A 文件齐备
   ===================================================================== */
t.head('A 文件齐备');

const FILES = [
  'index.html',
  'assets/app.css',
  'assets/api.js',
  'assets/ui.js',
  'assets/assembly.js',   // 装配视图数据（派生产物，由 J 段守它与 CSV 的一致性）
  'assets/pages.js',
  'assets/app.js',
];
FILES.forEach(f => t.ok(fs.existsSync(path.join(ROOT15, f)), `存在 ${f}`));

const indexHtml = read(path.join(ROOT15, 'index.html'));
const css = read(path.join(ROOT15, 'assets/app.css'));
const apiJs = read(path.join(ROOT15, 'assets/api.js'));
const uiJs = read(path.join(ROOT15, 'assets/ui.js'));
const asmJs = read(path.join(ROOT15, 'assets/assembly.js'));
const pagesJs = read(path.join(ROOT15, 'assets/pages.js'));
const appJs = read(path.join(ROOT15, 'assets/app.js'));

/* 取一个顶层函数的源码片段：从它的头部到下一个顶层函数声明（或下一个顶层块注释）为止。
   ★ 用来做"这个函数里必须/不许出现什么"的断言。
   ★ 必须同时认 `function` 与 `async function` —— 只认前者时，若后面紧跟的是
     `async function renderPage`，切片会一路吃到文件末尾，把**别处**的字符串也算进来
     （2026-10-16 加 [M] 段时因此误判过一条否定断言：空状态里本来没有"加载失败"，
      却因为切到了 renderPage 的兜底文案而报红）。 */
const sliceFn = (src, header) => {
  const i = src.indexOf(header);
  if (i < 0) return '';
  const cuts = [
    src.indexOf('\n  function ', i + header.length),
    src.indexOf('\n  async function ', i + header.length),
    src.indexOf('\n  /* ', i + header.length),
  ].filter(k => k > 0);
  return src.slice(i, cuts.length ? Math.min(...cuts) : src.length);
};

/* 静态扫描用的"裸源码"（剥掉注释）。
   ★ 为什么必须有这一层：本文大量断言是"源码里出没出现某个写法"，而正则不认注释 ——
     注释里提到 `<marker>`、`html:` 都会被当成一处真实使用点，报出来的却是
     "ui.js 又用了 <marker>"、"渲染纪律有问题"这类吓人的说法，纯误导。
     （2026-10-08 加装配视图时连踩两次：ui.js 注释里的 <marker>、pages.js 注释里的 html:）
   注：剥注释只会让"不应该出现 X"这类**否定**断言更宽松（注释不再误伤），
   不会放过真代码里的 X —— 代码本体不会被剥掉。 */
const stripComments = s => s
  .replace(/\/\*[\s\S]*?\*\//g, '')     // 块注释
  .replace(/(^|\s)\/\/[^\n]*/g, '$1');  // 行注释（前导空白，避免吃掉 :// 的斜杠）
const uiCode = stripComments(uiJs);
const pagesCode = stripComments(pagesJs);

/* 引用顺序：api.js 必须先于 app.js（后者启动时要调 API），ui.js 先于 pages.js
   （后者用 h/fmt）。assembly.js 夹在中间：pages.js 的两个新页要读它的 ASSEMBLY，
   它自己不依赖任何东西。 */
const refs = [...indexHtml.matchAll(/<script\s+src="([^"]+)"><\/script>/g)].map(m => m[1]);
t.eq(refs, ['assets/api.js', 'assets/ui.js', 'assets/assembly.js',
            'assets/pages.js', 'assets/app.js'], '脚本引用顺序正确');

const cssRefs = [...indexHtml.matchAll(/<link\s+[^>]*href="([^"]+)"/g)].map(m => m[1]);
t.eq(cssRefs, ['assets/app.css'], '样式表引用正确');

// 离线可用：不允许任何外部主机
const externals = [...(indexHtml + css + apiJs + uiJs + asmJs + pagesJs + appJs)
  .matchAll(/https?:\/\/[^\s"'<>)]+/g)]
  .map(m => m[0])
  .filter(u => !/127\.0\.0\.1|localhost/.test(u));
t.eq(externals, [], '无外部资源引用（离线可开）');

t.ok(/lang="zh-CN"/.test(indexHtml), 'html lang 为 zh-CN');
t.ok(/<meta charset="utf-8">/.test(indexHtml), '声明 utf-8');
t.ok(/viewport/.test(indexHtml), '声明 viewport');
t.ok(/<title>[^<]+<\/title>/.test(indexHtml), '有标题');

// 三个必需的挂载点
['id="login-view"', 'id="app-view"', 'id="content"'].forEach(k =>
  t.ok(indexHtml.includes(k), `挂载点 ${k}`));

/* =====================================================================
   B 接口契约：前端调的接口，后端必须真有
   ===================================================================== */
t.head('B 接口契约（对 14/src/api.py）');

t.ok(fs.existsSync(P14), '后端路由表可读 14/src/api.py');
const apiPy = fs.existsSync(P14) ? read(P14) : '';

// 后端 ROUTES：("GET", re.compile(r"/api/health"), ...)
const backRoutes = [];
for (const line of apiPy.split('\n')) {
  const m = line.match(/re\.compile\(r"(\/api[^"]*)"\)/);
  if (m) backRoutes.push(m[1]);
}
t.ge(backRoutes.length, 24, '后端路由条数');
t.ok(backRoutes.includes('/api/engine'), '后端提供 GET /api/engine');
t.ok(backRoutes.includes('/api/engine/live/start'), '后端提供 /api/engine/live/start');
t.ok(backRoutes.includes('/api/engine/live/stop'), '后端提供 /api/engine/live/stop');
t.ok(backRoutes.includes('/api/engine/live/reset'), '后端提供 /api/engine/live/reset');
t.ok(backRoutes.includes('/api/sim/runs'), '后端提供 /api/sim/runs');
t.ok(backRoutes.includes('/api/sim/runs/(?P<run_id>[\\w\\-]+)'),
  '后端提供 /api/sim/runs/{run_id}');

// 前端调用点：'/api/xxx' 或 `/api/xxx/${...}`
const frontPaths = new Set();
for (const m of apiJs.matchAll(/[`'"'](\/api\/[^`'"']*)[`'"]/g)) {
  frontPaths.add(m[1].replace(/\$\{[^}]*\}/g, '\u0001'));
}
const frontList = [...frontPaths].sort();
t.ge(frontList.length, 20, '前端接口调用点');

// 后端路由：Python 命名组 (?P<name>..) → JS (?<name>..)，并锚定整串（等价 fullmatch）
const backRe = backRoutes.map(r =>
  new RegExp('^' + r.replace(/\(\?P</g, '(?<') + '$'));
const missing = frontList
  .map(p => ({ src: p, probe: p.replace(/\u0001/g, 'X') }))
  .filter(x => !backRe.some(r => r.test(x.probe)))
  .map(x => x.src);
t.eq(missing, [], '前端调用的接口后端全部存在');

// 关键接口逐个点名（防止正则提取漏掉而被误判为通过）
[
  '/api/health', '/api/login', '/api/logout', '/api/me', '/api/overview',
  '/api/realtime', '/api/series', '/api/steps', '/api/commands',
  '/api/alarms', '/api/alarms/summary', '/api/strategies', '/api/economics',
  '/api/energy', '/api/report', '/api/invariants', '/api/control/command',
  '/api/audit', '/api/users', '/api/devices',
  '/api/engine', '/api/sim/runs',
].forEach(p => t.ok(apiJs.includes(`'${p}'`), `前端声明了 ${p}`));

// 后端有 /api/energy-flow（overview 的语义化别名，同一 handler），
// 前端不单独调它 —— 能量流四个量已随 overview 一次返回。这里两个都验证。
t.ok(backRoutes.includes('/api/energy-flow'), '后端提供 /api/energy-flow');
t.ok(/energyFlow\(\{[\s\S]{0,200}?pv_kw[\s\S]{0,200}?load_kw[\s\S]{0,200}?grid_kw[\s\S]{0,200}?battery_kw/.test(pagesJs),
  '前端用 overview 的四个功率量喂能量流图');
t.ok(/\/api\/export\//.test(apiJs), '前端覆盖 /api/export/');

/* =====================================================================
   C 页面与权限一致性
   ===================================================================== */
t.head('C 页面与权限一致性');

// MENU 项：{ id: 'overview', ... render: Pages.overview ... }
// ★ 必须**只在 MENU 数组里**取 id：app.js 里其它结构也有一模一样的
//   `id: 'xxx', name:` 形状（引擎分段的 run/sim、数据集条目的 live），
//   全文扫会把它们当菜单项抓进来，然后报出一串"缺图标"的假失败。
const MENU_BLOCK = (appJs.match(/const MENU = \[([\s\S]*?)\n\];/) || ['', ''])[1];
const menuIds = [...MENU_BLOCK.matchAll(/id:\s*'([a-z]+)',\s*name:/g)].map(m => m[1]);
t.eq(menuIds.length, 12, '菜单项数 = 12（v1.5 新增「装配视图」「一次接线图」两页）');
t.ok(new Set(menuIds).size === menuIds.length, '菜单 id 不重复');
t.ok(menuIds.includes('sim'), '菜单含「仿真」页');
t.ok(menuIds.includes('structure'), '菜单含「装配视图」页');
t.ok(menuIds.includes('wiring'), '菜单含「一次接线图」页');
// 两页的 render 必须指向 Pages 里的同名函数（不是占位函数）
t.ok(/render:\s*Pages\.structure/.test(appJs), '装配视图 render 指向 Pages.structure');
t.ok(/render:\s*Pages\.wiring/.test(appJs), '一次接线图 render 指向 Pages.wiring');

// Pages 导出键：取 IIFE 收尾那句 return { a, b, ... };})();
const pagesTail = pagesJs.match(/return \{ ([a-z0-9_,\s]+) \};\s*\}\)\(\);\s*$/m);
const pageKeys = pagesTail
  ? pagesTail[1].split(',').map(s => s.trim()).filter(Boolean)
  : [];
t.ge(pageKeys.length, 12, 'Pages 导出页面数');
t.eq([...pageKeys].sort(), [...menuIds].sort(), '菜单项与页面函数一一对应');

// 每个菜单 id 有图标
const iconKeys = [...appJs.matchAll(/^\s{2}([a-z]+):\s+'<path|^\s{2}([a-z]+):\s+'<circle|^\s{2}([a-z]+):\s+'<rect/gm)]
  .map(m => m[1] || m[2] || m[3]);
menuIds.forEach(id => t.ok(iconKeys.includes(id), `图标 ICONS.${id}`));

// 角色门槛：users 仅 admin
t.ok(/id:\s*'users'[^}]*minRole:\s*'admin'/.test(appJs), "菜单 users 限 admin");
t.ok(/const ROLE_LEVEL = \{ viewer: 1, operator: 2, admin: 3 \}/.test(appJs),
  '前端角色等级与后端一致（viewer<operator<admin）');
t.ok(/visibleMenu\(\)/.test(appJs), '菜单按角色过滤可见项');
t.ok(/API\.users\(\)/.test(pagesJs), 'users 页真的去拉用户列表（不是假表格）');

// 后端权限口径对照：写接口 operator、用户管理 admin
t.ok(/re\.compile\(r"\/api\/control\/command"\),\s*h_command_send,\s*"operator"/.test(apiPy),
  '后端 control/command ≥ operator');
t.ok(/re\.compile\(r"\/api\/users"\),\s*h_users,\s*"admin"/.test(apiPy),
  '后端 users(读) = admin');
t.ok(/h_user_create,\s*"admin"/.test(apiPy), '后端 users(建) = admin');

/* =====================================================================
   D 渲染纪律：不把数据拼进 innerHTML
   ===================================================================== */
t.head('D 渲染纪律');

/* 本段的统计一律扫 pagesCode（剥注释后的源码），理由见文件顶部 stripComments 处。 */
t.ok(/td\.textContent = /.test(uiJs), 'table() 单元格走 textContent');
t.ok(/else if \(k === 'text'\) el\.textContent = v;/.test(uiJs), "h(text:) 走 textContent");
t.ok(/document\.createTextNode/.test(uiJs), '子节点走 createTextNode');

// innerHTML 只允许出现在明确白名单的 5 处：h() 的 html 分支 + pages.js 四个图表容器
// （同样扫剥掉注释后的源码，注释里提到 innerHTML 不该被算成一处使用点）
const innerHtmlPages = (pagesCode.match(/innerHTML/g) || []).length;
const innerHtmlUi = (uiJs.match(/innerHTML/g) || []).length;
t.eq(innerHtmlUi, 1, 'ui.js 仅 1 处 innerHTML（h() 的 html 分支）');
t.eq(innerHtmlPages, 4, 'pages.js 仅 4 处 innerHTML（图表容器）');

// 这四处必须是图表函数或静态文案，不能直接塞接口返回的字符串
const innerTargets = [...pagesCode.matchAll(/innerHTML\s*=\s*(.{0,40})/g)].map(m => m[1]);
t.ok(innerTargets.every(s => /^(lineChart\(|energyFlow\(|'<div class="empty">)/.test(s)),
  'innerHTML 目标均为自绘 SVG 或静态文案');

// html: 的实参必须是字符串字面量（不含来自响应的变量插值）
const htmlArgs = [
  ...pagesCode.matchAll(/html:\s*\n?\s*(.{0,60})/g),
].map(m => m[1]);
t.ge(htmlArgs.length, 4, 'html: 使用点已枚举');
t.ok(htmlArgs.every(s => /^[`'"]/.test(s) || /^[A-Za-z]+\(\{/.test(s)),
  'html: 实参为字面量或图表函数');

// 无 eval / new Function
[['ui.js', uiJs], ['pages.js', pagesJs], ['app.js', appJs], ['api.js', apiJs]]
  .forEach(([n, s]) => {
    t.ok(!/\beval\s*\(/.test(s), `${n} 无 eval`);
    t.ok(!/new Function/.test(s), `${n} 无 new Function`);
  });

// 口令不落 localStorage
t.ok(!/localStorage\.setItem\([^)]*pass/i.test(apiJs + appJs), '口令不写入 localStorage');

/* =====================================================================
   E 图表与样式齐备
   ===================================================================== */
t.head('E 图表与样式');

const FN_DECL = ['h', '$', '$$', 'clear', 'badge', 'stateBadge', 'levelBadge',
  'resultBadge', 'toast', 'card', 'kpi', 'table', 'lineChart', 'chartLegend',
  'energyFlow', 'assemblyView', 'wiringDiagram', 'barChart', 'ratioRow'];
FN_DECL.forEach(fn => {
  const esc = fn.replace(/\$/g, '\\$');
  t.ok(new RegExp(`function ${esc}\\s*\\(`).test(uiJs), `ui.js 定义 ${fn}()`);
});
// fmt 是命名空间对象（方法简写 num(v, nd) {...}），不是独立函数
t.ok(/const fmt = \{/.test(uiJs), 'ui.js 定义 fmt 命名空间');
['num', 'signed', 'kw', 'kwh', 'pct', 'soc', 'money', 'hhmm', 'int'].forEach(k =>
  t.ok(new RegExp(`\\n\\s{2}${k}\\s*\\(`).test(uiJs), `fmt.${k}()`));

t.ok(/<svg viewBox="0 0 \$\{W\} \$\{H\}"/.test(uiJs), '图表输出内联 SVG（不引图表库）');
t.ok(/class="chart"/.test(uiJs), 'SVG 带 .chart 样式钩子');

['--c-load', '--c-pv', '--c-grid', '--c-bat'].forEach(v =>
  t.ok(css.includes(v), `CSS 变量 ${v}`));

// 中国股市/仪表配色约定：负荷蓝、光伏黄、电网橙、电池绿，四色互不相同
const colorVals = ['--c-load', '--c-pv', '--c-grid', '--c-bat'].map(v => {
  const m = css.match(new RegExp(`${v}:\\s*([^;]+);`));
  return m ? m[1].trim() : '';
});
t.ok(new Set(colorVals).size === 4, '四个量测色互不相同');

// 深色工业主题与中文文案
t.ok(/body\s*\{[^}]*background/.test(css), 'body 有背景色');
t.ok(/[\u4e00-\u9fa5]/.test(css), 'CSS 注释为中文（与全仓一致）');

/* ★ 用过的 CSS 变量必须在 app.css 里真的定义过。
   为什么值得单独一条断言：var(--没定义) **不报错** —— 它在"计算值阶段"失效，
   浏览器当成这个属性没写（fill 退回继承值、背景色退回透明）。界面照常画出来，
   只是颜色不对，看的人只会以为"设计就这样"。新画一张图时顺手写了个不存在的
   变量名，是这类失效最常见的入口 —— 装配视图第一版就踩了（--txt/--mute/--bg）。 */
const cssDefined = new Set([...css.matchAll(/^\s*(--[\w-]+)\s*:/gm)].map(m => m[1]));
const cssUsed = new Set();
[uiJs, pagesJs, asmJs].forEach(src => {
  [...src.matchAll(/var\((--[\w-]+)\)/g)].forEach(m => cssUsed.add(m[1]));
});
const cssMissing = [...cssUsed].filter(v => !cssDefined.has(v));
t.eq(cssMissing, [], `用到的 CSS 变量全部有定义（共 ${cssUsed.size} 个，缺 ${cssMissing.length} 个）`);

// 新增页面用到的样式钩子
[['lgd', '图例容器'], ['sw', '图例色块'], ['kpi-v', '部件详情大数值']].forEach(([cls, what]) => {
  t.ok(new RegExp(`\\.${cls}\\s*\\{`).test(css), `CSS 定义 .${cls}（${what}）`);
});

/* =====================================================================
   F 与后端的口径一致
   ===================================================================== */
t.head('F 与后端口径一致');

t.ok(/ems_platform_token/.test(apiJs), 'token 键名固定');
t.ok(/res\.status === 401/.test(apiJs), '401 统一走重新登录');
t.ok(/setUnauthorizedHandler/.test(apiJs) && /setUnauthorizedHandler/.test(appJs),
  '登录失效回调已接线');
t.ok(/scenario: sc/.test(apiJs), '接口统一带 scenario 参数');

// v1.4：默认数据集不再硬编码 —— 由引擎模式决定（run→live，sim→最近成功仿真）。
// 前端必须：
//   ① 允许 scenario 为空（交给后端 resolve_scenario 解析）
//   ② 从 /api/engine 的 scenario 字段取默认值
//   ③ 数据集分段按钮按引擎模式动态生成（不再写死在 index.html 里）
t.ok(!/scenario:\s*'normal'/.test(appJs), '前端不硬编码默认场景 normal');
t.ok(/engineInfo\.scenario/.test(appJs) || /info\.scenario/.test(appJs),
  '前端默认数据集取自 /api/engine 的 scenario 字段');
t.ok(!/data-scenario=/.test(indexHtml), '数据集分段按钮不在 index.html 里写死（动态生成）');
t.ok(/buildScenarioSeg\(/.test(appJs) && /data-scenario/.test(appJs),
  '数据集分段按钮由 app.js 动态生成');
t.ok(/'fault'/.test(appJs), '界面仍提供故障工况数据集（内置 fault）');
t.ok(/scenarioEntry\(/.test(appJs), '内置 normal/fault 有可读短名');

// 时标口径：86400 s / 8640 拍
t.ok(/8640/.test(pagesJs + appJs + indexHtml), '界面口径提到 8640 拍');
t.ok(/86400/.test(apiPy), '后端时标上界 86400 s');

// 平台已打通真机实时（07/ --record → live 场景），但默认仍是离线仿真。
// 界面上必须如实说明两个来源，不能还写着「尚未接入真机」。
t.ok(/live/.test(pagesJs) && /07\//.test(pagesJs), '界面明示「仿真 + 现场实录」两个来源');

/* =====================================================================
   G 能量流图改版契约（2026-09-28）
     粗箭头 + 杆内方向字 + 等轴测 3D 母线 + 竖排标签；两处小圆点已移除；
     箭头体量收窄不压设备；光带方向固定为"尾→头"；回放走 patch 不重建。
     为什么值得静态断言：这几条全是"改回去了也不会有人发现"的画法 / 性能约定 ——
     老的 <marker> 小三角、侧边徽章、扁平竖条母线、菜单小蓝点、顶栏小绿点、
     反向的光带动画、每拍整张重建（动画重启 = 卡顿），少了断言，
     任何一次重排都可能把它们悄悄带回来。
   ===================================================================== */
t.head('G 能量流图改版契约');

// 粗箭头：7 点实心多边形；不再用 <marker> 拼小三角
t.ok(!/<marker/.test(uiCode), 'ui.js 不再用 <marker> 画箭头（改实心多边形）');
t.ok(/const AT = 16, AH = 14, AHH = 12;/.test(uiJs),
     '粗箭头体量为 16/14/12（够粗能读，又不压储能柜与铁塔导线）');
t.ok(/const arrow = \(x1, x2, y, color, active, label, key\)/.test(uiJs),
     'arrow() 签名含方向字 label 与 patch 钩子 key');
t.ok(/class="flow-arrow"/.test(uiJs), '箭头有 .flow-arrow 钩子');
t.ok(!/badgeText/.test(uiCode), 'node() 不再画侧边徽章（方向字已挪进箭头杆）');

// 箭头两端留白：尾端退到 Lx±gap，母线端退到 cx±(busHalf+busGap)
t.ok(/const gap = 58, busGap = 10;/.test(uiJs), 'ui.js 定义箭头两端留白 gap = 58 / busGap = 10');
t.ok(/nearL = Lx \+ gap, nearR = Rx - gap/.test(uiJs),
     '箭头尾端用 Lx±gap（不是地块顶点 Lx±52），端面不压设备插画');
t.ok(/busL = cx - busHalf - busGap, busR = cx \+ busHalf \+ busGap/.test(uiJs),
     '箭头母线端退到 cx±(busHalf+busGap)，端面不贴死柱身轮廓（顶盖/底台比柱身宽）');

// 等轴测 3D 母线 + 竖排标签
t.ok(/const sceneBus = \(c\) =>/.test(uiJs), 'ui.js 定义等轴测母线场景 sceneBus()');
t.ok(/const busLabel = \['交', '流', '母', '线'\];/.test(uiJs), '母线标签为四字数组（每行一字）');
t.ok(!/rotate\(-90/.test(uiCode), '母线标签不再旋转（改竖排、字正）');

// 两处小圆点移除
t.ok(!/id="conn-state"/.test(indexHtml), 'index.html 无顶栏连通绿点 #conn-state');
t.ok(!/class: 'dot'/.test(appJs), 'app.js 不再往菜单按钮里塞小蓝点');
t.ok(!/\.nav \.dot/.test(css) && !/\.conn\b/.test(css), 'app.css 已删 .nav .dot 与 .conn 规则');

// ---- 光带方向：一套动画，符号固定为负 ----
// 光带的 <line> 一律从箭尾 x1 画到 base（夹在箭尾与箭头之间），线的正方向
// 就等于潮流方向；所以 dashoffset 只要往负方向走，短划就是朝箭头去的。
// 曾经按"箭尾在屏幕左 / 右"分两套动画（右行取负、左行取正），左向箭头上
// 的光带因此与箭头对走 —— 这两条断言就是防它回来。
t.ok(/x1="\$\{x1\}" y1="\$\{y\}" x2="\$\{base\}" y2="\$\{y\}"/.test(uiJs),
     '光带 <line> 从箭尾 x1 画到 base（线方向 = 潮流方向）');
t.ok(!/flow-sweep-l/.test(uiCode) && !/flow-sweep-l/.test(css),
     '已无"左行"光带变体（flow-sweep-l）—— 方向不再按屏幕左右取反');
t.ok(/@keyframes flow-sweep-run \{ to \{ stroke-dashoffset: -100; \} \}/.test(css),
     '光带位移固定为负 100（沿线的尾→头推进，与箭头一致）');
{
  const ms = Number((uiJs.match(/FLOW_SWEEP_MS = (\d+)/) || [])[1]);
  const sec = Number((css.match(/flow-sweep-run ([\d.]+)s linear/) || [])[1]);
  t.eq(ms, sec * 1000, 'FLOW_SWEEP_MS 与 CSS 动画周期一致（负延迟续接才对得上相位）');
}
t.ok(/animation-delay:-\$\{\(performance\.now\(\) % FLOW_SWEEP_MS\)/.test(uiJs),
     '光带用负 animation-delay 续接相位（重建后不从 0 重来）');

// ---- 回放走 patch：结构不变就不换元素，动画不被打断 ----
t.ok(/function flowValues\(d\)/.test(uiJs), 'ui.js 抽出 flowValues()（重建与 patch 共用一份计算）');
t.ok(/function energyFlowPatch\(svg, d\)/.test(uiJs), 'ui.js 提供 energyFlowPatch()（只改数字）');
t.ok(uiJs.includes('data-k="${key}"') && /data-k="soc-bar"/.test(uiJs)
     && /data-k="soc-txt"/.test(uiJs) && /data-k="sum"/.test(uiJs),
     '主渲染区数字 / SOC 条 / 守恒小结都带 data-k（patch 才找得到节点）');
t.ok(/barW = FLOW_SOC\.w/.test(uiJs), 'SOC 条几何走 FLOW_SOC（patch 与重建同一口径）');
t.ok(/const flowSignature = \(p\) =>/.test(pagesJs) && /energyFlowPatch\(flowSvg, fargs\)/.test(pagesJs),
     'pages.js 用结构签名决定「重建 / patch」（方向没变就不重建）');
t.ok(!/clear\(flowHost\)/.test(pagesCode), 'pages.js 不再每拍清空重建能量流卡（否则光带动画重启）');
t.ok(/card\('能量流', '数值为功率大小 · 方向见箭头'/.test(pagesJs),
     '能量流副标题已改口径（方向见箭头，不再是"见各侧徽章"）');

// 新样式钩子
t.ok(/\.rt-col \{[^}]*gap: 14px/.test(css), 'app.css 有 .rt-col（右列两卡用全站同一档 14px 间距）');

/* ------------- H 配置通道②：接入参数下发到端侧的界面契约 -------------
   后端会把接入参数原子落盘到 config/point-map/active.conn，13/ 07/ 启动时读它。
   界面这一侧要守住的是**告知**：如果保存后只弹一句"已保存"，用户会以为只是
   写进了数据库，于是继续手敲 --host —— 那正是这个功能要消灭的"两份互相
   不一致的真相"。所以提示语与状态徽章都必须把"文件落哪儿了"说出来。 */
t.head('H 接入参数下发（配置通道②）');

t.ok(/已落盘 → \$\{cf\.path\}/.test(pagesJs) || /cf\.path/.test(pagesJs),
     'pages.js 保存接入参数后提示落盘路径（不只说"已保存"）');
t.ok(/cf\.removed/.test(pagesJs),
     'pages.js 处理「地址清空 → 端侧配置文件被删」的提示分支');
t.ok(/pm\.conn_file_exists/.test(pagesJs) && /pm\.conn_synced/.test(pagesJs),
     'pages.js 渲染落盘状态徽章（conn_file_exists / conn_synced）');
t.ok(/active\.conn/.test(pagesJs),
     'pages.js 说明里写明端侧读的是 config/point-map/active.conn');
t.ok(/--device modbus/.test(pagesJs),
     'pages.js 说明里点出端侧入口（--device modbus 不必再手敲 IP）');
t.ok(/已下发到端侧文件/.test(pagesJs) && /尚未下发到端侧文件/.test(pagesJs),
     'pages.js 区分「已下发」与「尚未下发」两种状态文案');

// 接口入口没变（同一次 PUT /conn 既写库又落盘，前端不需要多调一次）
t.ok(/saveConn:\s*\(id, body\)\s*=>\s*put\(`\/api\/devices\/\$\{encodeURIComponent\(id\)\}\/conn`/.test(apiJs),
     'api.js saveConn 仍是一次 PUT /conn（写库 + 落盘由后端同事务完成）');

/* ---------------- I 引擎双模式（run / sim）的界面契约 ----------------
   后端把"数据从哪来"做成了平台级状态：
     engine = run → 实时引擎按墙钟跑，数据边跑边入库，界面要跟着长
     engine = sim → 离线批量仿真，跑完装成一份数据集，界面看的是静态结果
   界面这一侧最容易犯的错是"看起来在跑"：静态数据配个会动的绿点，
   或者反过来，实时数据配一行"最后更新"却不刷新。下面这些断言锁死：
     ① 两种模式都能切，且切换会重新解析数据集
     ② 运行模式有实时刷新（定时器 + 离开页面必须清掉）
     ③ 实时引擎的启停/清空有界面入口，且只让 operator 以上点
     ④ 仿真页能发起日/周/月并轮询进度，跑完能看结果
     ⑤ 一切"模型 / 真机"的标注如实，不假装接了真机                     */
t.head('I 引擎双模式（run / sim）');

// ① 顶栏引擎分段器 + 切换后重新解析数据集
t.ok(/id="engine-seg"/.test(indexHtml), 'index.html 有引擎分段器 #engine-seg');
t.ok(/engines\.forEach|list\.forEach\(e =>/.test(appJs) && /data-engine/.test(appJs),
  'app.js 按 /api/engine 的 engines[] 生成引擎按钮');
t.ok(/state\.scenario = scenarioId \|\| '';\s*\n\s*state\.t = null;\s*\n\s*await loadEngine\(\)/
     .test(sliceFn(appJs, 'async function switchEngine')),
  '切引擎模式后重新解析数据集与时刻并重载引擎信息（不沿用旧数据集）');
t.ok(/await API\.setEngine\(eng\)/.test(appJs), 'app.js 调 POST /api/engine 切换模式');
t.ok(/function canWrite\(\)/.test(appJs) && /if \(!canWrite\(\)\)/.test(appJs),
  '引擎切换与会话写操作共用一道角色闸门');

// ② 运行模式自动刷新
t.ok(/const LIVE_PAGES = \[[^\]]*'overview'[^\]]*'realtime'[^\]]*\]/.test(appJs),
  'app.js 定义运行模式下会自刷的页面白名单');
t.ok(/'strategies'/.test(appJs) && !/LIVE_PAGES = \[[^\]]*'strategies'/.test(appJs),
  '策略配置等**有输入**的页面不在自刷白名单里（否则会把正在敲的内容冲掉）');
t.ok(/setInterval\(liveTick, LIVE_REFRESH_MS\)/.test(appJs), '运行模式挂定时刷新');
t.ok(/function stopLiveTimer|const stopLiveTimer/.test(appJs) && /clearInterval\(liveTimer\)/.test(appJs),
  '离开运行模式 / 退出登录会清掉定时器');
t.ok(/document\.hidden/.test(appJs), '后台标签页不自刷（省接口）');
t.ok(/keepScroll/.test(appJs) && /content\.scrollTop = keepTop/.test(appJs),
  '自刷保留滚动位置（否则每 5 秒被弹回页顶）');
t.ok(/id="live-badge"/.test(indexHtml) && /live-badge/.test(css),
  '页头有「实时数据」徽章（含样式）');
t.ok(/lv-on|lv-warn/.test(css), '实时徽章区分运行中 / 已停写两种状态');
t.ok(/onDispose/.test(appJs) && /disposers\.splice\(0\)/.test(appJs),
  '页面可注册清理函数（换页前统一执行，防止定时器泄漏）');

// ③ 实时引擎的界面入口
t.ok(/id="engine-banner"/.test(indexHtml), 'index.html 有实时引擎状态条');
t.ok(/API\.liveStart\(/.test(appJs) && /API\.liveStop\(/.test(appJs) && /API\.liveReset\(/.test(appJs),
  'app.js 覆盖启停与清空三个动作');
t.ok(/window\.confirm\(/.test(appJs), '清空实时数据前有二次确认（不可撤销）');
t.ok(/模型源，不是真实电站/.test(appJs), '状态条如实标注「模型源，不是真实电站」');
t.ok(/source_is_model/.test(appJs), '前端读后端给的 source_is_model 标记（不写死"模型源"）');

// ④ 仿真页
t.ok(/'day', 'week', 'month'\]|kinds/.test(pagesJs), '仿真页时长选项来自后端 kinds');
t.ok(/API\.simStart\(\{ kind, fault \}\)/.test(pagesJs), '仿真页按 时长 + 工况 发起仿真');
t.ok(/setInterval\(async \(\) =>/.test(pagesJs), '仿真页在有 running 时轮询进度');
t.ok(/ctx\.onDispose\(\(\) => \{ if \(timer\)/.test(pagesJs),
  '仿真页把轮询定时器交给 onDispose（换页即停）');
t.ok(/API\.simDelete\(/.test(pagesJs) && /删除/.test(pagesJs), '仿真页能删掉一次仿真');
t.ok(/典型日/.test(pagesJs), '仿真页写明「典型日重复」的诚实边界');
t.ok(/只发生在第 0 天|故障窗/.test(pagesJs), '仿真页写明故障注入只覆盖第 0 天');
t.ok(/ctx\.openDataset\(/.test(pagesJs) && /openDataset: async \(/.test(appJs),
  '「查看结果」走 openDataset（会顺带切到仿真模式，不会出现点了没反应）');
t.ok(/if \(state\.engine !== 'sim'\) \{\s*\n\s*await switchEngine\('sim', scenarioId\)/.test(appJs),
  'openDataset 在看仿真结果前先确保引擎在仿真模式（与顶栏、空状态共用 switchEngine）');

// ⑤ 数据源插拔 + 不假装接真机
const enginePy = fs.existsSync(path.join(ROOT, '14', 'src', 'engine.py'))
  ? read(path.join(ROOT, '14', 'src', 'engine.py')) : '';
t.ok(/"available": False/.test(enginePy),
  '后端把真机数据源标为不可用（如实告知，不开假入口）');
t.ok(/"id": "device"/.test(enginePy) && /--device modbus/.test(enginePy),
  '后端写明接真机的入口与前置条件');
t.ok(/"id": "model"/.test(enginePy) && /sim_live\.exe/.test(enginePy),
  '后端提供模型源（sim_live.exe）');
t.ok(/record_csv\.h/.test(enginePy),
  '后端说明实录契约与 07/ 完全一致（换真机时入库与界面不用改）');

/* =====================================================================
   J 装配视图数据与 CSV 一致（爆炸图 / 一次接线图的共用地基）
   ---------------------------------------------------------------------
   ★ 这一段守的是"派生产物没脱钩"。
     assets/assembly.js 是从 docs/规划/部件测点映射表.csv 生成的，不是手抄的。
     手抄清单的典型失效是：CSV 加了一行、界面还按旧的画 —— 没有任何业务断言会红，
     界面也不报错，只是"现场有这个测点、界面上没这个测点"。
     所以这里**逐点**比三列（测点名 / 设备点名 / 单位），不是只比总数：
     只比总数的话，两处各错一处还能凑成一样的数。
   ===================================================================== */
t.head('J 装配视图 ← CSV 一致');

const CSV_MAP = path.join(ROOT, 'docs', '规划', '部件测点映射表.csv');
t.ok(fs.existsSync(CSV_MAP), '存在 docs/规划/部件测点映射表.csv');

/* 极简 CSV 解析（够用即可：本仓库规划 CSV 是 UTF-8 + 逗号分隔 + 可选双引号） */
function parseCsv(text) {
  const rows = []; let row = [], cur = '', q = false;
  const s = String(text).replace(/^\uFEFF/, '');
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (q) {
      if (c === '"') { if (s[i + 1] === '"') { cur += '"'; i++; } else q = false; }
      else cur += c;
    } else if (c === '"') q = true;
    else if (c === ',') { row.push(cur); cur = ''; }
    else if (c === '\n') { row.push(cur); rows.push(row); row = []; cur = ''; }
    else if (c !== '\r') cur += c;
  }
  if (cur !== '' || row.length) { row.push(cur); rows.push(row); }
  return rows.filter(r => r.some(x => String(x).trim() !== ''));
}

const mapRows = parseCsv(read(CSV_MAP));
const mapHead = mapRows[0] || [];
const mapBody = mapRows.slice(1);
const hdr = needle => mapHead.findIndex(c => String(c).replace(/\s/g, '').includes(needle));
const I_AREA = hdr('所属部位'), I_PART = hdr('部件'), I_PT = hdr('测点'),
      I_DEV = hdr('设备点名'), I_UNIT = hdr('单位');
t.ok(I_AREA >= 0 && I_PART >= 0 && I_PT >= 0 && I_DEV >= 0 && I_UNIT >= 0,
  'CSV 表头含「所属部位 / 部件 / 测点 / 设备点名 / 单位」五列');

// assembly.js 是纯数据脚本（const ASSEMBLY = [...]），不依赖浏览器 —— 直接求值取出
const ASSEMBLY = new Function(`${asmJs}\nreturn ASSEMBLY;`)();

/* ★ join 键用 src（CSV 原值），不用 name（页面显示名）。
   name 是给人看的（带部位序号、把"站内·一次"写成"④ 站级·站内一次"），
   与 CSV 的写法天然不同 —— 早先用 name 做 join，只能靠一串 replace 猜两边口径，
   改一个显示名就误报。现在 assembly.js 由 15/scripts/gen_assembly.py 生成，
   每个部位 / 部件都带 src，join 是**精确**的：对不上就是真脱钩。 */
const GEN_PY = path.join(ROOT15, 'scripts', 'gen_assembly.py');
t.ok(fs.existsSync(GEN_PY), '存在生成器 15/scripts/gen_assembly.py（派生产物可重现）');

const allParts = ASSEMBLY.flatMap(a => a.parts.map(p => Object.assign({ area: a }, p)));
const nPts = allParts.reduce((n, p) => n + (p.points || []).length, 0);

t.eq(ASSEMBLY.length, 4, '装配视图 4 个部位');
t.eq(allParts.length, 14, '装配视图 14 个部件');
t.eq(nPts, 78, '装配视图 78 个测点');
t.eq(mapBody.length, 78, 'CSV 数据行 = 78（与装配视图同源）');
t.ok(new Set(allParts.map(p => p.id)).size === allParts.length, '部件 id 不重复');

// ① 部件自洽：n 必须等于它自己 points[] 的长度（界面上写着"N 个测点"）
allParts.forEach(p =>
  t.eq((p.points || []).length, p.n, `部件 ${p.id} 的 n 与 points[] 长度一致`));

// 部位集合：CSV 里出现的新部位必须已经在 assembly.js 里登记（否则静默漏掉一整舱）
t.eq(ASSEMBLY.map(a => a.src).sort(),
  [...new Set(mapBody.map(r => String(r[I_AREA]).trim()))].sort(),
  '部位 src 集合与 CSV「所属部位」一致（CSV 新增部位会被这里抓住）');
t.ok(new Set(allParts.map(p => p.src)).size === allParts.length,
  '部件 src 在 CSV 中唯一（join 不会串组）');

// ② 分组计数：CSV 按（部位 + 部件）分组的行数 = 该部件的点数
const csvGroup = new Map();
mapBody.forEach(r => {
  const k = `${String(r[I_AREA]).trim()}||${String(r[I_PART]).trim()}`;
  if (!csvGroup.has(k)) csvGroup.set(k, []);
  csvGroup.get(k).push(r);
});
allParts.forEach(p => {
  const k = `${String(p.area.src).trim()}||${String(p.src).trim()}`;
  t.eq((csvGroup.get(k) || []).length, p.points.length,
    `部件 ${p.id} 的测点数与 CSV 分组「${k}」一致`);
});

// ③ 逐行三列比对（真正"不是手抄"的证据）
const ptBad = [];
allParts.forEach(p => {
  const k = `${String(p.area.src).trim()}||${String(p.src).trim()}`;
  const g = csvGroup.get(k) || [];
  p.points.forEach((pt, j) => {
    const r = g[j];
    if (!r) { ptBad.push(`${p.id}[${j}] 缺对应 CSV 行`); return; }
    if (pt.n !== String(r[I_PT]).trim())   ptBad.push(`${p.id}[${j}] 测点名「${pt.n}」≠「${r[I_PT]}」`);
    if (pt.d !== String(r[I_DEV]).trim())  ptBad.push(`${p.id}[${j}] 设备点名「${pt.d}」≠「${r[I_DEV]}」`);
    if (pt.u !== String(r[I_UNIT]).trim()) ptBad.push(`${p.id}[${j}] 单位「${pt.u}」≠「${r[I_UNIT]}」`);
  });
});
t.eq(ptBad, [], `78 个测点逐行对齐 CSV（测点名 / 设备点名 / 单位），不符 ${ptBad.length} 处`);

// ④ 实时源绑定：只有 5 个部件拿得到值，其余必须显式为 null（不是"暂时空"）
const LIVE_PARTS = ['cluster', 'cell', 'conv', 'meter', 'pv'];
t.eq(allParts.filter(p => p.dev).map(p => p.id).sort(), [...LIVE_PARTS].sort(),
  '仅 5 个部件绑定实时设备（对应 /api/realtime 那 5 台）');
allParts.filter(p => !p.dev).forEach(p =>
  t.ok(p.key === null || p.key === undefined, `无实时源的部件 ${p.id} 不绑 key`));

// ⑤ 页面上的计数必须由 ASSEMBLY 现算，不许写死
t.ok(/ASSEMBLY\.reduce/.test(pagesJs), '装配页的部件 / 测点计数由 ASSEMBLY 现算（不写死数字）');

/* =====================================================================
   K 报表打印 与 告警分类
   ===================================================================== */
t.head('K 报表打印与告警分类');

// ---- 打印 / 存 PDF ----
t.ok(/@media print/.test(css), 'app.css 有 @media print（报表可打印 / 存 PDF）');
t.ok(/window\.print\(\)/.test(pagesJs), '报表页有「打印 / 存为 PDF」入口');
t.ok(/\.no-print/.test(css) && /class: 'no-print'/.test(pagesJs),
  '操作条整行带 .no-print（不进纸面）');

/* ★ 打印里必须拆掉 overflow：.app-view 是 height:100vh + overflow:hidden，
   .content 是 overflow-y:auto 的滚动容器。不拆的话打印只吐"第一屏"，
   下面的表格全丢 —— 不报错，纸面看着像"报表就这些内容"。 */
/* ★ 收尾的 \n 后面不要再要求 \n：app.css 是 CRLF 换行（实测 647 行 CRLF），
   `}\r\n` 对 `\n\}\n` 不成立，块取不出来 → 下面 5 条连带全红，
   看上去像"打印样式没写"，其实是正则没吃 CRLF。 */
const PRINT_BLOCK = (css.match(/@media print \{([\s\S]*?)\n\}/) || ['', ''])[1];
t.ok(PRINT_BLOCK.length > 0, '能取出 @media print 块');
t.ok(/\.app-view[^}]*overflow: visible/.test(PRINT_BLOCK),
  '打印时 .app-view 拆掉 overflow（否则只印第一屏）');
t.ok(/\.content[^}]*overflow: visible/.test(PRINT_BLOCK),
  '打印时 .content 拆掉滚动（否则下方表格被裁掉）');
t.ok(/\.tbl-wrap[^{]*\{[^}]*max-height: none/.test(PRINT_BLOCK),
  '打印时表格容器不设高度上限');
t.ok(/break-inside: avoid/.test(PRINT_BLOCK), '打印时卡片整块不跨页断开');
t.ok(/@page/.test(PRINT_BLOCK), '打印纸张尺寸已指定');

// 打印稿抬头：数据集 / 视角 / 出具人 / 时间 + 后端的数据边界说明
t.ok(/\.print-head/.test(css) && /class: 'print-head'/.test(pagesJs),
  '打印稿有抬头（数据集 / 视角 / 出具人 / 时间）');
t.ok(/d\.data_note/.test(pagesJs),
  '打印稿带上后端 data_note（不把仿真数据印得像现场实录）');
t.ok(/PERIOD_NAME/.test(pagesJs),
  '日报 / 周报 / 月报的名字只有一份（分段按钮与打印抬头共用）');

// ---- 告警分类 ----
t.ok(/API\.alarmSummary\(ctx\.scenario, \{ level, source \}\)/.test(pagesJs),
  '告警页把当前筛选传给 summary（计数才与列表条数对得上）');
t.ok(/API\.alarms\(ctx\.scenario, \{ level, source, limit: LIMIT, offset \}\)/.test(pagesJs),
  '告警页按 等级 + 来源 双维取数');
t.ok(/PREFS\.alarmLevel/.test(pagesJs) && /PREFS\.alarmSource/.test(pagesJs)
     && /PREFS\.alarmOffset/.test(pagesJs),
  '★ 告警筛选存 PREFS（本页在 LIVE_PAGES 里，不存会被 5 s 重刷冲回"全部"）');
t.ok(/const LEVELS = \['INFO', 'WARNING', 'DERATED', 'FAULT', 'EMERGENCY'\]/.test(pagesJs),
  '等级全谱与后端 ALARM_LEVELS 同一份清单');
t.ok(/onclick: \(\) => setLevel\(/.test(pagesJs) && /onclick: \(\) => setSource\(/.test(pagesJs),
  '分类统计表可点（点了就筛，不只是好看）');

t.ok(/source = q1\(query, "source"\)/.test(apiPy),
  '后端 /api/alarms 支持 source 筛选');
t.ok(/ALARM_LEVELS = \("INFO", "WARNING", "DERATED", "FAULT", "EMERGENCY"\)/.test(apiPy),
  '后端 summary 按全谱补零（没发生的等级也占一行）');
t.ok(/def _where\(use_level: bool, use_source: bool\)/.test(apiPy),
  '后端 summary 走交叉筛选（各维度计数带"另一个维度"、不带自己）');
t.ok(!/status = q1\(query, "status"\)/.test(apiPy),
  '★ 后端不开 status 筛选（库里 status 恒为 active，开放即"恒 0 条"假入口）');

/* =====================================================================
   [L] 脚本语法守卫 —— 每个 .js 都必须能被解析

   ★ 为什么必须有这一段（2026-10-11 撞出来的）：
     pages.js 里一个多余的括号 ⇒ **整个文件加载失败** ⇒ 登录按钮没有 JS 接管
     ⇒ 表单走浏览器**原生提交** ⇒ 页面刷新。症状是「登录进不去」，
     而且界面上**没有任何报错**，控制台之外完全看不出是脚本坏了。

     更糟的是：在此之前本文件的 289 条断言全是**文本 / 正则**检查，
     **没有一条会真的解析 JS**。于是全量构建一片绿，产品却是坏的 ——
     典型的「测试全绿 ≠ 这条路走通了」。

   判据用 `node --check`：只解析不执行，不依赖浏览器环境；
   node 路径用 process.execPath（自检本来就是 Node 跑的），不引入新依赖。
   ===================================================================== */
t.head('[L] 脚本语法守卫');
const { execFileSync } = require('child_process');
const ASSETS = path.join(ROOT15, 'assets');
const JS_FILES = ['api.js', 'ui.js', 'assembly.js', 'pages.js', 'app.js'];

t.eq(JS_FILES.filter(f => fs.existsSync(path.join(ASSETS, f))).length,
     JS_FILES.length, '五个前端脚本都在');

const syntaxBad = [];
for (const f of JS_FILES) {
  const fp = path.join(ASSETS, f);
  if (!fs.existsSync(fp)) continue;
  try {
    execFileSync(process.execPath, ['--check', fp], { stdio: 'pipe' });
  } catch (e) {
    const msg = String((e && e.stderr) || (e && e.message) || e);
    syntaxBad.push(`${f}: ${msg.split('\n').filter(Boolean)[0] || msg}`);
  }
}
t.eq(syntaxBad, [],
     '★ 五个脚本全部能被解析（语法错误 = 整个文件加载失败 ⇒ 登录按钮没反应、页面刷新，且无任何报错）');

// 新增脚本必须同步进上面的清单，否则这条守卫会漏掉它
const idxHtmlText = read(path.join(ROOT15, 'index.html'));
const scriptRefs = [...idxHtmlText.matchAll(/<script\s+src="assets\/([^"]+)"/g)].map(m => m[1]);
t.ok(scriptRefs.length > 0, `index.html 引用了 ${scriptRefs.length} 个脚本`);
t.eq(scriptRefs.filter(r => !JS_FILES.includes(r)), [],
     'index.html 引用的脚本**全部**被语法守卫覆盖（新增脚本必须同步进 JS_FILES）');

/* =====================================================================
   [M] 「这一刻没有数据」≠「加载失败」—— 空状态 + 出路

   ★ 为什么有这一段（2026-10-16 实跑撞出来的）：
     §63.7 当时记的判断是「renderPage 没有 try-catch ⇒ 异常冒泡回 boot()
     ⇒ 用户被锁在登录界面外」。用 jsdom 驱动**真前端 + 真后端**跑一遍
     （运行模式 + 实时引擎未启动）后，这个判断被推翻：登录进得去，
     12 个菜单都在，只是 6 个页面显示红字「加载失败：运行模式还没有实时数据…」。
     —— 静态推断又一次把「呈现措辞不对」读成了「流程断了」。

     真问题是**呈现**：404（数据未就绪）被当成「加载失败」，用户读到的是
     「功能坏了」，排障方向被带偏。所以判据落在四件事上：
       ① 错误对象必须带 HTTP 状态（不然前端只能猜文案，而文案是后端自由文本）；
       ② 只有 404 走空状态；5xx / 网络错仍说「加载失败」；
       ③ 空状态里**必须有出路**，且出路走既有接口（setEngine / liveStart）；
       ④ 出路按钮按角色给（viewer 不该看到操作按钮）。
   ===================================================================== */
t.head('[M] 数据未就绪 ≠ 加载失败');

t.ok(/err\.status = res\.status/.test(apiJs),
     'api.js 把 HTTP 状态挂到错误对象上（前端据此分流，不猜后端文案）');
t.ok(/e401\.status = 401/.test(apiJs),
     '401 同样带状态（会话过期与"没数据"是两件事）');

t.ok(/function isNotReady\(e\)/.test(appJs) && /e\.status === 404/.test(appJs),
     'app.js 有「数据未就绪」判据，判据是 HTTP 404 而不是错误文案');
t.ok(/isNotReady\(e\)/.test(appJs) && /renderNotReady\(e, ctx\)/.test(appJs),
     '渲染失败时按 404 / 其他分流');
t.ok(/加载失败：\$\{e\.message\}/.test(appJs),
     '★ 真失败（5xx / 网络）仍保留「加载失败」措辞 —— 分流不能把两种错合成一种');

// 取一个顶层函数的源码片段用 sliceFn（定义在文件上方）。
const nrBody = sliceFn(appJs, 'function renderNotReady');
t.ok(nrBody.length > 0 && nrBody.length < appJs.length, '空状态渲染器 renderNotReady 能定位到');
// 注意判据的精度：空状态里**不能有任何一段文字以「加载失败」开头**，
// 但它可以（也应该）明说「这不是加载失败」—— 所以要按 `text: '…'` 的取值判，
// 不能用 `/加载失败/` 一刀切（第一版就这么写错了，被自己的反向断言拦下）。
t.ok(!/text:\s*['"`]加载失败/.test(nrBody),
     '★ 空状态不把「加载失败」当作要显示的文字（措辞直接决定用户往哪查）');
t.ok(/这不是加载失败/.test(nrBody),
     '反过来要**显式**告诉用户「接口是通的、不是坏了」，否则他会继续查接口');
t.ok(/switchEngine\('sim'\)/.test(nrBody),
     '出路①「切到仿真模式」走既有接口，不是新造一套');
t.ok(/API\.liveStart\(/.test(nrBody),
     '出路②「启动实时引擎」走既有接口');
t.ok(/canWrite\(\)/.test(nrBody),
     '出路按钮按角色给（viewer 看不到操作入口）');
t.ok(/goPage\('sim'\)/.test(nrBody),
     '仿真模式下没有数据集时，出路是「去仿真页」而不是原地重试');

// 切模式必须成套（少一步就出「按钮切过去了、看的还是旧数据集」）
const swBody = sliceFn(appJs, 'async function switchEngine');
t.ok(swBody.length > 0, '切换引擎模式收成一个入口 switchEngine');
t.ok(/state\.scenario = scenarioId \|\| ''/.test(swBody) && /await loadEngine\(\)/.test(swBody),
     '★ 切模式必须重解析数据集（清空 / 换成指定那份）并重载引擎信息'
     + '（不清就会「顶栏显示仿真模式、文案还是运行模式那句」）');
t.ok((appJs.match(/API\.setEngine\(/g) || []).length === 1,
     '★ setEngine 全文件只在一处调用（空状态与顶栏共用 —— 各写一遍必然漂移）');

// 登录入口不能在「登录框已隐藏」之后把异常抛回去（那会把用户扔进空白页且无处申诉）
const enterBody = sliceFn(appJs, 'async function enterApp');
t.ok(/classList\.add\('hidden'\)[\s\S]*try \{/.test(enterBody),
     '★ enterApp 在隐藏登录框之后有 try 兜底（否则错误文案会写进看不见的 #login-err）');
t.ok(/界面初始化失败/.test(enterBody),
     '初始化失败时留在界面里说清楚，而不是弹回登录框');

const okAll = t.report();
process.exit(okAll ? 0 : 1);
