/* =====================================================================
   15/ 平台界面自检（Node 静态契约校验，零依赖）

   为什么要静态自检：15/ 是纯前端，没有编译期检查。它唯一的「编译」
   是浏览器加载。所以这里把三类最容易静默出错的东西做成断言：

     A 文件齐备    —— 引用的文件在不在、顺序对不对、能不能离线打开
     B 接口契约    —— 前端调的每个 /api/... 后端必须真有（14/src/api.py）
     C 页面与权限  —— 菜单 / 页面函数 / 角色门槛 三者必须一致

   另加 D 渲染纪律（不做危险的 DOM 注入）与 E 图表函数齐备。

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
  'assets/pages.js',
  'assets/app.js',
];
FILES.forEach(f => t.ok(fs.existsSync(path.join(ROOT15, f)), `存在 ${f}`));

const indexHtml = read(path.join(ROOT15, 'index.html'));
const css = read(path.join(ROOT15, 'assets/app.css'));
const apiJs = read(path.join(ROOT15, 'assets/api.js'));
const uiJs = read(path.join(ROOT15, 'assets/ui.js'));
const pagesJs = read(path.join(ROOT15, 'assets/pages.js'));
const appJs = read(path.join(ROOT15, 'assets/app.js'));

// 引用顺序：api.js 必须先于 app.js（后者启动时要调 API），ui.js 先于 pages.js（后者用 h/fmt）
const refs = [...indexHtml.matchAll(/<script\s+src="([^"]+)"><\/script>/g)].map(m => m[1]);
t.eq(refs, ['assets/api.js', 'assets/ui.js', 'assets/pages.js', 'assets/app.js'],
  '脚本引用顺序正确');

const cssRefs = [...indexHtml.matchAll(/<link\s+[^>]*href="([^"]+)"/g)].map(m => m[1]);
t.eq(cssRefs, ['assets/app.css'], '样式表引用正确');

// 离线可用：不允许任何外部主机
const externals = [...(indexHtml + css + apiJs + uiJs + pagesJs + appJs)
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
t.eq(menuIds.length, 10, '菜单项数 = 10（v1.4 新增「仿真」页）');
t.ok(new Set(menuIds).size === menuIds.length, '菜单 id 不重复');
t.ok(menuIds.includes('sim'), '菜单含「仿真」页');

// Pages 导出键：取 IIFE 收尾那句 return { a, b, ... };})();
const pagesTail = pagesJs.match(/return \{ ([a-z0-9_,\s]+) \};\s*\}\)\(\);\s*$/m);
const pageKeys = pagesTail
  ? pagesTail[1].split(',').map(s => s.trim()).filter(Boolean)
  : [];
t.ge(pageKeys.length, 10, 'Pages 导出页面数');
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

t.ok(/td\.textContent = /.test(uiJs), 'table() 单元格走 textContent');
t.ok(/else if \(k === 'text'\) el\.textContent = v;/.test(uiJs), "h(text:) 走 textContent");
t.ok(/document\.createTextNode/.test(uiJs), '子节点走 createTextNode');

// innerHTML 只允许出现在明确白名单的 5 处：h() 的 html 分支 + pages.js 四个图表容器
const innerHtmlPages = (pagesJs.match(/innerHTML/g) || []).length;
const innerHtmlUi = (uiJs.match(/innerHTML/g) || []).length;
t.eq(innerHtmlUi, 1, 'ui.js 仅 1 处 innerHTML（h() 的 html 分支）');
t.eq(innerHtmlPages, 4, 'pages.js 仅 4 处 innerHTML（图表容器）');

// 这四处必须是图表函数或静态文案，不能直接塞接口返回的字符串
const innerTargets = [...pagesJs.matchAll(/innerHTML\s*=\s*(.{0,40})/g)].map(m => m[1]);
t.ok(innerTargets.every(s => /^(lineChart\(|energyFlow\(|'<div class="empty">)/.test(s)),
  'innerHTML 目标均为自绘 SVG 或静态文案');

// html: 的实参必须是字符串字面量（不含来自响应的变量插值）
const htmlArgs = [
  ...pagesJs.matchAll(/html:\s*\n?\s*(.{0,60})/g),
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
  'energyFlow', 'barChart', 'ratioRow'];
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
t.ok(!/<marker/.test(uiJs), 'ui.js 不再用 <marker> 画箭头（改实心多边形）');
t.ok(/const AT = 16, AH = 14, AHH = 12;/.test(uiJs),
     '粗箭头体量为 16/14/12（够粗能读，又不压储能柜与铁塔导线）');
t.ok(/const arrow = \(x1, x2, y, color, active, label, key\)/.test(uiJs),
     'arrow() 签名含方向字 label 与 patch 钩子 key');
t.ok(/class="flow-arrow"/.test(uiJs), '箭头有 .flow-arrow 钩子');
t.ok(!/badgeText/.test(uiJs), 'node() 不再画侧边徽章（方向字已挪进箭头杆）');

// 箭头两端留白：尾端退到 Lx±gap，母线端退到 cx±(busHalf+busGap)
t.ok(/const gap = 58, busGap = 10;/.test(uiJs), 'ui.js 定义箭头两端留白 gap = 58 / busGap = 10');
t.ok(/nearL = Lx \+ gap, nearR = Rx - gap/.test(uiJs),
     '箭头尾端用 Lx±gap（不是地块顶点 Lx±52），端面不压设备插画');
t.ok(/busL = cx - busHalf - busGap, busR = cx \+ busHalf \+ busGap/.test(uiJs),
     '箭头母线端退到 cx±(busHalf+busGap)，端面不贴死柱身轮廓（顶盖/底台比柱身宽）');

// 等轴测 3D 母线 + 竖排标签
t.ok(/const sceneBus = \(c\) =>/.test(uiJs), 'ui.js 定义等轴测母线场景 sceneBus()');
t.ok(/const busLabel = \['交', '流', '母', '线'\];/.test(uiJs), '母线标签为四字数组（每行一字）');
t.ok(!/rotate\(-90/.test(uiJs), '母线标签不再旋转（改竖排、字正）');

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
t.ok(!/flow-sweep-l/.test(uiJs) && !/flow-sweep-l/.test(css),
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
t.ok(!/clear\(flowHost\)/.test(pagesJs), 'pages.js 不再每拍清空重建能量流卡（否则光带动画重启）');
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
t.ok(/state\.scenario = '';\s*\n\s*state\.t = null;\s*\n\s*await loadEngine\(\)/.test(appJs),
  '切引擎模式后清空数据集与时刻并重新解析（不沿用旧数据集）');
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
t.ok(/if \(state\.engine !== 'sim'\) \{\s*\n\s*await API\.setEngine\('sim'\)/.test(appJs),
  'openDataset 在看仿真结果前先确保引擎在仿真模式');

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

const okAll = t.report();
process.exit(okAll ? 0 : 1);
