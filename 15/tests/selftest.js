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
// v1.10 新增的进程编排模块：这一段的判据要指向**真正实现它的文件**，
// 不能只看 api.py —— 否则"断言通过"和"功能存在"是两件事。
const P14SIMPROC = path.join(ROOT, '14', 'src', 'simproc.py');
const simprocPy = fs.existsSync(P14SIMPROC) ? read(P14SIMPROC) : '';
// 平台启动路径：链路进程只有在这里被「认领」一次，面板才不会在重启后说反话。
const P14SERVER = path.join(ROOT, '14', 'src', 'server.py');
const serverPy = fs.existsSync(P14SERVER) ? read(P14SERVER) : '';


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
     （2026-10-16 加该段时因此误判过一条否定断言：空状态里本来没有"加载失败"，
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
/* ★ XML 命名空间**不是**网络资源：
   createElementNS('http://www.w3.org/2000/svg', 'svg') 里那个 URL 永远不会被
   fetch —— 它是"这个元素属于哪一套标签"的标识符，写错只是图形画不出来。
   把它算成外部引用，等于禁止手写 SVG，而本项目不引图标库（"换一套"的循环
   箭头就是手写的）。真正的离线风险是 <img src="https://..."> / CDN 链接 /
   外部字体，那些才是这条判据要拦的。 */
const XML_NS = new Set(['http://www.w3.org/2000/svg',
                        'http://www.w3.org/1999/xhtml',
                        'http://www.w3.org/1999/xlink']);
const externals = [...(indexHtml + css + apiJs + uiJs + asmJs + pagesJs + appJs)
  .matchAll(/https?:\/\/[^\s"'<>)]+/g)]
  .map(m => m[0])
  .filter(u => !/127\.0\.0\.1|localhost/.test(u))
  .filter(u => !XML_NS.has(u));
t.eq(externals, [], '无外部资源引用（离线可开）');
t.ok(/const NS = 'http:\/\/www\.w3\.org\/2000\/svg'/.test(pagesJs)
     && /createElementNS\(NS,\s*'svg'\)/.test(pagesJs)
     && !/(src|href)\s*=\s*['"]?https?:/.test(pagesJs),
     '★ SVG 图标走 createElementNS（h() 用 createElement 进不了 SVG 命名空间）');

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
//   `id: 'xxx', name:` 形状（运行模式分段的模式、来源条目的 id），
//   全文扫会把它们当菜单项抓进来，然后报出一串"缺图标"的假失败。
const MENU_BLOCK = (appJs.match(/const MENU = \[([\s\S]*?)\n\];/) || ['', ''])[1];
const menuIds = [...MENU_BLOCK.matchAll(/id:\s*'([a-z]+)',\s*name:/g)].map(m => m[1]);
t.eq(menuIds.length, 11, '菜单项数 = 11（产品化后去掉「仿真」页）');
t.ok(new Set(menuIds).size === menuIds.length, '菜单 id 不重复');
/* ★ v1.5 起「仿真」不再是产品功能（离线批量仿真是研发侧的验证手段，
   不是现场用户要按的按钮）。这条是**反向断言**：入口不许悄悄长回来 ——
   长回来了也没人会报错，只会在评审时被问"业主为什么要按这个"。 */
t.ok(!menuIds.includes('sim'), '菜单里没有「仿真」页（离线批量仿真不进产品界面）');
t.ok(!/Pages\.sim\b/.test(appJs) && !/function sim\(/.test(pagesJs),
  'app.js / pages.js 都没有仿真页的入口与实现');
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
t.eq(pageKeys.length, 11, 'Pages 导出页面数 = 11（与菜单项数一致）');
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

// v1.5：数据源收敛成**一个**。前端必须：
//   ① 允许 scenario 为空（交给后端 resolve_scenario 解析）
//   ② 唯一来源是 /api/engine 的 scenario 字段
//   ③ 不提供任何"选数据集 / 切引擎"的入口（选了就分不清是真数据还是演示数据）
t.ok(!/scenario:\s*'normal'/.test(appJs), '前端不硬编码默认场景 normal');
t.ok(/const sc = d\.scenario \|\| '';/.test(appJs),
  '★ 当前数据集唯一取自 /api/engine 的 scenario 字段（没有第二个来源）');
t.ok(/state\.scenario = sc;/.test(appJs), '解析结果落到 state.scenario 供全站共用');
t.ok(!/data-scenario=/.test(indexHtml) && !/data-scenario/.test(appJs),
  '★ 界面上没有数据集切换按钮（产品不提供"换一份演示数据"的入口）');
t.ok(!/buildScenarioSeg|scenarioEntry|scenarioList/.test(appJs),
  '数据集选择器的三个函数已随入口一起删除（不留半截实现）');

// 时标口径：一天 86400 s / 每 10 s 一条 = 8640 个记录点。
// ★ 时间轴的值域是**采样点下标**，不是秒。旧的「按整段时长铺 864 格、
//   再由后端取 ≤t 的最后一拍」在数据连续时没问题，但 live 是跨会话累积的
//   （可能横跨几周、中间大段空白），绝大多数位置会落到断档里、一律回落到
//   同一拍 —— 拖动有反应、画面不动。这条断言钉的是"每个位置都能取到数"。
t.ok(/86400/.test(apiPy), '后端时标上界 86400 s');
t.ok(/max: String\(Math\.max\(0, N - 1\)\), step: '1'/.test(pagesJs),
  '★ 时间轴值域 = 采样点下标（每个位置都落在数据里真实存在的一拍）');
t.ok(!/tHi - tLo\) \/ 864\)/.test(pagesJs),
  '★ 已删除"按整段时长铺 864 格"的旧实现（断档时会产出取不到数的位置）');
t.ok(/N \? Number\(dayT\[tIdx\]\) : -1/.test(pagesJs),
  '★ 取不到采样点时发 -1（= 最新一拍），不发 0 —— wall 口径下 0 是 1970 年');

// 平台已打通真机实时（07/ --record → live 场景）。
// 界面上必须如实说明来源，不能还写着「尚未接入真机」，也不能把"模型源"藏起来。
t.ok(/source_is_model/.test(appJs), '界面读后端给的 source_is_model 标记（不写死"模型源"）');
// v1.6：页脚报的是「数据源 + 是不是现场装置」，不再用"模型源"这个研发口径的称呼。
t.ok(/非现场装置/.test(appJs) && /现场设备/.test(appJs),
  '页脚常驻标注数据源性质（模拟的一定带「非现场装置」，现场的一定写「现场设备」）');

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

/* ---------------- I 单一数据源 + 采集状态 ----------------
   ★ v1.5 产品化：界面不再有"引擎双模式"与"数据集切换器"。
     删它的理由不是"简化界面"，是**语义**：
       · "运行模式 / 仿真模式"描述的是"这些数字是怎么算出来的"——那是研发要
         区分的事。现场用户只关心"这是不是我这台站的数、新不新"。
       · 数据源做成开关，就必然出现"界面显示一切正常、其实看的是三天前的
         仿真结果"，而且没有任何东西会报错 —— 这正是本项目最忌讳的失效方式。
     所以：数据源由后端单点决定（/api/engine 的 scenario），界面只负责两件事
     —— 如实标注来源性质、采集不正常时说清楚。本段同时守住"入口不许长回来"。
   ==================================================== */
t.head('I 单一数据源与采集状态');

// ① 两个切换器都拆掉了（反向断言：入口长回来不会有人报错，只会让人分不清数据真假）
t.ok(!/id="engine-seg"/.test(indexHtml), '★ index.html 无引擎模式分段器 #engine-seg');
t.ok(!/id="scenario-seg"/.test(indexHtml), '★ index.html 无数据集切换器 #scenario-seg');
t.ok(!/data-engine/.test(appJs), '★ app.js 不再生成引擎切换按钮');
t.ok(!/API\.setEngine\(/.test(appJs),
  '★ 界面不改后端引擎模式 —— 改一次全站就换了一份数据，而用户毫不知情');
// 顶栏仍保留"运行模式"（峰谷套利 / 削峰填谷 / 辅助服务 / 保电备电）——
// 那是业务维度、是真功能，不能跟数据源一起删掉。
t.ok(/id="mode-seg"/.test(indexHtml) && /API\.activateMode\(/.test(appJs),
  '业务维度的运行模式切换仍在（与数据源无关，不能连带删掉）');

// ② 数据源只有一个解析点
t.ok(/async function loadDataSource\(\)/.test(appJs),
  '数据源收成唯一入口 loadDataSource()');
t.ok(/const sc = d\.scenario \|\| '';/.test(appJs) && /state\.scenario = sc;/.test(appJs),
  '★ 当前数据集只从 /api/engine 的 scenario 取（后端按采集状态决定，前端不猜）');
t.ok(/state\.t = null;   \/\/ 换了数据集/.test(appJs),
  '换数据集时清掉记住的时刻（两份数据的时标口径可能不同，混用会取到空拍）');

// ③ 自动刷新：判据只有"登录着 + 纯展示页"
t.ok(/const LIVE_PAGES = \[[^\]]*'overview'[^\]]*'realtime'[^\]]*\]/.test(appJs),
  'app.js 定义自动重刷的页面白名单');
t.ok(/'strategies'/.test(appJs) && !/LIVE_PAGES = \[[^\]]*'strategies'/.test(appJs),
  '策略配置等**有输入**的页面不在自刷白名单里（否则会把正在敲的内容冲掉）');
t.ok(/setInterval\(liveTick, LIVE_REFRESH_MS\)/.test(appJs), '挂定时刷新');
t.ok(/function stopLiveTimer|const stopLiveTimer/.test(appJs) && /clearInterval\(liveTimer\)/.test(appJs),
  '退出登录会清掉定时器');
t.ok(/document\.hidden/.test(appJs), '后台标签页不自刷（省接口）');
t.ok(/keepScroll/.test(appJs) && /content\.scrollTop = keepTop/.test(appJs),
  '自刷保留滚动位置（否则每 5 秒被弹回页顶）');
t.ok(/const want = !!state\.user && LIVE_PAGES\.includes\(state\.page\);/.test(appJs),
  '★ 自刷判据不再看引擎模式（同一页在不同数据源下行为必须一致，否则用户不知道为什么）');
t.ok(/id="live-badge"/.test(indexHtml) && /live-badge/.test(css),
  '页头有刷新徽章（含样式）');
t.ok(/lv-on|lv-warn/.test(css), '徽章区分正常 / 采集异常两种状态');

// ★ v1.11：采集停了、"数据停在某一拍"这件事必须由徽章说出来
//   原来是「每 5 s 自动刷新 · 更新于 <现在>」—— 那个"更新于"是**前端刷新时间**，
//   关掉采集之后它照样每次刷新都在变，于是"数据已经停了两小时"和"正在实时采"
//   在界面上长得一模一样（2026-10-19 实测：live 冻结在 01:37:39，概览仍 200 有数）。
t.ok(/采集已停止/.test(appJs) && /数据停在/.test(appJs),
  '★ 采集停了就说「采集已停止 · 数据停在 xx」—— 不能继续显示「更新于 <现在>」：'
  + '那是刷新时间，不是数据时间');
t.ok(/lv\.imported_at/.test(appJs),
  '★ 那个"数据时间"取自入库时间（imported_at），不是前端时钟');
t.ok(/const frozen = run && !unhealthy/.test(appJs)
     && /lv\.state !== 'running'/.test(appJs) && /lv\.state !== 'starting'/.test(appJs),
  '★ "停住了"的判据要排除 running 与 starting —— starting 是刚起来还没跑第一拍，'
  + '把它当成"已停止"会在每次启动时误报一次');
t.ok(/lv\.state === 'failed' \? '失败' : '中断'/.test(appJs),
  '★ 失败 / 中断要与"用户主动停止"分开说：一个是去查日志，一个是本来就这样');
t.ok(/onDispose/.test(appJs) && /disposers\.splice\(0\)/.test(appJs),
  '页面可注册清理函数（换页前统一执行，防止定时器泄漏）');

// ④ 顶部采集状态条已整条删除（v1.9，用户要求）—— 三处都要干净，不留残壳
t.ok(!/id="engine-banner"/.test(indexHtml),
  '★ index.html 里没有状态条容器（留着空 div 就会有人又往里塞东西）');
t.ok(!/renderEngineBanner|LIVE_STATE/.test(appJs),
  'app.js 不再渲染状态条（renderEngineBanner / LIVE_STATE 一并删除，不留死函数）');
t.ok(!/\.engine-banner|\.eb-dot|\.eb-err/.test(css),
  '样式里没有残留（死 CSS 会让后来人以为这块还在用）');
// ★ 启停从 app.js 搬到设备管理页：其他页面是正式运行页面，不提供采集开关。
t.ok(!/API\.liveStart\(|API\.liveStop\(|API\.liveReset\(/.test(appJs),
  '★ app.js（页面外壳）不再直接启停采集 —— 开关只在设备管理页');
t.ok(/API\.liveStart\(/.test(pagesCode) && /API\.liveStop\(/.test(pagesCode)
     && /API\.liveReset\(/.test(pagesCode),
  '设备管理页覆盖启停与清空三个动作');
t.ok(/window\.confirm\(/.test(pagesCode), '清空实时数据前有二次确认（不可撤销）');
t.ok(/source_is_model/.test(appJs), '来源性质由后端 source_is_model 决定（不写死"模型源"）');

// ⑤ 实时监控页：时间轴仍在，但"播放 / 倍速"这类演示控件已去掉
t.ok(/跳到最新/.test(pagesJs), '实时监控页有「跳到最新」（产品里没有播放键）');
t.ok(!/rt-play|SPEEDS|icoPlay/.test(pagesJs), '★ 回放控制台（播放键 / 倍速）已删除');
t.ok(/API\.realtime\(ctx\.scenario, tt\)/.test(pagesJs),
  '实时监控仍按时刻取快照（查询能力没丢）');

// ⑥ 数据源插拔 + 不假装接真机（这部分后端能力不变，只是界面不再给入口）
const enginePy = fs.existsSync(path.join(ROOT, '14', 'src', 'engine.py'))
  ? read(path.join(ROOT, '14', 'src', 'engine.py')) : '';
// v1.6：真机源不再是"不可用"—— 它现在是**只读监视**（--record 不再要求
// --control），所以敢开。这是本轮把"接入后其他页面就能正常运行"落到实处的地方。
t.ok(/"id": "sim"/.test(enginePy) && /"id": "real"/.test(enginePy),
  '后端有两个数据源：sim / real');
t.ok(/"control": True/.test(enginePy) && /"control": False/.test(enginePy),
  '★ control 标记决定"下不下发"：替身闭环、真机只读');
t.ok(enginePy.includes('"--device", "modbus"'),
  '两个源走同一个采集器（07/ --device modbus）');
t.ok(/"id": "model"/.test(enginePy) && /sim_live\.exe/.test(enginePy),
  '降级路径仍保留模型源（sim_live.exe）');
t.ok(/同一份实录契约/.test(enginePy),
  '★ 写明两条路走同一份实录契约（换对端时入库与界面不用改）');
t.ok(/def default_scenario\(conn\)/.test(enginePy),
  '后端仍按采集状态决定默认数据集（前端删掉的只是"选择器"，不是这套判断）');

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

/* ★ 内置场景白名单：现在不是"要授权"，而是**前端根本不许有**。
   历史（2026-10-11 撞出来的静默缺口）：scenarioList() 里曾硬编码
   ['normal','fault'] 当"内置场景"名单；新增 stress 后后端有数据、
   ?scenario=stress 也照常返回 13 条，界面上却没有对应按钮 —— 用户点不到。
   全链路都通、接口也 200，只有"入口被名单挡住"这一处，是最难发现的一档。

   v1.5 把选择器整个删掉之后，这类缺口的**根**就不存在了：不再有名单，
   也就不会漏。所以判据从"名单要写对"升级成"不许有名单" ——
   这是更强的守卫：任何一个新数据集都不需要再改前端。 */
t.ok(!/\['normal',\s*'fault'\]/.test(appJs),
  '★ 前端不再出现内置场景白名单（没有名单就没有"漏配"）');
t.ok(!/scenarioEntry|scenarioList/.test(appJs),
  '照名字造按钮的那套函数已删（不留半截实现等人接回去）');
t.ok(/sch\.name/.test(appJs),
  '数据集的可读名由后端 scenario.name 提供（前端不维护第二份名字表）');

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
   [M] 一次接线图几何 —— 线不穿框 / 数不压字（2026-10-11 补）

   ★ 为什么这一段要**真跑一遍渲染**：
     前面 12 段全是文本 / 正则，能证明「代码里写了避让逻辑」，
     证明不了「画出来真的不重叠」。而这次的问题偏偏是几何算错：
       · label 放在「节点中心 − 8」，正落在节点名称那一行上 ⇒ 数字压文字；
       · 出线固定「a 右缘 → b 左缘」，b 在 a 左侧时会绕回去横穿 b 的框
         （母线 → 厂区负荷就是这么穿过负荷框的）；
       · 辅助舱三根折线的竖直段落在主链节点框内 ⇒ 线从「交流断路器」里穿出来。
     静态断言看不见这三类，只有把 SVG 真画出来、拆成盒子与线段两两求交才行。

   ★ 夹具口径：坐标抄自 pages.js:1282-1310，功率取一套典型工况
     （购电 400 / 负荷 254.7 / 充电 143.3 / 光伏 0）。
     那边改了常量，下面那条常量断言会红 —— 提醒同步本段，别让两边悄悄分叉。
   ===================================================================== */
t.head('[M] 一次接线图几何');

const vm = require('vm');
const uiBox = { console, document: { querySelector: () => null, querySelectorAll: () => [] } };
vm.createContext(uiBox);
vm.runInContext(read(path.join(ASSETS, 'ui.js'))
  + '\n;globalThis.__exp = { wiringDiagram, fmt, lineChart };', uiBox);
const WD = uiBox.__exp && uiBox.__exp.wiringDiagram;
const FMT = uiBox.__exp && uiBox.__exp.fmt;
const LC = uiBox.__exp && uiBox.__exp.lineChart;
t.ok(typeof WD === 'function' && !!FMT && typeof LC === 'function',
     'ui.js 能在沙箱里取出 wiringDiagram() / fmt / lineChart()（供几何实测）');

const geo = pagesJs.match(/const NW = (\d+), NH = (\d+), ROW = (\d+)/) || [];
t.eq([+geo[1], +geo[2], +geo[3]], [82, 50, 168],
     '夹具的 NW / NH / ROW 与 pages.js 一致（那边改坐标要同步本段）');

const NW = 82, NH = 50, ROW = 168;
const colX = (i) => 12 + i * (NW + 24);
const wNode = (id, name, i, y, opt) => Object.assign(
  { id, name, x: colX(i), y, w: NW, h: NH, live: false, value: null, unit: '' }, opt || {});
const WP = { grid_kw: 400.0, load_kw: 254.7, pv_kw: 0.0, battery_kw: -143.3 };
const wKw = (v) => `${FMT.num(v, 1)} kW`;
const wNodes = [
  wNode('grid', '电网', 0, ROW),
  Object.assign(wNode('meter', '关口电表', 1, ROW), { live: true, value: 400.0, unit: 'kW' }),
  wNode('tr', '变压器', 2, ROW),
  wNode('bus', '站内母线', 3, ROW),
  wNode('sw', '交流断路器', 4, ROW),
  Object.assign(wNode('conv', 'PCS 变流器', 5, ROW), { live: true, value: -143.3, unit: 'kW' }),
  wNode('contactor', '直流接触器', 6, ROW),
  Object.assign(wNode('cluster', '电池簇', 7, ROW), { live: true, value: 23.8, unit: '%' }),
  Object.assign(wNode('pv', '光伏逆变器', 2, ROW + 92, { branch: true }), { live: true, value: 0.0, unit: 'kW' }),
  Object.assign(wNode('load', '厂区负荷', 3, ROW + 92, { branch: true }), { live: true, value: 254.7, unit: 'kW' }),
  wNode('fire', '消防主机', 5, ROW - 96, { branch: true }),
  wNode('hvac', '空调 / 液冷', 6, ROW - 96, { branch: true }),
  wNode('sec', '门禁 / 安防', 7, ROW - 96, { branch: true }),
];
const wEdges = [
  { from: 'grid', to: 'meter', label: wKw(WP.grid_kw), active: true, color: 'c-grid' },
  { from: 'meter', to: 'tr', color: 'c-grid' },
  { from: 'tr', to: 'bus', color: 'c-grid' },
  { from: 'bus', to: 'sw', color: 'c-bat' },
  { from: 'sw', to: 'conv', color: 'c-bat' },
  { from: 'conv', to: 'contactor', color: 'c-bat' },
  { from: 'contactor', to: 'cluster', label: wKw(WP.battery_kw), active: true, color: 'c-bat' },
  { from: 'pv', to: 'bus', label: wKw(WP.pv_kw), active: false, color: 'c-pv' },
  { from: 'bus', to: 'load', label: wKw(WP.load_kw), active: true, color: 'c-load' },
  { from: 'bus', to: 'fire', aux: true, dashed: true, color: 'c-aux' },
  { from: 'bus', to: 'hvac', aux: true, dashed: true, color: 'c-aux' },
  { from: 'bus', to: 'sec', aux: true, dashed: true, color: 'c-aux' },
];

/* ---- 静态：三条避让逻辑必须还在（删了下面运行时也会红，这里是定位用） ---- */
t.ok(/Math\.abs\(a\.x - b\.x\) < 1/.test(uiJs),
     '★ 出线侧自适应：同列走竖连（固定右缘→左缘会绕回、横穿目标框）');
t.ok(/function safeX\(/.test(uiJs) && /hitsNode/.test(uiJs),
     '★ 折线竖直段避让：落点不碰任何节点框（否则线从框里穿出来）');
t.ok(/e\.aux/.test(uiJs) && /AUX_Y/.test(uiJs) && /aux: true/.test(pagesJs),
     '★ 辅助舱共用一条辅助母线（三根折线会横穿主链）');
t.ok(/Math\.min\(y1, y2\) - 33/.test(uiJs),
     '★ 功率数字抬到框外（放「中心 −8」正落在节点名称那一行）');

/* ---- 运行时：把 SVG 拆成文字盒子与线段，两两求交 ---- */
const CJK = /[\u4e00-\u9fa5]/;
const txtW = (s, f) => [...(s || '')].reduce((w, c) => w + (CJK.test(c) ? f : f * 0.56), 0);
const txtBox = (t) => {
  const w = txtW(t.s, t.fs);
  const x0 = t.anchor === 'middle' ? t.x - w / 2 : t.anchor === 'end' ? t.x - w : t.x;
  return { x0, x1: x0 + w, y0: t.y - t.fs * 0.8, y1: t.y + t.fs * 0.25, s: t.s };
};
const hit = (a, b) => a.x0 < b.x1 && a.x1 > b.x0 && a.y0 < b.y1 && a.y1 > b.y0;
const rOf = (n) => ({ x0: n.x, x1: n.x + n.w, y0: n.y, y1: n.y + n.h });

const wSvg = WD({ nodes: wNodes, edges: wEdges });
const wTexts = [];
let mm;
const reText = /<text x="([-\d.]+)" y="([-\d.]+)" text-anchor="(\w+)"[^>]*>([^<]*)<\/text>/g;
while ((mm = reText.exec(wSvg))) {
  const seg = wSvg.slice(mm.index, mm.index + 400);
  const fm = seg.match(/font-size="([\d.]+)"/);
  const it = { x: +mm[1], y: +mm[2], anchor: mm[3], s: mm[4], fs: fm ? +fm[1] : 11, kind: 'label' };
  wNodes.forEach((n) => {                      // 落在节点的名称行 / 数值行 ⇒ 自带文字
    if (Math.abs(it.y - (n.y + 20)) < 0.6 && Math.abs(it.x - (n.x + n.w / 2)) < 0.6) {
      it.kind = 'name'; it.owner = n.id;
    } else if (Math.abs(it.y - (n.y + n.h - 10)) < 0.6 && Math.abs(it.x - (n.x + n.w / 2)) < 0.6) {
      it.kind = 'value'; it.owner = n.id;
    }
  });
  wTexts.push(it);
}
const wSegs = [];
const rePath = /<path d="([^"]+)"/g;
while ((mm = rePath.exec(wSvg))) {
  const num = mm[1].match(/[-\d.]+/g).map(Number);
  for (let i = 2; i + 1 < num.length; i += 2) {
    wSegs.push({ x1: num[i - 2], y1: num[i - 1], x2: num[i], y2: num[i + 1] });
  }
}
t.eq([wTexts.filter(x => x.kind === 'label').length,
      wTexts.filter(x => x.kind !== 'label').length], [4, wNodes.length * 2],
     'SVG 解析正确：4 个功率数字 + 每个节点名称 / 数值各一行');

const wBoxes = wTexts.map(txtBox);
const pLabelOnRect = [], pTextOnText = [], pSegInRect = [];
wTexts.forEach((t0, i) => {
  if (t0.kind !== 'label') return;
  wNodes.forEach((n) => {
    if (hit(wBoxes[i], rOf(n))) pLabelOnRect.push(`「${t0.s}」压在「${n.name}」框上`);
  });
});
for (let i = 0; i < wBoxes.length; i++) {
  for (let j = i + 1; j < wBoxes.length; j++) {
    if (wTexts[i].owner && wTexts[i].owner === wTexts[j].owner) continue;  // 同框文字不参与
    if (hit(wBoxes[i], wBoxes[j])) {
      pTextOnText.push(`「${wBoxes[i].s}」× 「${wBoxes[j].s}」`);
    }
  }
}
wSegs.forEach((sg) => wNodes.forEach((n) => {
  for (let k = 1; k < 240; k++) {
    const tt = k / 240;
    const x = sg.x1 + (sg.x2 - sg.x1) * tt, y = sg.y1 + (sg.y2 - sg.y1) * tt;
    if (x > n.x + 2 && x < n.x + n.w - 2 && y > n.y + 2 && y < n.y + n.h - 2) {
      const d1 = Math.hypot(x - sg.x1, y - sg.y1), d2 = Math.hypot(x - sg.x2, y - sg.y2);
      if (d1 > 3 && d2 > 3) {
        pSegInRect.push(`(${sg.x1},${sg.y1})→(${sg.x2},${sg.y2}) 穿过「${n.name}」`);
        return;
      }
    }
  }
}));

t.eq(Array.from(new Set(pLabelOnRect)), [], '★ 运行时：功率数字不骑在任何一个节点框上');
t.eq(Array.from(new Set(pTextOnText)), [], '★ 运行时：功率数字不压任何文字（含节点名称与数值）');
t.eq(pSegInRect, [], '★ 运行时：没有一根线从节点框里穿出来');

/* =====================================================================
   [N] renderPage 静默刷新（2026-10-11 修「运行模式每 5 s 闪屏」）
   原实现：自动重刷一进来就 clear(content) + 插「加载中…」再 await 取数 ——
   清屏与换屏分属两个任务，中间浏览器把空屏画了出来，整页每 5 s 白一下。
   修后契约（下面用最小 DOM 桩跑**真实函数体**，四条路径各自断言）：
     1) quiet + 数据没变（outerHTML 相同）→ 一字节不动，旧屏定时器活着
     2) quiet + 数据变了 → 幕后建好后原子换屏；退旧定时器、保新定时器、恢复滚动
     3) quiet + 取数失败 → 保留上一屏（网络抖动不把数据页打成一屏红字）
     4) 非 quiet（点菜单主动换页）→ 清屏 + 加载中… + 换屏（原有体验不变）
   ★ 定时器记账用「入界快照 d0」：quiet 时 [0,d0) 是旧屏的、[d0,end) 是本次
     幕后构建的 —— 谁该退谁不该退，三条路各不相同，这里逐条钉死。
   ===================================================================== */
t.head('[N] renderPage 静默刷新');

(async () => {
  const a = appJs.indexOf('async function renderPage(opts)');
  const b = appJs.indexOf('function drawFooter()');
  if (!t.ok(a > 0 && b > a, '能从 app.js 抽出 renderPage 函数体')) return;

  /* —— 桩：renderPage 闭包里的每个自由变量都在这个 IIFE 作用域里 —— */
  function fakeEl(html) {
    return {
      _kids: [], outerHTML: html || '<div></div>', scrollTop: 0, textContent: '',
      appendChild(ch) { this._kids.push(ch); return ch; },
      get firstElementChild() { return this._kids[0] || null; },
    };
  }
  const content = fakeEl('<div id="content"></div>');
  const side = {};
  const $ = (sel) => (sel === '#content'
    ? content : (side[sel] = side[sel] || fakeEl(`<x></x>`)));
  const h = (tag, attrs) =>
    fakeEl(`<${tag} class="${(attrs && attrs.class) || ''}">${(attrs && attrs.text) || ''}</${tag}>`);
  const clear = (el) => { el._kids = []; return el; };
  const placeSegs = () => {};
  const highlightNav = () => {};
  let syncCalls = 0;
  const syncLiveTimer = () => { syncCalls++; };
  const disposers = [];
  const state = { page: 'overview', scenario: 'normal', user: { role: 'admin' },
                  mode: null, modes: [], engine: 'run', engineInfo: null, t: null };
  const MENU = [{ id: 'overview', name: '总览', sub: 'x', render: null }];

  // ★ 2026-10-17 合并新增：renderPage 的 catch 现在按 HTTP 状态分流，
  //   会调 isNotReady / renderNotReady 两个自由变量 —— 沙箱必须给桩，
  //   否则 eval 出的函数体一进 catch 就 ReferenceError（第一次合并就撞上了）。
  //   桩按真实语义打：只有 404 算「未就绪」，并把调用次数记下来供断言。
  let notReadyRendered = 0;
  const isNotReady = (e) => !!(e && e.status === 404);
  const renderNotReady = (e) => {
    notReadyRendered++;
    return fakeEl(`<div class="empty">未就绪:${(e && e.message) || ''}</div>`);
  };

  // eval 出的函数表达式的闭包链 = 本 IIFE 作用域 → 上面的桩名全部可解析
  const renderPage = eval('(' + appJs.slice(a, b) + ')');

  let oldDisposed = 0, newDisposed = 0;
  const oldNode = fakeEl('<div class="page">旧屏</div>');

  async function scene(render, setup) {
    disposers.length = 0; content._kids = []; content.scrollTop = 0; syncCalls = 0;
    MENU[0].render = render;
    if (setup) setup();
    await renderPage({ keepScroll: true, quiet: true });
  }

  /* 路径 1：quiet + 数据没变 → 不换屏 */
  await scene(async (ctx) => { ctx.onDispose(() => newDisposed++); return oldNode; },
    () => { content.appendChild(oldNode); content.scrollTop = 321;
            disposers.push(() => oldDisposed++); });
  t.ok(content._kids.length === 1 && content._kids[0] === oldNode,
       '★ 数据没变：旧屏原样保留（一个字节都不动，这就是不闪屏）');
  t.eq(content.scrollTop, 321, '★ 数据没变：滚动位置未被扰动');
  t.eq(oldDisposed, 0, '★ 数据没变：旧屏定时器仍活着');
  t.eq(newDisposed, 1, '★ 数据没变：被丢弃的幕后构建的定时器已退（不泄漏）');
  t.eq(syncCalls, 1, '静默刷新后 syncLiveTimer 已调');

  /* 路径 2：quiet + 数据变了 → 原子换屏 */
  const newNode = fakeEl('<div class="page">新屏</div>');
  oldDisposed = 0; newDisposed = 0;
  await scene(async (ctx) => { ctx.onDispose(() => newDisposed++); return newNode; },
    () => { content._kids = [oldNode]; content.scrollTop = 100;
            disposers.push(() => oldDisposed++); });
  t.ok(content._kids.length === 1 && content._kids[0] === newNode,
       '★ 数据变了：换上新树（幕后建好、一次性换，无空屏帧）');
  t.eq(content.scrollTop, 100, '★ 数据变了：keepScroll 恢复滚动位置');
  t.eq(oldDisposed, 1, '★ 数据变了：旧屏定时器已退');
  t.eq(newDisposed, 0, '★ 数据变了：新屏定时器未误杀（d0 记账正确）');
  t.eq(syncCalls, 1, '换屏后 syncLiveTimer 已调');

  /* 路径 3：quiet + 取数失败 → 保留上一屏 */
  oldDisposed = 0; newDisposed = 0;
  const origWarn = console.warn; console.warn = () => {};   // 本段预期会 warn 一条
  await scene(async (ctx) => { ctx.onDispose(() => newDisposed++);
                               throw new Error('网络抖动'); },
    () => { content._kids = [newNode]; disposers.push(() => oldDisposed++); });
  console.warn = origWarn;
  t.ok(content._kids.length === 1 && content._kids[0] === newNode,
       '★ 取数失败：保留上一屏（错误页不覆盖数据，下一拍自动重试）');
  t.eq(newDisposed, 1, '★ 取数失败：半途构建注册的定时器已退');
  t.eq(oldDisposed, 0, '★ 取数失败：旧屏定时器保留');

  /* 路径 4：非 quiet（主动换页）→ 清屏 + 加载中… + 换屏 */
  oldDisposed = 0; newDisposed = 0; syncCalls = 0;
  disposers.length = 0;
  content._kids = [newNode];
  let sawDuringFetch = '';
  disposers.push(() => oldDisposed++);
  MENU[0].render = async (ctx) => {
    sawDuringFetch = content._kids.length ? String(content._kids[0].outerHTML) : 'EMPTY';
    ctx.onDispose(() => newDisposed++);
    return fakeEl('<div class="page">换页后的屏</div>');
  };
  await renderPage({});
  t.ok(sawDuringFetch.includes('加载中'), '主动换页：取数前清屏并显示「加载中…」（原有体验不变）');
  t.ok(String(content._kids[0] && content._kids[0].outerHTML).includes('换页后的屏'),
       '主动换页：最终内容为新页');
  t.eq(oldDisposed, 1, '主动换页：旧屏定时器已退');
  t.eq(newDisposed, 0, '主动换页：新页定时器保留');
  t.eq(syncCalls, 1, '主动换页后 syncLiveTimer 已调');

  /* 路径 5：非 quiet + 404（用户主动点进一个"还没有数据"的页）→ 未就绪页
     ★ 这条是合并时**显式定下**的语义：远端 §68 只区分 quiet / 非 quiet，
       我们额外要求"404 ≠ 加载失败"必须对用户可见 —— 但只在**他要看的时候**。 */
  oldDisposed = 0; newDisposed = 0; syncCalls = 0; notReadyRendered = 0;
  disposers.length = 0; content._kids = [newNode];
  MENU[0].render = async () => {
    const err = new Error('运行模式还没有实时数据'); err.status = 404; throw err;
  };
  await renderPage({});
  t.eq(notReadyRendered, 1, '★ 主动换页遇 404：走未就绪页（不是「加载失败」）');
  t.ok(String((content._kids[0] || {}).outerHTML || '').includes('未就绪'),
       '★ 主动换页遇 404：屏上确实是空状态，不是红字「加载失败」');

  /* 路径 6：quiet + 404 + 还没有旧屏（首屏）→ 也要画未就绪
     ★ 否则首屏会**永远停在「加载中…」** —— 远端策略"失败就保留上一屏"在这里没有上一屏可保留 */
  oldDisposed = 0; newDisposed = 0; syncCalls = 0; notReadyRendered = 0;
  disposers.length = 0; content._kids = [];
  MENU[0].render = async () => {
    const err = new Error('还没有任何数据集'); err.status = 404; throw err;
  };
  await renderPage({ quiet: true, keepScroll: true });
  t.eq(notReadyRendered, 1, '★ 静默刷新但还没有旧屏：画未就绪（否则首屏永远停在「加载中…」）');

  /* 路径 7：quiet + 404 + 有旧屏 → 保留旧屏，**不**覆盖成未就绪
     ★ 与路径 6 合起来才是完整规则：静默刷新不抢用户的屏，首屏例外。 */
  oldDisposed = 0; newDisposed = 0; notReadyRendered = 0;
  disposers.length = 0; content._kids = [newNode]; disposers.push(() => oldDisposed++);
  MENU[0].render = async () => {
    const err = new Error('运行模式还没有实时数据'); err.status = 404; throw err;
  };
  const origWarn2 = console.warn; console.warn = () => {};
  await renderPage({ quiet: true, keepScroll: true });
  console.warn = origWarn2;
  t.eq(notReadyRendered, 0, '★ 静默刷新遇 404 且已有旧屏：不打成未就绪页（不抢正在看的数据）');
  t.ok(content._kids.length === 1 && content._kids[0] === newNode,
       '★ 静默刷新遇 404：旧屏原样保留');

  /* =====================================================================
     [O] lineChart x 轴刻度：按标签宽度自适应 + 共享日期只画一次
     起因（2026-10-12 用户截图）：run 模式（wall 口径）的历史曲线，x 轴是
     "2026-10-11 22:03:12" 这样的完整日期时间；旧逻辑固定取 labels.length/8
     ≈ 10 个刻度，相邻中心 ~92 单位 < 19 字符标签的 ~118 单位 ⇒ 两两重叠。
     功率与 SOC 两张图同源（都走 lineChart），修一处、这里两处都验。
     ★ 水平方向 preserveAspectRatio="none" 等比拉伸：中心距与字宽按同一
       比例放大，viewBox 内不重叠 ⇔ 显示出来不重叠，判据可在 viewBox 做。
     ===================================================================== */
  t.head('[O] lineChart x 轴刻度不重叠');

  const tickBoxes = (svg, H) => {
    // x 轴刻度 = y 恰为 H-8 的 text（y 轴刻度固定 x=padL-8 且 y 带 1 位小数，不会混入）
    const boxes = [];
    const re = new RegExp('<text x="([\\d.]+)" y="' + (H - 8) + '"[^>]*>([^<]*)</text>', 'g');
    let m;
    while ((m = re.exec(svg))) boxes.push({ x: +m[1], s: m[2] });
    return boxes;
  };
  const CHAR_W = 6.2;                                   // 与 ui.js 里同一个估算字宽
  const noOverlap = (boxes) => {
    const b = boxes.map(o => ({ l: o.x - o.s.length * CHAR_W / 2,
                                r: o.x + o.s.length * CHAR_W / 2 }))
                 .sort((p, q) => p.l - q.l);
    return b.every((o, i) => i === 0 || o.l >= b[i - 1].r - 0.5);
  };
  const p2 = x => String(x).padStart(2, '0');
  const mkChart = (labels, height) => LC({
    series: [{ name: 'P', label: 'P', color: 'var(--ok)',
               values: labels.map((_, i) => 100 + i) }],
    xLabels: labels, height,
  });

  // ① 复现用户截图工况：wall 口径 54 点（22:03 起每 10 s），完整日期时间
  const wallLabels = Array.from({ length: 54 }, (_, i) => {
    const s = 22 * 3600 + 3 * 60 + i * 10;
    return `2026-10-11 ${p2(Math.floor(s / 3600))}:${p2(Math.floor(s / 60) % 60)}:${p2(s % 60)}`;
  });
  const svgWall = mkChart(wallLabels, 330);
  const b1 = tickBoxes(svgWall, 330);
  t.ok(b1.length >= 4, `wall 19 字符标签出多个刻度（实出 ${b1.length} 个）`);
  t.ok(noOverlap(b1), `★ wall 长标签两两不重叠（${b1.length} 个刻度，旧逻辑 10 个必重叠）`);
  t.ok(!b1.some(o => o.s.includes('2026-')), '刻度文本已剥掉共享日期前缀（只剩时刻）');
  t.eq((svgWall.match(/>2026-10-11<\/text>/g) || []).length, 1,
       '共享日期只画一次（右上角，不逐刻度重复）');

  // ② sim 口径（HH:MM，5 字符）不回归：仍是 6~10 个刻度、不重叠
  const simLabels = Array.from({ length: 288 }, (_, i) => {
    const m = Math.floor(i * 10 / 60) % 1440;
    return `${p2(Math.floor(m / 60))}:${p2(m % 60)}`;
  });
  const svgSim = mkChart(simLabels, 330);
  const b2 = tickBoxes(svgSim, 330);
  t.ok(b2.length >= 6 && b2.length <= 10, `sim 短标签刻度数不回归（实出 ${b2.length} 个）`);
  t.ok(noOverlap(b2), 'sim HH:MM 标签两两不重叠');
  t.ok(!/>\d{4}-\d{2}-\d{2}<\/text>/.test(svgSim),
       'sim 标签无日期前缀时右上角不画日期');

  // ③ 跨午夜（日期不一致）：不误剥前缀，靠降低刻度密度保不重叠
  const crossLabels = wallLabels.map((s, i) => (i < 27 ? s : '2026-10-12' + s.slice(10)));
  const b3 = tickBoxes(mkChart(crossLabels, 330), 330);
  t.ok(b3.some(o => o.s.includes('2026-')), '日期不一致时保留完整标签（不误剥）');
  t.ok(noOverlap(b3), `★ 跨午夜 19 字符标签靠降密度保不重叠（实出 ${b3.length} 个）`);

  // ④ SOC 图（height=170）同源同修
  const b4 = tickBoxes(mkChart(wallLabels, 170), 170);
  t.ok(noOverlap(b4), `SOC 图（170 高）同样不重叠（实出 ${b4.length} 个刻度）`);

  /* ===================================================================
     [P] 「这一刻没有数据」≠「加载失败」—— 空状态 + 出路

     ★ 段号说明（2026-10-17 合并）：本段原编号为 [M]，而远端 §65 已经把 [M]
       用给了「一次接线图几何」、[N] 给了 renderPage 静默刷新、[O] 给了 lineChart
       x 轴 —— 所以合进来时改到 [P]。断言内容未变。

     ★ 为什么有这一段（2026-10-16 实跑撞出来的）：
       当时记的判断是「renderPage 没有 try-catch ⇒ 异常冒泡回 boot()
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
     =================================================================== */
  t.head('[P] 数据未就绪 ≠ 加载失败');

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
  t.ok(/location\.hash = '#\/devices'/.test(nrBody)
       && /去设备管理接入数据源/.test(nrBody),
       '出路①指向设备管理页 —— 采集的启停只有那一个地方，别在这里再造一个入口');
  t.ok(!/API\.liveStart\(/.test(nrBody),
       '★ 空状态里不许直接拉采集：那样"谁把它停的/起的"就查不清了');
  t.ok(/canWrite\(\)/.test(nrBody),
       '出路按钮按角色给（viewer 看不到操作入口）');
  t.ok(/重新加载/.test(nrBody),
       '出路②「重新加载」—— 采集侧补上数据后不必退出重登');
  t.ok(!/switchEngine|goPage\('sim'\)/.test(nrBody),
       '★ 空状态里不再有「切到仿真模式 / 去仿真页」的出路（产品界面没有这条路）');

  // 登录入口不能在「登录框已隐藏」之后把异常抛回去（那会把用户扔进空白页且无处申诉）
  const enterBody = sliceFn(appJs, 'async function enterApp');
  t.ok(/classList\.add\('hidden'\)[\s\S]*try \{/.test(enterBody),
       '★ enterApp 在隐藏登录框之后有 try 兜底（否则错误文案会写进看不见的 #login-err）');
  t.ok(/界面初始化失败/.test(enterBody),
       '初始化失败时留在界面里说清楚，而不是弹回登录框');

  // 合并后新增：404 与静默刷新交叉时的分流规则（两个分支的语义冲突，合并时显式定下）
  const rpBody = sliceFn(appJs, 'async function renderPage');
  t.ok(/o\.quiet && hasOld/.test(rpBody),
       '★ 静默刷新 + 已有旧屏 → 保留旧屏（一次抖动不该把数据页打空）');
  t.ok(/else if \(notReady\)/.test(rpBody),
       '★ 其余情形下 404 才画「未就绪」（首屏/主动导航不会永远停在"加载中…"）');

  // ------------------------------------------------------------------
  t.head('[Q] 两端接入：调度主站接入 / 站内设备接入 × 真实/模拟输入源');
  // ------------------------------------------------------------------
  // 这一版新增的语义：设备管理页不再只是「一份 Modbus 参数」，而是**两端**：
  //   上端（调度侧，EMS 是从站）/ 下端（设备侧，EMS 是主站），
  //   每一端都可以接真实源，也可以接模拟源。
  // 守的重点不是"画了这张卡"，而是三件容易退化成摆设的事：
  //   ① 形态要真的能存、能落盘、端侧读得到（否则就是个装饰性开关）；
  //   ② 「模拟源」必须被说出来（★ 不是真机），否则联调完切真源会忘；
  //   ③ 上端状态平台查不到，就不许点绿灯。
  const schemaSql = fs.existsSync(path.join(ROOT, '14', 'schema.sql'))
    ? read(path.join(ROOT, '14', 'schema.sql')) : '';
  const connPy = fs.existsSync(path.join(ROOT, '14', 'src', 'connconf.py'))
    ? read(path.join(ROOT, '14', 'src', 'connconf.py')) : '';
  const upPy = fs.existsSync(path.join(ROOT, '14', 'src', 'uplinkconf.py'))
    ? read(path.join(ROOT, '14', 'src', 'uplinkconf.py')) : '';
  const conn13 = fs.existsSync(path.join(ROOT, '13', 'src', 'device_conn_conf.h'))
    ? read(path.join(ROOT, '13', 'src', 'device_conn_conf.h')) : '';
  const linkP3 = fs.existsSync(path.join(ROOT, 'P3', 'src', 'access_link_conf.h'))
    ? read(path.join(ROOT, 'P3', 'src', 'access_link_conf.h')) : '';

  t.ok(/re\.compile\(r"\/api\/access"\),\s*h_access,\s*"viewer"/.test(apiPy),
       '后端有 GET /api/access（接入装配总览，viewer 可读）');
  t.ok(/re\.compile\(r"\/api\/access\/upstream"\),\s*h_access_upstream_put,\s*"operator"/.test(apiPy),
       '后端有 PUT /api/access/upstream（改上端 = operator）');
  t.ok(/CREATE TABLE IF NOT EXISTS upstream_link/.test(schemaSql),
       '上端链路有独立表 —— 两端参数集没有交集，合用一张表只能靠一堆 NULL 列');
  t.ok(/source_kind\s+TEXT NOT NULL DEFAULT 'real'/.test(schemaSql),
       '下端形态落在 device_conn 上（按设备记），且老库补列后默认 real');

  // 形态不是假的：它必须真的决定端侧连谁
  t.ok(/def target\(/.test(connPy) && /sim_host/.test(connPy),
       '★ 端侧实际连的地址由 target() 统一算出（real→host / sim→sim_host）');
  t.ok(/kind == "real" and not host/.test(connPy),
       '★ host 的必填性跟着形态走 —— 接模拟源时允许没有真实设备地址');
  t.ok(/active\.upstream/.test(upPy),
       '上端参数落盘成 active.upstream（与 active.csv / active.conn 同目录）');

  // 端侧真的读得懂，否则平台只是写了一个没人看的文件
  t.ok(/source_kind/.test(conn13) && /conn_target_host/.test(conn13),
       '13/ 的接入参数 loader 认形态，并据此给出实际目标地址');
  t.ok(/active\.upstream/.test(linkP3) && /listen_port/.test(linkP3),
       'P3/ 的网关读 active.upstream（否则界面上配的 2404 只是个摆设）');

  // 前端：两张卡 + 每台设备的形态
  t.ok(/accessUpSection\(/.test(pagesCode) && /accessDownSection\(/.test(pagesCode),
       '设备管理页渲染「调度主站接入 / 站内设备接入」两部分（不用「上端/下端」当标题）');
  t.ok(/API\.access\(/.test(pagesJs) && /API\.saveUpstream\(/.test(pagesJs),
       '页面走 /api/access 与 /api/access/upstream，不自己拼数据');
  t.ok(pagesCode.includes("source_kind: $('#pm-kind').value"),
       '★ 每台设备的接入卡有形态选择，且真的提交到保存接口');
  t.ok(/title: '接入形态'/.test(pagesCode),
       '②点表 有「接入形态」列（v1.8 由台账并入）—— 看不出谁是模拟源的话，等于没记');
  t.ok(/不是真机/.test(pagesCode),
       '★ 形态为 sim 时必须明说「不是真机」（联调完忘切回来的代价是真数据被当真）');
  t.ok(/平台不回采上端连接状态/.test(pagesCode),
       '★ 上端点不出绿灯：网关不回报连接数，界面上就不许画一个查不到的状态');
  t.ok(/fails\.length/.test(pagesCode) && /ok}\/\$\{ids\.length\}/.test(pagesCode),
       '★ 下端批量切换必须报「k/N 台成功」—— 改一半时"看起来全成功"最危险');
  t.ok(/按设备逐台记/.test(pagesCode),
       '★ 下端形态按设备记（不是整站一个开关）：调试期常是真机与模拟器混用');

  // 2026-10-18 自检修掉的三处（都是"看起来对、其实在编结论"那一类）
  t.ok(/LEFT JOIN device_conn/.test(apiPy),
       '★ 下端汇总以 device 为左表 —— 只数 device_conn 会漏掉"一行都没配"的设备，'
       + '汇总台数少于台账台数且看不出来');
  t.ok(/kindBadge\(r\.state \|\| 'unknown'\)/.test(pagesCode) && /state: 'unknown'/.test(pagesCode),
       '★ 拿不到 /api/access 时清册列给「未知」，不降级成「未配置」（查不到 ≠ 未接）');
  t.ok(!/binarySeg\(/.test(pagesCode) && !/KIND_OPTS/.test(pagesCode),
       '★ 没有"二选一分段器"了：形态由两块各自的动作按钮切，不存在一个共用的开关');
  t.ok(pagesCode.includes('把 ${ids.length} 台设备全部改为模拟输入源')
       && /window\.confirm\(/.test(pagesCode),
       '★ 整片切到模拟输入源要二次确认 —— 误触会把整站数据换成替身，而界面看上去一切正常');
  t.ok(!/\*\*从站\*\*|\*\*主站\*\*/.test(pagesCode),
       '界面文案里的 **强调** 已清掉 —— card()/toast() 走 text，星号会原样显示');

  /* ------------------------------------------------------------------
     [R] 输入端只长在设备管理页

     产品口径（2026-10-18，v1.7 修订）：
       仿真/现场**不是模式**，而是「输入源」—— 设备管理页上的开关。
       ★ v1.7 把它从"整站一个"改成"两端各一个"：调度主站接入与站内设备
         接入是两条独立通道，对端可以一头真一头假。
       其他页面是正式运行页面：有数据就渲染，没数据就指向设备管理。
       所以这一段的判据不是"有没有仿真功能"，而是**开关只长在一个地方**，
       且**一端的开关不许动另一端**。
     ------------------------------------------------------------------ */
  t.head('[R] 输入端只长在设备管理页，且两端互不相干');

  // ---- 后端：一个开关写穿两端 ----
  t.ok(backRoutes.includes('/api/data-source'), '后端提供 /api/data-source');
  t.ok(/def h_data_source\b/.test(apiPy) && /def h_data_source_set\b/.test(apiPy),
       'GET / POST 两个处理函数都在');
  t.ok(/if end not in ENDS/.test(apiPy) && /一次只切一端/.test(apiPy),
       '★ 切输入源必须指明 end，没有"两端一并切"的默认值（那一步会静默换掉整站数据源）');
  t.ok(/UPDATE device_conn SET source_kind = \?/.test(apiPy),
       '★ 切下端要写穿到 device_conn 的形态（否则界面切了、端侧还连旧对端）');
  // 上端必须是 **upsert**。原判据钉的是裸 UPDATE 的文本，实现换成 upsert 就假红；
  // 而真正要证的是语义：**没有行就插、有行就改 kind**。裸 UPDATE 在空表上影响 0 行、
  // 读侧 `_link_row()` 又回落成默认 `kind='real'` ⇒ 界面「文件说模拟、上端卡说现场」
  // （CHANGES §79.5）。所以两个方向写进同一条：要 upsert，且不许退回裸 UPDATE。
  t.ok(/ON CONFLICT\(link_id\) DO UPDATE SET kind = excluded\.kind/.test(apiPy)
       && !/UPDATE upstream_link SET kind = \?/.test(apiPy),
       '★ 切数据源要写穿上端（upsert：空表时也登记上；不许回到「空表上静默空操作」的裸 UPDATE）');
  t.ok(/_rewrite_conn_for_kind\(/.test(apiPy) && /uplinkconf\.save\(/.test(apiPy),
       '切换后重写 active.conn / active.upstream —— 端侧读的是文件，不是库');
  t.ok(/先停止采集再切换数据源/.test(apiPy),
       '★ 采集中拒绝切换（运行中换对端，采集器还在按旧参数读写）');

  // ---- 后端：采集器只有一个，两边只是参数不同 ----
  t.ok(/"control": True/.test(enginePy) && /"control": False/.test(enginePy),
       'sim / real 各有一份 control 标记（替身可闭环、真机只读）');
  t.ok(/--control/.test(enginePy) && /src\["control"\]/.test(enginePy),
       '★ 是否下发由数据源决定，不在别处再写一遍 if');
  t.ok(/modbus_slave\.py/.test(enginePy) && /_wait_tcp\(/.test(enginePy),
       '模拟数据源会拉起 13/ 设备替身，并以"端口能连上"为就绪判据');
  t.ok(/_loopback\(/.test(enginePy),
       '外部替身（非本机地址）不去拉，直接连 —— 不能把别人机器上的模拟器也起一遍');
  t.ok(/现场设备连不上/.test(enginePy),
       '★ 现场源先做 TCP 预检，连不上直接给结论（而不是"已启动、然后一直没有数据"）');
  t.ok(/形态是「模拟源」，与所选的「现场数据源」不一致/.test(enginePy),
       '★ 数据源与下端形态不一致时拒绝启动，不自动改写接入参数');

  // ---- 前端：卡片在设备管理页 ----
  // api.js 定义方法，pages.js 才是调用点 —— 两处都要在，不然是"定义了没人用"。
  t.ok(apiJs.includes("dataSource:") && apiJs.includes("setDataSource:")
       && pagesCode.includes("API.dataSource("),
       '前端有数据源读写接口，且设备管理页真的在用');
  t.ok(/setDataSource:\s*\(end, kind\)/.test(apiJs),
       '★ 切输入源必须带 end（一次只切一端）—— 不存在"两端一并切"的调用形式');
  const upBody = sliceFn(pagesCode, 'function accessUpSection');
  const downBody = sliceFn(pagesCode, 'function accessDownSection');
  /* ★ v1.14：上端那两份输入（① 接入参数 / ② IOA 转发表）搬进了
     「接入调度主站」九步向导的第 6 步。所以上端的判据要打这里 ——
     打 accessUpSection 已经打不到了，那里现在只剩一个入口按钮。 */
  const wzBody = sliceFn(pagesCode, 'function accessWizard');
  t.ok(upBody.length > 0 && downBody.length > 0,
       '两大部分各自是一段完整实现（不是一个函数里按参数分叉）');
  t.ok(/启动采集/.test(downBody) && /停止采集/.test(downBody)
       && /清空实时数据/.test(downBody),
       '★ 采集的启停与清空挂在「站内设备接入」上 —— 采集器只属于下端，'
       + '放到上端会让人以为停采集就断了调度');
  t.ok(!/启动采集/.test(upBody), '「调度主站接入」里没有采集启停按钮');
  // ★ v1.7.x 翻转：原来这里守的是"两部分各有一个「切到模拟」按钮"。
  //   那两个按钮随「真实/模拟输入源」卡片一起删了 —— 端的形态改由页尾
  //   「模拟输入源」面板**一次管两端**。守的东西因此反过来：区段里不许
  //   再出现"只切一端"的入口，否则又会出现"只开了一端、页面说模拟中"。
  t.ok(!/switchSrcBtn|setDataSource/.test(upBody + downBody),
       '★ 两端区段里没有"只切一端"的入口 —— 形态只在页尾面板切（一次管两端）');
  t.ok(/API\.saveConn\(id/.test(downBody),
       '下端仍按台改 /api/devices/{id}/conn（整片切与逐台编辑都用它）');
  t.ok(/locked/.test(downBody) && /!canWrite \|\| running/.test(downBody),
       '★ 采集中下端的切换按钮置灰（而不是点了再看 409）');
  // ★ v1.11：这句话从下端区段的正文挪进了「启用模拟源」按钮的 title ——
  //   它是"这一页唯一会骗人的地方"，不能因为撤灰字就一起撤掉。
  t.ok(/不是现场装置，也不是真机/.test(pagesCode),
       '模拟输入源必须写明「不是现场装置 / 不是真机」—— 那是这一页唯一会骗人的地方'
       + '（v1.11 起在「启用模拟源」的 title 上）');
  t.ok(/mixBanner/.test(pagesCode) && /ds\.mix/.test(pagesCode),
       '★ 两端形态组合的危险提示由后端算（ds.mix），界面不自己再推一遍');

  // ---- 其他页面：正式运行页面，只有"去设备管理" ----
  // 状态条删除后，采集告警不能跟着消失：它挪到了设备管理页（启停也在那一页）。
  t.ok(/采集告警：\$\{col\.error\}/.test(pagesCode),
       '★ 采集异常在设备管理页里显示 —— 状态条删了，异常不能变成没人知道');
  t.ok(!/API\.liveStart\(|API\.liveStop\(/.test(appJs),
       '★ 整个 app.js（页面外壳）不再直接启停采集');
  t.ok(/data_source_name/.test(appJs),
       '页脚用数据源名字，而不是"仿真/正式"这种模式名');
  t.ok(!/'仿真模式'/.test(pagesCode) && !/'仿真模式'/.test(appJs),
       '★ 正式运行页面不出现「仿真模式」字样');

  /* ------------------------------------------------------------------
     [S] 每端两份输入：① 接入参数 / ② 点表·转发表（v1.7.x）

     用户看图（两端各两份输入）后问的第一句是「网页里怎么看不出来」。
     所以这一段的判据不是"有没有这两块"，而是**页面上看不看得出来**：
       · 每一端内部有两个显式分节（① 接入参数 / ② 点表或转发表），
         不是一个卡片里字段随便排；
       · 两份都是**真的**：上端的转发表要能读出 24 上送 + 6 下行，
         下端的每台要有目标 / 从站 / 节奏 / 点数；
       · 上端转发表**只读**（真相源在 P3/），下端点表可编辑且能跳转。
     ------------------------------------------------------------------ */
  t.head('[S] 每端两份输入：接入参数 + 点表 / 转发表');

  // ① 两个显式分节，两部分各有一份
  t.ok(/function phase\(/.test(pagesCode), '有 phase() 分节标题助手');
  t.eq((pagesCode.match(/phase\('①', '接入参数'/g) || []).length, 1,
       '下端的「① 接入参数」用 phase() 分节（每台一份）');
  /* 这条不许删：对上端来说「接入参数」和「转发表」仍然是两条独立的事，
     只是搬到了向导里。判据跟着搬，不是把检查去掉。 */
  t.ok(wzBody.includes('① 接入参数') && wzBody.includes('② IOA 转发表'),
       '★ 上端的 ① 接入参数 / ② IOA 转发表 在向导第 6 步（不是消失了）');
  t.ok(/phase\('②', '点表'/.test(downBody), '★ 下端有「② 点表」小节');
  t.ok(wzBody.includes('全站唯一一份'), '上端参数写明是「全站唯一一份」');
  t.ge((downBody.match(/每台一份/g) || []).length, 2,
       '★ 下端两份都写明「每台一份」（地址参数与点表都是逐台的）');

  // ② 上端第二份：转发表，真的读出来，且只读
  t.ok(backRoutes.includes('/api/upstream/point-map'),
       '后端提供 GET /api/upstream/point-map');
  t.ok(/re\.compile\(r"\/api\/upstream\/point-map"\),\s*h_upstream_point_map,\s*"viewer"/.test(apiPy),
       '转发表接口 viewer 可读（现场对账不需要操作权限）');
  // ---- v1.6 起**可以写**：这一段整段翻转 ----
  //   原来守的是「不许有 PUT」。那个担心（怕出现第二个真相源）是对的，
  //   处置却错了：正确答案是把真相源**收敛成一个文件**（平台写
  //   active.pointmap.csv、网关按约定路径读它），而不是让界面永远只读。
  //   所以下面改成正向判据，并补一条**新的**反向守卫：平台绝不回写
  //   P3/ 仓库里那份默认底本（那只是随包样例）。
  t.ok(/h_upstream_point_map_put/.test(apiPy) && /h_upstream_point_map_delete/.test(apiPy),
       '★ 能写也能恢复：PUT 保存 / DELETE 恢复内置默认（两者必须成对 —— '
       + '只给写不给删，等于只能往前不能回头）');
  t.ok(/def h_upstream_point_map_put[\s\S]{0,900}upmapconf\.normalize\(raw_rows\)/.test(apiPy),
       '★ 保存先过 upmapconf 校验**再**落盘（放端侧去拒 = 人在这儿操作、雷在那头炸）');
  t.ok(/upmapconf\.save\(rows, now, who\)[\s\S]{0,500}emsdb\.executemany/.test(apiPy),
       '★ 顺序是 落盘 → 写库，失败整体回滚（不会出现"文件新、库里旧"两个答案）');
  t.ok(apiJs.includes('upstreamPointMap:') && apiJs.includes("get('/api/upstream/point-map')"),
       'api.js 走 GET /api/upstream/point-map');
  t.ok(apiJs.includes('saveUpstreamPointMap:') && apiJs.includes("put('/api/upstream/point-map'")
       && apiJs.includes('resetUpstreamPointMap:') && apiJs.includes("del('/api/upstream/point-map'"),
       'api.js 有保存与恢复两个方法');
  t.ok(/upstreamPointMapCsvUrl/.test(apiJs),
       'api.js 有 CSV 导出直链（模板 / 当前表两个）');
  t.ok(/API\.upstreamPointMap\(\)/.test(pagesCode), '页面真的读它（不是写了一段没人看的接口）');
  t.ok(/API\.saveUpstreamPointMap\(st\.rows\)/.test(pagesCode),
       '★ 界面真的保存（不是只能看 —— 这正是这一次要补的东西）');
  t.ok(pagesCode.includes('24 上送 + 6 下行'),
       '★ 界面上写明口径：24 上送 + 6 下行');

  /* ---- 转发表能**导入 CSV**（调度下发那张表进软件的第一步）----
     这一段守的不是"按钮在不在"，是那三件容易做错的事：
       ① **导入 ≠ 提交**：只进编辑区，落盘仍只走「保存转发表」；
       ② **不抄下端的行数约束**：上端行序无意义（按 IOA 定位），
          照抄"应为 N 行"就是一条不存在的约束，会把人挡在门外；
       ③ **解析口径必须与 14/ 的 parse 同**：BOM / # 注释 / ioa 表头 /
          note 含逗号 / 空列补默认，一条都不能自己发明 —— 前端比后端
          宽容或严格，都会变成「界面导进来 30 行、点保存被拒在第 7 行」。
     最后几条是**真跑**那个函数（切出来 new Function），不是扫文本。 */
  t.ok(pagesCode.includes('导入 CSV'), '★ 上端转发表有「导入 CSV」按钮');
  t.ok(/fileInput\.click\(\)/.test(pagesCode)
       && /head, tool, fileInput, editorHost/.test(pagesCode),
       '★ fileInput 真挂进 DOM（不挂上去，点按钮时灵时不灵）');
  {
    const m = pagesCode.match(/function importCsv\(file\)\s*\{[\s\S]*?\n    \}\n/);
    const body = m ? m[0] : '';
    t.ok(body.length > 300, 'importCsv 函数体抓到（判据要打在真正的实现上）');
    t.ok(!/API\.saveUpstreamPointMap/.test(body),
         '★ 导入不落盘（函数体里没有保存调用 —— 导入是编辑动作，不是提交）');
    t.ok(/dirtyPM\(\)/.test(body),
         '★ 覆盖编辑区前先问 dirtyPM（只有真有未保存改动才确认，不是每次都弹）');
    t.ok(!/names\.length|行序即索引/.test(body),
         '★ 不抄下端的「行数必须相等」——上端行序无意义，多一行少一行都合法');
  }
  {
    const a = pagesCode.indexOf('const UP_KINDS = [');
    const b = pagesCode.indexOf('function upPmEditor(canWrite, hook) {');
    let parseCsv = null;
    if (a >= 0 && b > a) {
      try { parseCsv = new Function(pagesCode.slice(a, b) + '\nreturn upPmParseCsv;')(); }
      catch (e) { parseCsv = null; }
    }
    t.ok(typeof parseCsv === 'function',
         '★ upPmParseCsv 能独立执行（切出来那段不依赖页面其它东西，否则没法验它）');
    if (typeof parseCsv === 'function') {
      const one = parseCsv('\ufeff# 说明\nioa,kind,group,name,src,scale,offset,unit,note\n'
        + '4004,MEAS_FLOAT,1,关口功率,MEAS.P_GRID,1,0,kW,购电为正,倒送为负\n');
      t.eq(one.rows.length, 1, '解析：剥 BOM + 跳 # 注释 + 跳 ioa 表头后 1 行');
      t.eq(one.rows[0].note, '购电为正,倒送为负',
           '★ note 可含逗号（取最后一段）—— 与 14/src/upmapconf.parse 同口径');
      t.eq(one.filled, 0, '九列填全的行不补默认值');

      const blank = parseCsv('4004,MEAS_FLOAT,,关口功率,,,,kW,\n');
      t.eq([blank.rows[0].group, blank.rows[0].scale, blank.rows[0].offset], ['1', '1', '0'],
           '★ 空分组/系数/偏置补 1/1/0 —— 后端 _as_int/_as_float 对空串是**报错**，'
           + '不走默认值；不补就成了「导进来 30 行、点保存被拒在第 1 行」');
      t.eq(blank.filled, 1, '补过默认值的行数要统计（必须能告诉用户，不能静默改数据）');

      const down = parseCsv('5001,CMD_SINGLE,,PCS 启停,,1,0,,\n');
      t.eq(down.rows[0].group, '0', '★ 下行点空分组补 0（与内置默认表一致：下行 group=0）');

      const short = parseCsv('4001,MEAS_FLOAT,1,负荷\n'
        + '4002,MEAS_FLOAT,1,光伏,MEAS.P_PV,1,0,kW,\n');
      t.eq([short.rows.length, short.errors.length, short.errors[0].line], [1, 1, 1],
           '★ 列数不足的行跳过、但**记下行号**（静默丢行 = 「导进来 24 行、实际 30 行」）');

      const def = parseCsv(read(path.join(ROOT, 'P3', 'data', 'point_map_default.csv')));
      const upN = def.rows.filter(r =>
        /^(MEAS_|SINGLE_POINT|DOUBLE_POINT|COUNTER)/.test(r.kind)).length;
      t.eq([def.rows.length, def.errors.length, def.filled], [30, 0, 0],
           '★ 内置默认表整表能导回来：30 行 / 0 错 / 0 补默认');
      t.eq([upN, def.rows.length - upN], [24, 6], '导回来仍是 上送 24 / 下行 6');
    }
  }
  t.ok(pagesCode.includes('--point-map') && pagesCode.includes('编辑转发表'),
       '★ 既讲清网关的加载顺序（--point-map → 约定路径 → 内置默认表），'
       + '也给出现场能动手的入口 —— 只说"改 CSV"等于让人对着命令行想办法');
  t.ok(/r\.direction === 'down' \? '下行' : '上送'/.test(pagesCode),
       '转发表逐行标方向 —— 30 行里 6 行是下行，不标等于让人自己数 IOA 区间');
  t.ok(/不会热更新/.test(pagesCode) || /重启/.test(pagesCode),
       '★ 明说"改完要重启网关才生效"（不然人会以为存完就生效了）');
  t.ok(!/open\(UPLINK_PM_CSV[\s\S]{0,20}['"]w/.test(apiPy),
       '★ 新的反向守卫：平台不写 P3/ 仓库里的默认底本（被回写后，下一次'
       + '拿它当基准对照就失效了）');

  // ③ 下端两份：每台的地址参数 + 点表，都从 /api/access 来
  t.ok(/poll_period_ms/.test(downBody), '★ 每台接入参数表带轮询周期');
  t.ok(/r\.target/.test(downBody), '★ 目标列取 target（端侧实际连的地址），不是把两栏都列一遍');
  t.ok(/point_rows/.test(downBody), '★ 每台点表表带点数');
  t.ok(/point_rows/.test(apiPy) && /poll_period_ms/.test(apiPy),
       '后端 /api/access 的 items 真的带这两个字段');
  t.ok(/未配置 · 回落内置默认表/.test(downBody),
       '★ 0 点要说「回落内置默认表」—— 不说的话，人会以为这台没在采');
  t.ok(/\(id\) => \{ if \(id\) access\.select\(id\); else editorModal\.open\(\); \}/.test(pagesCode),
       '★ 表里的「编辑」选中那一台；空库时那个「新增第一台设备」直接开弹层');
  t.ok(/return \{ node: shell, load, select,/.test(pagesCode),
       'pointAccess 暴露 select（选中与渲染只有它一个所有者）');
  t.ok(/setTitle\(`逐台编辑 · \$\{id \|\| '未选中'\}\`\)/.test(pagesCode),
       '★ 弹层页头写出「正在改哪一台」（最晚也得在点开那一刻知道改的是谁）');
  t.ok(/shell\.appendChild\(root\)/.test(pagesCode) && !/card\(null, null, root\)/.test(pagesCode),
       '★ 编辑器不再套一层白卡外壳：弹层本身就是一张页，'
       + '白盒子套白盒子看不出哪一层是内容');
  t.ok(!/card\(`逐台编辑/.test(pagesCode),
       '★ 编辑器卡片自己没有标题：设备号只在弹层页头出现一次，'
       + '两处都写会让人不知道该看哪个、改的是不是同一台');
  t.ok(/if \(HK\.onOpen\) HK\.onOpen\(\);/.test(pagesCode),
       '★ 点「编辑」打开弹层（原来是滚到页面末尾 —— 滚过去还得自己在长页面里找）');

  /* ------------------------------------------------------------------
     2026-10-18 第三次改这一页：台账**不是删掉，是并进 ②点表**。
       之前：清册（有哪些设备）单独一张卡在最下面，与 ②点表（读哪些寄存器）
       隔着两屏。判断"这台的点表该不该改"要看"这台是什么型号、有没有在采"，
       两处分开就只能靠设备号在脑子里对。
     ★ 判据不是"台账没了"，而是：
       · 清册八列真的在 ②点表 那张表里，且排在「点数 / 状态 / 编辑」之前；
       · 逐台编辑不在页面上铺开（9 列 × 32 行会把整页撑成一条缝），
         改成**跟页面一样大**的弹层，点「编辑」打开、Esc 或「关闭」返回；
       · 弹层按 key 复用 —— 页面来回切不会每切一次往 body 上多挂一层。
     ------------------------------------------------------------------ */
  t.head('[T] 台账并入 ②点表 + 逐台编辑改成全页弹层');

  const devBody = sliceFn(pagesCode, 'async function devices');

  // ① 台账并进去了（判据是"八列在点表里"，不只是"卡片没了"）
  t.ok(!/card\('台账'/.test(pagesCode), '台账不再单独占一张卡');
  t.ok(/const LEDGER_COLS = \[/.test(downBody), '★ 清册八列集中定义（列单一处，不在两处各写一遍）');
  ['设备号', '类型', '接入形态', '额定功率', '额定容量', '状态', '当前功率', '说明']
    .forEach(c => t.ok(downBody.includes(`title: '${c}'`),
                       `★ ②点表 带清册列「${c}」`));
  const pmSeg = downBody.slice(downBody.indexOf('const pmTable'));
  t.ok(pmSeg.indexOf("title: '说明'") < pmSeg.indexOf("title: '点数'")
       && pmSeg.indexOf("title: '点数'") < pmSeg.indexOf("title: '动作'"),
       '★ 列序是 清册八列 → 点数 → 点表状态 → 编辑（先知道"是什么"才知道"该不该改"）');
  t.ok(/rt\[r\.device_id\]/.test(downBody) && /rtPower/.test(downBody),
       '★ 当前功率跟着一起并进去（清册与点表合一，功率才跟得上）');
  t.ok(/accItems\.length \? accItems/.test(downBody),
       '★ acc 缺失时用设备清册兜底 —— 点表空着等于"整页没设备"，那是编结论');

  // ② 逐台编辑在弹层里，不在页面上
  t.ok(/srcHost,\s*\n\s*card\('点表口径'/.test(pagesCode),
       '★ 页面只留两路输入源 + 点表口径（逐台编辑不在页面上铺开）');
  t.ok(/editorModal\.body\.appendChild\(access\.node\)/.test(pagesCode),
       '★ 编辑器真的住进弹层（不是页面上留一份、弹层里再复制一份）');

  // ③ 弹层与页面一样大 —— 32 行 × 9 列的点表塞进小窗等于在邮票上看表格
  t.ok(/function modal\(opts\)/.test(uiCode), '有 modal() 全页弹层助手');
  t.ok(/position: fixed; inset: 0/.test(css), '★ 弹层跟页面一样大（铺满视口，不是居中小窗）');
  t.ok(/\.mdl-body \{ flex: 1; overflow-y: auto;/.test(css),
       '★ 弹层内部自己滚（内容比一屏高时不顶死、也不撑长背后的页面）');
  t.ok(/body\.mdl-open \{ overflow: hidden/.test(css),
       '弹层开着时背后的页面不跟着滚（否则关掉之后位置不对）');
  t.ok(/z-index: 90/.test(css) && /right: 22px; bottom: 22px; z-index: 999/.test(css),
       '★ 弹层 z-index 低于 toast：保存成功的提示必须浮在弹层之上，'
       + '不然点完「保存点表」看不到反馈，会以为没存上');

  // ④ 开与关都要有，且弹层不会一页一挂
  t.ok(/text: '关闭'/.test(uiCode), '★ 有关闭按钮（不依赖键盘，鼠标也能出去）');
  t.ok(/e\.key === 'Escape'/.test(uiCode), '★ Esc 可关');
  t.ok(/mdl-' \+ o\.key/.test(uiCode) && /key: 'dev-editor'/.test(devBody),
       '★ 弹层按 key 复用 —— 页面来回切不会每切一次往 body 上多挂一层');
  t.ok(/body\.scrollTop = 0/.test(uiCode), '每次打开都从头看（别停在上一次滚到的地方）');
  t.ok(/ctx\.onDispose\(\(\) => editorModal\.close\(\)\)/.test(pagesCode),
       '★ 换页时把弹层收起来 —— 它挂在 body 上、不在 #content 里，'
       + '不显式关就会盖在新页面上，看着像"点菜单没反应"');

  // ⑤ 空库不是死路：0 台设备时两条入口都要通
  const paBody = sliceFn(pagesCode, 'function pointAccess');
  t.ok(paBody.indexOf('const newCtl') > 0
       && paBody.indexOf('const newCtl') < paBody.indexOf('if (!st.id)'),
       '★ 新增三件套建在 st.id 判空**之前** —— 否则空库时弹层里只剩一句'
       + '「请先新增一台」，而页面上又没有行可点，第一台设备永远建不出来');
  t.ok(/text: '新增第一台设备'/.test(downBody),
       '★ ②点表 是空表时留了新增入口（0 台设备时这一页没有任何「编辑」可点）');
  t.ok(/else editorModal\.open\(\)/.test(pagesCode),
       '★ 那个入口直接开弹层（不是把空 id 丢给 select 然后什么都不发生）');

  // =================================================================
  t.head('[U] 模拟输入源：页尾一个面板管两端 · 一键填好 · 一键换一套 · 关掉不留痕');
  // =================================================================
  /* ★ 这一段守的是"界面上那四个动作真的存在，且各自只用自己的那条接口"：

       启用 / 关闭  →  POST /api/sim-source {enable}      ← **一次管两端**
       换一套       →  POST /api/sim-source {rotate}      ← 只换数据，不换形态
       整片切       →  PUT  /api/devices/{id}/conn        ← 按台调，另一个粒度
       逐台切一端   →  POST /api/data-source {end, kind}

     四个动作分属三个接口，是因为它们的**粒度不同**，不是随手拆的：
     "下端模拟 + 上端真实"是最危险的一种组合（替身数据被当现场值上送），
     所以**进模拟必须一次点两端**；而"PCS 接真机、电表先接模拟器"是调试常态，
     所以按台调的口子要留着。
  */
  const simBody = sliceFn(pagesCode, 'function simSourcePanel');
  const apiSim = /simSource:\s*\(\)\s*=>\s*get\('\/api\/sim-source'\)/.test(apiJs);
  const apiSetSim = /setSimSource:\s*\(body\)\s*=>\s*post\('\/api\/sim-source'/.test(apiJs);
  t.ok(apiSim && apiSetSim, '★ api.js 有 simSource / setSimSource 两个方法'
       + '（与 setDataSource 分工不同：那个按端调，这个一键管两端）');
  t.ok(simBody.length > 0, '页尾「模拟输入源」是一段独立实现');
  // 两个按钮都走同一个 act() → API.setSimSource(body)，只是 body 不同。
  // 这样"点了哪个按钮"与"发了什么请求"只在一处对应，不会各写一遍。
  t.ok(/API\.setSimSource\(body\)/.test(simBody),
       '★ 启用 / 关闭 / 换一套 都收在 act() 里的一个 setSimSource(body)');
  t.ok(/act\(\{ enable: !on \}\)/.test(simBody),
       '★ 启用 / 关闭发 {enable}（一个开关管两端）');
  t.ok(/act\(\{ rotate: true \}\)/.test(simBody),
       '★ 换一套发 {rotate: true}（不是把 enable 再发一遍）');
  t.ok(!/API\.setDataSource/.test(simBody),
       '★ 面板里没有 setDataSource —— 它一次只切一端，混进来就会出现'
       + '「只开了一端、页尾却说模拟中」');

  // ⑥ 在飞期间不许再点（2026-10-19 体检：一次「启用」落了 5 条相同审计）
  //    拿掉这几条守卫，"用户看没反应就连点"会重新变成 5 次配置写入 + 5 条审计。
  t.ok(/let busy = false;/.test(simBody) && /if \(busy\) return;/.test(simBody),
       '★ act() 开头有 busy 闸门 —— 连点直接吞掉，不再发第二条请求');
  const iAct = simBody.indexOf('async function act');
  const iDisable = simBody.indexOf('b.disabled = true', iAct);
  const iReq = simBody.indexOf('API.simProc(body)', iAct);
  const iSet = simBody.indexOf('API.setSimSource(body)', iAct);
  t.ok(iAct >= 0 && iDisable > iAct && iDisable < Math.min(iReq, iSet),
       '★ 先禁用按钮、再发请求（顺序反了等于没防住：请求已经出去了才禁用）');
  /* ★ 判据对准**行为**，不锁代码形状（2026-10-19 改过一次才明白）：
     原来写的是正则 `btns.forEach(b => { b.disabled = !canWrite; })`，
     finally 里一改成多行（顺带把 toggle 文案恢复）它就误报。
     要证的只有一件事：**恢复时 disabled 取决于角色，而不是一律放开**。 */
  const iFin = simBody.indexOf('} finally {', iAct);
  const finBody = iFin >= 0 ? simBody.slice(iFin, iFin + 700) : '';
  t.ok(/b\.disabled = !canWrite;/.test(finBody) && !/b\.disabled = false/.test(finBody),
       '★ 结束后按角色恢复 —— 恢复成"全部可点"等于把 viewer 也放开了');

  /* ⑦ 点击后必须立刻有反馈（2026-10-19 现场反馈："点了没反应"）
     ★ 两个坑各自都有独立判据，少一条就会漏：
        ① 只改按钮文字、没有进度条 —— 面板在页尾，用户视线不在按钮上；
        ② **节点建好了却没插进 DOM** —— 本版第一次实现就踩了这个：
           `.sim-prog` 只 new 出来、没 insertBefore，DOM 里 progCount=0，
           屏幕上什么都没有，而代码看起来"全都写了"。 */
  const iProgCall = simBody.indexOf('progShow(msg);', iAct);   // 带分号：否则会命中 progShow 的定义
  t.ok(iProgCall > iAct && iProgCall < Math.min(iReq, iSet),
       '★ 请求发出**之前**就挂进度条 —— 等到返回再显示等于没有反馈');
  t.ok(/host\.insertBefore\(progEl/.test(simBody),
       '★ 进度条必须真的插进 DOM（只创建不插入 = progCount 0，屏幕上看不见）');
  t.ok(/dataset: \{ role: 'toggle' \}/.test(simBody)
       && /b\.dataset\.role === 'toggle'/.test(simBody),
       '★ 在飞期间按钮文字变「正在…」，且只改 toggle 那一颗（另两颗是名词）');
  t.ok(/class: 'sim-state ' \+ sCls/.test(simBody)
       && /is-bad/.test(simBody) && /is-warn/.test(simBody) && /is-ok/.test(simBody),
       '★ 状态行四态齐全 —— 只有"好"和"坏"会把「链路在跑但采集器没起」说成好');
  t.ok(/采集器未运行|COLL_TXT/.test(simBody) && /coll\.state/.test(simBody),
       '★ 状态行必须报采集器状态 —— 9/9 全绿但采集器没起时，页面是不动的');
  t.ok(/SIM_RETRY_N/.test(simBody) && /setTimeout\(\(\) => \{ SIM_RETRY = null; load\(\); \}/.test(simBody),
       '★ 过渡态补刷：重启刚返回时报 stopped，几秒后要自己再看一眼（有次数上限）');

  // ★ 2026-10-19 实测：模拟源明明启用着、链路 8/9，数据却越等越旧。
  //   后端 error 里写着「OperationalError: attempt to write a readonly database」，
  //   界面上一个字没提 —— 用户只读到「点了没反应」。
  //   「采集器失败」是结论，**原因**才是能动手修的东西。
  t.ok(/coll\.error/.test(simBody) && /cerr/.test(simBody),
       '★ 状态行必须把后端给的失败原因带出来 —— 只写「采集器失败」等于没说');
  const iNotOk = simBody.indexOf('} else if (!cOk) {');
  t.ok(iNotOk > 0 && /coll\.error/.test(simBody.slice(iNotOk, iNotOk + 700)),
       '★ 原因要挂在「采集器没在采」那个分支里（正常态不需要解释）');
  t.ok(/cstate === 'failed'\) \? 'is-bad' : 'is-warn'/.test(simBody),
       '★ failed 与 stale 要分色：写库彻底断是红的，还在重试是黄的');
  t.ok(/stale:\s*'数据已停止更新'/.test(simBody),
       '★ stale 文案写「数据已停止更新」—— 写「采集器在采」会和「写库失败」自相矛盾');

  /* ⑧ §92b：后端改成"收下就算"（HTTP 202）之后，前端必须**跟到 job 说完了**为止。
     ★ 为什么单列：拿第一帧返回就画面板，会出现"进度条刚收、下面写着还在拉起"
       —— 那比"点了没反应"更伤：界面在明明白白地说一件假的事。
       反过来，无限轮询也不行：真卡住时"到底好了没有"就永远悬着。 */
  t.ok(/v\.chain && v\.chain\.job/.test(simBody),
       '★ POST 202 只是"收下了" —— 必须盯着 chain.job，不能直接拿首帧收尾');
  t.ok(/const JOB_WAIT_MS = \d+/.test(simBody) && /Date\.now\(\) < deadline/.test(simBody),
       '★ 轮询要有**上限** —— 到点就如实说"还在办"，不许一直转下去');
  t.ok(/jf && jf\.ok === false/.test(simBody),
       '★ job.ok === false 才算办砸 —— 后台跑失败了，界面不许报「已切换」');
  /* ★ 这条不能靠注释里的字眼（pagesCode 是**剥过注释**的），要对准行为：
       轮询那次 GET 的 catch 里不许出现 throw / break / 报错 ----
       一旦有，就是"一次没回应就把整件事判失败"，而后端此刻多半正在起进程。 */
  const iPollTry = simBody.indexOf('API.simSource()', iAct);
  const iPollCatch = iPollTry > 0 ? simBody.indexOf('catch', iPollTry) : -1;
  const pollCatch = iPollCatch > 0 ? simBody.slice(iPollCatch, iPollCatch + 160) : '';
  t.ok(iPollCatch > iPollTry && /catch/.test(pollCatch)
       && !/throw|break|toast|progHide/.test(pollCatch),
       '★ 轮询中一次 GET 没回应不算失败（后端正在起进程），留着上一次继续等');

  // ① 面板必须在页尾：它管的是**整页的状态**，放在两端各自的块里，
  //    人会以为可以只开一端。
  const dvBody = sliceFn(pagesCode, 'async function devices');
  t.ok(/simPanel,\s*\n\s*\]\)/.test(dvBody),
       '★ 面板挂在设备管理页的**最后一张卡**（上面四块表都在它之前）');
  t.ok(dvBody.indexOf('const simPanel = simSourcePanel') >
       dvBody.indexOf('await redrawAll();'),
       '★ 面板在两端区段渲染完之后才建（它读的是两端重画后的状态）');

  // ② 两端区段里**不再**各自带一个「模拟输入源」块
  const upBody2 = sliceFn(pagesCode, 'function accessUpSection');
  const dnBody2 = sliceFn(pagesCode, 'function accessDownSection');
  const wzBody2 = sliceFn(pagesCode, 'function accessWizard');
  t.ok(!/simBlk/.test(upBody2) && !/simBlk/.test(dnBody2),
       '★ 两端区段里没有各自的「模拟输入源」块（一个面板管两端）');
  t.ok(!/switchSrcBtn|sourceBlock/.test(pagesCode),
       '★ 「真实/模拟输入源」并列卡片连同它们的两个助手函数一起删干净了'
       + '（留着不调用就是两个假入口，下一个人会去找它为什么没显示）');
  // ★ 扫**剥注释后**的 CSS：app.css 里保留了一段说明"这些类已删"的注释，
  //   不剥注释就会把说明文字当成仍然存在规则。
  const cssCode = stripComments(css);
  t.ok(!/\.src-(blk|hd|t|row)\b/.test(cssCode) && !/\.g-2\s*\{/.test(cssCode),
       '★ 对应的 CSS（.src-blk / .src-hd / .src-row / .g-2）也删了');

  // ③ 模拟值填在**原来的表里**，并在原位可改（用户选的是"能改，改的是模拟值"）
  /* 上端这块搬进向导后，局部字段助手叫 fld（原来叫 field）——
     判据跟着改到实现上，而不是把这条删掉。 */
  t.ok(/fld\('模拟主站地址（模拟源时生效）', simHost\)/.test(wzBody2),
       '★ 上端的模拟主站地址并入 ① 接入参数（会自动填好，也能手改）');
  t.ok(/field\('替身地址（模拟源时生效）', simHost\)/.test(dnBody2)
       && /field\('替身端口（模拟源时生效）', simPort\)/.test(dnBody2),
       '★ 下端的替身地址 / 端口并入 ① 接入参数（会自动填好，也能手改）');
  t.ok(/改的是模拟值/.test(wzBody2) || /改的是模拟值/.test(simBody),
       '★ 面板上写明「改的是模拟值」，不是改真实地址');

  // ④ v1.11：模拟态**不收在面板之外** —— 上面的表不再换配色
  //   原来是给 srcHost 贴 .sim-mode，让上端 ①② / 下端 ①② 四张卡整体变紫。
  //   撤它的理由见 app.css 里那段注释：那两张表回答的是"打给谁、问什么"，
  //   不是"数从哪来"；一个页尾开关的状态画上去，人反而分不清
  //   「这一列是模拟值」和「整张表都在模拟」。
  //   这里守的是**别再长回来**：类名、帮手函数、CSS 规则一个都不许留。
  t.ok(!/sim-mode/.test(pagesCode),
       '★ pages.js 不再给上面的表贴 .sim-mode（模拟态只由页尾面板表达）');
  t.ok(!/applySimTint|setSimOn/.test(pagesCode),
       '★ 配色开关与两个帮手函数一起删干净 —— 留着不调用就是假入口，'
       + '下一个人会去找它为什么没生效');
  t.ok(!/\.sim-mode/.test(cssCode),
       '★ CSS 里 .sim-mode 的规则也删了（只删 JS 会留下一段没人用的紫底样式）');

  // ⑤ 「关掉不留痕」这个承诺不能丢，但也不必常驻 —— 它挂在按钮 title 上
  t.ok(/不会被清零/.test(simBody),
       '★ 真实值不会被清零这句话仍在（收进「启用/关闭」按钮的 title，悬停可见）');
  t.ok(/sim_host/.test(simBody) && /sim_port/.test(simBody)
       && /master_host/.test(simBody),
       '★ 仍写明模拟只改哪几列（下端 sim_host / sim_port + 上端 master_host）'
       + ' —— 写成中文"模拟那一列"只有写代码的人看得懂');
  // ⑤b v1.11：正文只留状态，不留常驻解释
  //   ★ 判据要对着 simBody（面板那一段），不是整份 pages.js ——
  //     别的卡片里还有 pm-note，那是别处的说明，不该被这条算进来。
  t.ok(!/class: 'pm-note'/.test(simBody),
       '★ 模拟面板正文没有常驻解释段（原来那四段灰字全收进按钮 title）：'
       + '每天看四遍的解释活不过两天就只剩一片灰，'
       + '真出事时反而找不到那句要紧的红字告警');
  t.ok(/不是现场装置，也不是真机/.test(simBody)
       && /当实测值/.test(simBody),
       '★ 「不是现场装置 / 不是真机」仍在页面上（在「启用模拟源」的 title 里，'
       + '连带"把编的数当实测值"那句风险提示）—— 撤灰字时不能连它一起撤');

  // ⑥ 「已停用」与「未配置」不塌成同一个状态（后端 2026-10-18 修）
  t.ok(/state === 'off'/.test(pagesCode),
       '★ kindBadge 有「已停用」分支 —— 地址齐了但停用 ≠ 没配过，'
       + '塌成「未配置」就会和「目标」列打架');

  // ⑦ 后端那条最容易复发的错：rotate 时 kind 不能落到 real
  t.ok(/kind = None if want is None else \("sim" if want else "real"\)/
       .test(apiPy),
       '★ 后端 rotate 时 kind=None（沿用两端各自形态）—— 写成 '
       + '`"sim" if want else "real"` 会让 want=None 落到 real，'
       + '那一下「换一套」等于把模拟源关了');
  t.ok(/up_kind = kind or _upstream_kind\(conn\)/.test(apiPy)
       && /dn_kind = kind or engine\.get_data_source\(conn\)/.test(apiPy),
       '★ kind=None 时两端各自沿用当前形态（不强行把两端拉齐）');

  // ⑧ v1.10：表与进程分开 —— 「换一个进程」是另一条通道
  //   守的是：**别再把"改配置"当成"让链路跑起来"**。
  //   v1.7~v1.9 的面板只有 setSimSource，用户点完启用，表全填好了、
  //   颜色也变了，可页面上仍然一行数据都没有 —— 因为没有任何进程在应答。
  t.ok(/simProc:\s*\(body\)\s*=>\s*post\('\/api\/sim-source\/proc'/.test(apiJs),
       '★ api.js 有 simProc（POST /api/sim-source/proc）—— 改进程与改表是两条通道');
  t.ok(/act\(\{ action: 'restart' \}, true\)/.test(simBody),
       '★ 「换一个进程」发 {action:restart} 并走**进程那条通道**'
       + '（走 issetSimSource 就又变成改表了）');
  t.ok(/useProc \? await API\.simProc\(body\) : await API\.setSimSource\(body\)/
       .test(simBody),
       '★ act() 按通道分流，两条接口不混用');
  t.ok(/换一个进程/.test(simBody), '面板上有「换一个进程」按钮');
  t.ok(/s\.chain/.test(simBody),
       '★ 面板读 s.chain 拿进程状态 —— 不另抄一份 collector 字段（两个真相源）');

  // 进程表：计划 vs 现实。上面那张是"表上写的端口"，这张是"谁真的在听"。
  t.ok(/r\.alive\s*\n?\s*\?\s*badge\('运行中'/.test(simBody),
       '★ 进程状态以 alive 为准渲染（不是按"曾经起过"）');
  // skip 行（该台停用 / 程序未构建）不能显示成「掉了」：一个是"去查日志"，
  // 一个是"什么都不用做"，混在一起就是让人白忙。
  t.ok(/r\.skip \? badge\('不适用'/.test(simBody)
       && /badge\('掉了', 'b-bad'\)/.test(simBody),
       '★ skip 行显示「不适用」，只有真的该起没起才显示「掉了」');
  t.ok(/cn\.expected/.test(simBody),
       '★ 链路分母用 expected（不含停用/未构建）—— 否则「9/10」看起来像掉了一个');
  t.ok(/链路进程/.test(simBody),
       '★ 面板上把「计划 / 现实」两张表标开 —— 只有计划表时，'
       + '人会以为端口真的有人在听');
  t.ok(/采集器一次只连一台/.test(simBody),
       '★ 写明采集器一次只连一台（架构边界，不是故障）');

  // 表与进程不同套时必须看得见：否则两套并行、都自称正常
  t.ok(/preset_matches/.test(simBody) && /进程还在/.test(simBody),
       '★ 「换一套」改表后，若进程没跟，面板明说"进程还在上一套"');
  t.ok(/换一个进程/.test(simBody) && /它改的是\*\*表\*\*/.test(simBody),
       '★ 面板上写清「换一套」改的是表、「换一个进程」才让进程搬过去'
       + '（位置在「换一套」的 title 上：这句是解释，不是状态）');

  // SVG 走 createElementNS（h() 用 createElement，进不了 SVG 命名空间）
  t.ok(/function iconSwap\(\)/.test(pagesCode)
       && /createElementNS\(NS, 'svg'\)/.test(sliceFn(pagesCode, 'function iconSwap')),
       '★ 「换一个进程」的双向箭头是手写 SVG，且走 createElementNS');

  // 后端两处硬判据（写错会静默"看起来在跑"）
  t.ok(/re\.compile\(r"\/api\/sim-source\/proc"\)/.test(apiPy),
       '★ 后端注册了 POST /api/sim-source/proc');
  t.ok(simprocPy.length > 0, '进程编排模块 14/src/simproc.py 存在');
  t.ok(/spawn_peer=False/.test(simprocPy),
       '★ 采集器经 engine 起且要求不要重复拉替身'
       + '（同一端口两个替身会互踢，后者启动即退出 —— 看起来像"替身坏了"）');
  t.ok(/collector_alive/.test(simprocPy),
       '★ 采集器存活以 engine 状态机为准（服务器重启后子进程对象是 None，'
       + '但采集器可能还在跑）');
  // ★ 主站 `--seconds 0` 是个真踩过的坑：网关 0=一直跑，主站 0=一拍都不跑。
  t.ok(/MASTER_RUN_S/.test(simprocPy) && !/MASTER_EXE, "--seconds", "0"/.test(simprocPy),
       '★ 模拟主站的 --seconds 不能写 0（它的循环条件是 now-t0 < seconds*1000，'
       + '0 会让它立刻退出，而日志里其实已经收到 ASDU —— 排障方向全跑偏）');

  // ★ 平台重启 ≠ 现场进程退出。启动路径里必须主动认领一次，
  //   靠「打开面板时懒触发」不够 —— 启动那一下面板还没人打开。
  t.ok(/simproc\.CHAIN\.attach\(conn\)/.test(serverPy),
       '★ 平台启动时按 PID 认领遗留链路进程（否则面板说「未启动」而数据在涨，'
       + '再点「换一个进程」会拉起第二套抢同一个端口）');
  t.ok(/sim_chain_procs/.test(simprocPy) && /tasklist/.test(simprocPy),
       '★ 认领靠 meta 里的 pid 表 + tasklist 判活，不是凭"上次起过"');

  // =================================================================
  t.head('[V] 时刻标签：末一拍不许折回 00:00 · 页面要说清"在看哪份数据"');
  // =================================================================
  /* ★ 起因是用户原话「实时检控永远是 0:00」（2026-10-19）：
       ① 默认数据集跟引擎模式走，引擎在仿真模式 ⇒ 看的是离线日仿真 sim-d002；
       ② 那份数据的末一拍 t_s = 86400，被 %24 折成 "00:00"，与当天第一拍同标签
          ⇒ 读数不动、轴最右端也是 0:00，第一反应是"系统卡了"；
       ③ 页面上从头到尾没写"你在看哪份数据"，于是无处消解这个误会。

     ★ 判据因此必须是"真跑一遍算出什么"，不是"函数里有没有某个字"：
       源码注释里为了说明这个问题，本身就得写出那个旧式子。 */
  {
    const m = /hhmm\(t_s\) \{[\s\S]*?\n  \},/.exec(uiJs);
    let hhmm = null;
    if (m) {
      try {
        // _bad 是 ui.js 模块内的私有函数，这里按它的语义塞一份同义实现。
        const fake = (v) => v === null || v === undefined
          || (typeof v === 'number' && !Number.isFinite(v));
        hhmm = new Function('_bad', `return {${m[0].replace(/,\s*$/, '')}}`)(fake).hhmm;
      } catch (e) { hhmm = null; }
    }
    t.ok(typeof hhmm === 'function', 'ui.js 的 fmt.hhmm 能被独立取出来跑');
    if (typeof hhmm === 'function') {
      t.eq(hhmm(86400), '24:00',
        '★ 24 h 仿真末一拍 t_s=86400 → 24:00（老写法折回 00:00，与首拍一样）');
      t.eq(hhmm(0), '00:00', '第 0 拍仍是 00:00（起点没被改坏）');
      t.eq(hhmm(43200), '12:00', '12:00 不受影响');
      t.eq(hhmm(86401), 'D2 00:00',
        '★ 跨过午夜带 D 前缀（周 / 月仿真不能被压进同一天）');
      t.eq(hhmm(604800), 'D7 24:00', '★ 周仿真末拍 = 第 7 天 24:00');
      t.eq(hhmm(null), '—', '空值走 —（没有 NaN / undefined 漏到界面上）');
    }
  }

  const rtBody = sliceFn(pagesCode, 'function realtime');
  t.ok(/这一页在看哪份数据/.test(rtBody),
       '★ 实时监控页有一张「这一页在看哪份数据」说明卡'
       + '（不写就默认用户在看实时，0:00 不动会被读成"系统卡了"）');
  t.ok(/othEng\.dataset/.test(rtBody) && /ei\.live/.test(rtBody),
       '★ 说明卡写了"另一边实时在写多少行"—— 数据来自同一个 /api/engine，'
       + '不与后端 default_scenario 各推一套');
  t.ok(!/'仿真模式'/.test(rtBody) && !/'运行模式'/.test(rtBody),
       '★ 说明卡不写「运行模式 / 仿真模式」：那是研发语境，'
       + '且「运行模式」已被业务侧（峰谷套利…）占用');
  t.ok(/curEng\.dataset \|\| ctx\.scenario/.test(rtBody),
       '数据集 id 直接取自 /api/engine 的 engines[]，页面上不重算');

  /* ------------------------------------------------------------------
     [W] 「接入调度主站」九步向导（v1.14）

     这一段守的不是"有没有向导"，是那几件**做错了会骗人**的事：

       · 九步的标题与必填清单**只在后端一份**（accessconf.STEPS）。
         前端写死一份，加一格往后就只改一边，导出件与界面对不上。
       · 「下一步」不校验必填 —— 现场资料常有没拿到的，逼人填假日期更坏。
         校验只发生在「完成填报」那一下。
       · 按钮**不写「已完成」**：九步里通道、证书、联调、传动都不在软件里，
         写「已完成」等于软件替人宣布接入完成。
       · 「完成填报」要求必填全齐（后端判），缺项要指出**第几步缺什么**，
         并且弹层**不关** —— 只说"还有没填的"，人要自己一页页找。
       · 只有第 6 步标「会改配置」：它是九步里唯一写文件的那一步。
         不多不少 —— 八步都挂一句，读到第三页就没人看了。
       · 重置只清草稿，**不动**链路参数与转发表，且必须先确认。
       ------------------------------------------------------------------ */
  t.head('[W] 接入调度主站九步向导');

  const wzW = sliceFn(pagesCode, 'function accessWizard');
  t.ok(wzW.length > 2000, 'accessWizard 是一段完整实现（判据打在实现上）');

  // ① 字段清单只在后端
  t.ok(!/管辖权与接入方案/.test(wzW) && !/报送资料/.test(wzW),
       '★ 九步的标题不写在前端（字段清单只有后端 accessconf 一份）');
  t.ok(/API\.accessDraft\(\)/.test(wzW),
       '向导真的从 /api/access-draft 取 schema + 草稿 + 缺项');
  t.ok(/r\.steps \|\| \[\]/.test(wzW) && /r\.missing/.test(wzW),
       '★ 步骤与缺项都来自后端，前端不自己算放行');

  // ② 填了就存；「下一步」不拦
  t.ok(/setTimeout\(\(\) => \{ flush\(\); \}, 500\)/.test(wzW)
       || /setTimeout\([^)]*flush/.test(wzW),
       '输入即存（防抖），不靠「保存草稿」按钮');
  t.ok(!/'保存草稿'/.test(wzW),
       '★ 没有「保存草稿」按钮 —— 多一个按钮就多一个"忘了点"的理由');
  t.ok(/lastStep === cur\) return true/.test(wzW),
       '★ 翻页也算改动（current_step 要跟着存），否则断点续填永远回到保存内容那一页');

  // ③ 完成才校验，且必须指出缺在哪
  t.eq((wzW.match(/completed: true/g) || []).length, 1,
       '★ completed=true 只在「完成填报」那一处发出');
  t.ok(/list\[0\]\.step/.test(wzW) && /已跳到那一页/.test(wzW),
       '★ 缺项时跳到第一个缺的那一步，并说清缺什么（不是只说"还有没填的"）');
  t.ok(/还有 \$\{lst\.length\}|\$\{list\.length\} 项必填没填/.test(wzW)
       || /项必填没填/.test(wzW),
       '错误提示带缺项条数');
  t.ok(!/'已完成'/.test(wzW),
       '★ 按钮与状态都不写「已完成」：九步里大半不在软件里，写它就是替人宣布接入完成');
  t.ok(/'完成填报'/.test(wzW) && /last \? '完成填报' : '下一步'/.test(wzW),
       '最后一页的按钮是「完成填报」，别的页是「下一步」');

  // ④ 只有第 6 步亮明"会改配置"；步骤头的灰底解释段不再占版面
  t.ok(/'会改配置'/.test(wzW) && !/'只记台账'/.test(wzW),
       '★ 只有第 6 步标「会改配置」—— 八步不再逐页写声明（每页都挂一句，读到第三页就没人看）');
  t.ok(!/wz-why/.test(wzW) && /title: s\.why/.test(wzW),
       '★ 步骤头的灰底解释段（s.why）不再渲染；文案没丢，挂在 h3 的 title 上');
  t.ok((wzW.match(/refreshStars\(\);/g) || []).length >= 2,
       '★ 星号首屏就要收敛一次（只在 touch() 里调 = 没敲过键的那屏挂着满屏假必填）');
  t.ok(/if \(s\.config\) configPanel\(\)\.forEach/.test(wzW),
       '配置区只在标了 config 的那一步渲染（第 6 步），不是每步都有');

  // ⑤ 断点续填：不静默跳
  t.ok(/resumeNote/.test(wzW) && /上次填到第 \$\{cur\} 步/.test(wzW),
       '★ 续填时明说「上次填到第几步、什么时候、谁填的」（静默跳会被读成"上次填的丢了"）');
  t.ok(/resumeNote = '';/.test(wzW),
       '那句说明只在刚打开时显示一次，翻页不重复');

  // ⑥ 关窗路径都要落地
  t.ok(/e\.key === 'Escape'\) flush\(\)/.test(wzW),
       '★ Esc 关弹层前先 flush（走的是 modal 自己的监听，不经过「退出」按钮）');
  t.ok(/await flush\(\);\n        downloadFile\(API\.accessDraftCsvUrl\(\)/.test(wzW)
       || /flush\(\)[\s\S]{0,80}accessDraftCsvUrl/.test(wzW),
       '★ 导出前先 flush：不先存就会导出上一版（"导出件少了两格"最容易被当成丢数据）');
  t.ok(/await flush\(\);\n        if \(o\.close\) o\.close\(\)/.test(wzW)
       || /flush\(\)[\s\S]{0,60}o\.close\(\)/.test(wzW),
       '「退出」先把当前输入存下来再关');
  t.ok(/wizard\.flush\(\); wzModal\.close\(\)/.test(pagesCode),
       '★ 切页时也 flush（弹层挂在 body 上，不 flush 就会丢掉刚敲的那一下）');

  // ⑦ 不预填：替用户断言比留空更坏
  t.ok(!/default: 'pass'|default: "pass"/.test(pagesCode),
       '★ 不预填「通过」这类结论（预填就是系统替人签字）');

  // ⑧ 重置的边界
  t.ok(/window\.confirm/.test(upBody) && /链路参数/.test(upBody) && /转发表/.test(upBody),
       '★ 「重置」先确认，并写明不动链路参数与转发表（混着清会把线上配置一起抹掉）');
  t.ok(/API\.resetAccessDraft\(\)/.test(upBody), '「重置」真的调 DELETE，不只是弹个框');

  // ⑩ 每步的材料清单 / 建议值 / 本步导出（v1.15：八步不再只是空表单）
  t.ok(/function matsBlock/.test(wzW) && /s\.doc\.mats/.test(wzW),
       '★ 材料清单从后端 doc.mats 渲染 —— 前端不写死「报送 10 项」这类内容');
  /* ★ v1.16：向导里不许再出现自造的表格/框体类。
     整站所有表格都是 card() + table()（table.tbl：表头无底色、11px 大写小字距），
     向导原来另起了一套 .wz-tb（表头灰底、12.5px）与 .wz-mats（外面再套圆角框），
     两套并排放在同一屏就是"风格不匹配"的全部来源。 */
  t.ok(/card\(\s*\n?\s*m\.title/.test(wzW) && /table\(tcols/.test(wzW),
       '★ 材料清单用整站的 card() + table() 画，不另起一套 wz-tb');
  t.ok(!/wz-mats|wz-tb|wz-cfg/.test(wzW),
       '★ 向导里已无 wz-mats / wz-tb / wz-cfg —— 同一件东西只画一遍');
  t.ok(!/\.wz-mats|\.wz-tb|\.wz-cfg/.test(cssCode),
       '★ app.css 里对应的老样式也删干净了（留着就是下次又被人捡起来用）');
  t.ok(!/DL\/T 516-2017 7\.2/.test(wzW),
       '★ 规程口径不写死在前端：文案归 accessconf.DOCS，前端只负责画');
  t.ok(/wz-assist/.test(wzW) && /按建议填好/.test(wzW),
       '★ 建议值是一键填进字段（不是弹一句提示让人自己抄）');
  t.ok(/s\.assist \|\| \[\]/.test(wzW), '建议值也来自后端（assist），前端不自己推');
  t.ok(/API\.accessDraftCsvUrl\(st\.num\)/.test(wzW),
       '★ 导出按步走：URL 带 step，列顺序与文件名由后端 export_step 决定');
  t.ok(/st\.export \|\| \{\}/.test(wzW), '按钮文案取后端 export.label');
  t.ok(/\?step=' \+ step/.test(apiJs), 'api.js 的导出 URL 支持 ?step=N（本步那份）');

  // ⑨ 接口齐不齐
  for (const m of ['accessDraft', 'saveAccessDraft', 'resetAccessDraft', 'accessDraftCsvUrl']) {
    t.ok(new RegExp(m + '\\s*:').test(apiJs), 'api.js 有 ' + m);
  }

  const okAll = t.report();
  process.exit(okAll ? 0 : 1);
})();
