# -*- coding: utf-8 -*-
"""
生成 15/assets/assembly.js —— 柜体装配视图数据（爆炸图 / 一次接线图的共用地基）。

为什么要有这个脚本
------------------
assembly.js 是**派生产物**，源是 `docs/规划/部件测点映射表.csv`（78 行）。
没有生成器的时候，改了 CSV 只能手改 JS —— 而手改必然与 CSV 脱钩，
且这种脱钩不会让任何业务断言失败，只会让界面与现场对不上：
「现场有这个测点、界面上没有」，不报错，看不出来。

所以这里把职责切开：
  结构 + 测点列表  → 全部由 CSV 生成（本脚本）
  显示名 / 实时绑定 → 人工维护下面两张小表（这两项 CSV 里没有，本就该人工定）

生成的每个部位 / 部件都带 `src` 字段 = CSV 里的原值。
`15/tests/selftest.js` 的 J 段用它做**精确** join（不做字符串猜测），
逐点比对测点名 / 设备点名 / 单位三列。

用法
----
    python 15/scripts/gen_assembly.py            生成并覆盖 15/assets/assembly.js
    python 15/scripts/gen_assembly.py --check    只比对、不写盘（改动不符时退出码 1）
"""

import csv
import io
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CSV_MAP = os.path.join(ROOT, 'docs', '规划', '部件测点映射表.csv')
OUT_JS = os.path.join(ROOT, '15', 'assets', 'assembly.js')

# ---------------------------------------------------------------------
# 人工维护的两张表（CSV 里没有这些信息，只能人定）
# ---------------------------------------------------------------------

# 部位：CSV「所属部位」原值 → (部位 id, 界面显示名)
# ★ 显示名与 CSV 原值故意不同（页面要带序号、要写"站级"），
#   所以两边都留着：src 用于与 CSV 对齐，name 用于给人看。
AREAS = [
    ('柜内·电池舱', 'bat', '① 电池舱'),
    ('柜内·PCS舱',  'pcs', '② PCS 舱'),
    ('柜内·辅助',   'aux', '③ 辅助舱'),
    ('站内·一次',   'sta', '④ 站级·站内一次'),
]

# 部件：CSV「部件」原值 → (部件 id, 实时设备, 设备字段, 单位, 倍率)
# ★ dev = None 表示**设计上就没有实时数据源**，不是"暂时没数据"。
#   界面对这类部件必须显式显示"无实时源" —— 涂成"正常"等于谎报现场在传数据。
#   能绑的只有 5 个，因为 /api/realtime 只返回 5 台设备：
#   DEV-METER-01 / DEV-PV-01 / DEV-LOAD-01 / DEV-PCS-01 / DEV-BMS-01。
BIND = {
    '电池簇':           ('cluster',   'DEV-BMS-01',   'soc',           '%',   100),
    '电芯（模组内单体）': ('cell',      'DEV-BMS-01',   'temperature_c', '°C',  1),
    '绝缘监测':         ('insul',     None,           None,            '',    1),
    '直流接触器':       ('contactor', None,           None,            '',    1),
    'BMS 主控':         ('bms',       None,           None,            '',    1),
    'PCS 变流器':       ('conv',      'DEV-PCS-01',   'power_kw',      'kW',  1),
    'PCS 指令落点':     ('cmdp',      None,           None,            '',    1),
    '消防主机':         ('fire',      None,           None,            '',    1),
    '空调 / 液冷机组':   ('hvac',      None,           None,            '',    1),
    '门禁 / 安防':      ('sec',       None,           None,            '',    1),
    '配电 / 开关':      ('sw',        None,           None,            '',    1),
    '关口电表':         ('meter',     'DEV-METER-01', 'power_kw',      'kW',  1),
    '变压器':           ('tr',        None,           None,            '',    1),
    '光伏逆变器':       ('pv',        'DEV-PV-01',    'power_kw',      'kW',  1),
}

HEADER = """/* =====================================================================
   15/ — 柜体装配视图数据（爆炸图 / 一次接线图的共用地基）
   ---------------------------------------------------------------------
   ★★ 本文件是**派生产物**，由 `15/scripts/gen_assembly.py` 生成，请勿手改。
      源 = `docs/规划/部件测点映射表.csv`（78 行 × 9 列）。
      改动请走生成器：  python 15/scripts/gen_assembly.py
      （加 --check 可只比对不写盘，用于 CI / 自检）

   字段语义
     src              该部位 / 部件在 CSV 里的**原值**。
                      15/tests/selftest.js 的 J 段用它做精确 join，
                      逐点比对「测点（现场口径）/ 设备点名 / 单位」三列 ——
                      这是"界面结构没与现场点表脱钩"唯一的自动拦截点。
     id               界面内部标识（点部件、跨渲染记忆选中态都用它）
     name             显示名（带序号、带"站级"等页面口径，与 src 故意不同）
     dev/key/unit/mul 运行时绑定。dev=null 表示**设计上就没有实时数据源**
                      （不是"暂时没数据"）—— 界面必须显式显示"无实时源"，
                      把它涂成"正常"等于谎报现场这个部件在传数据。
     points[]         该部件的全部测点定义（点名 / 设备侧点名 / 单位）

   当前 14 个部件里只有 5 个能绑定实时值（cluster/cell/conv/meter/pv），
   因为 `/api/realtime` 只有 DEV-METER-01 / DEV-PV-01 / DEV-LOAD-01 /
   DEV-PCS-01 / DEV-BMS-01 五台设备。其余 9 个部件的点要走 `13/` 的
   Modbus 接入才有实时值 —— 这是事实，不是待办，界面按事实呈现。
   ===================================================================== */

const ASSEMBLY = [
"""

FOOTER = """  ];
"""


def js_str(s):
    """JS 单引号字符串字面量（本仓库数据里没有单引号与反斜杠，转义留个兜底）"""
    return "'" + str(s).replace('\\', '\\\\').replace("'", "\\'") + "'"


def js_val(v):
    return 'null' if v is None else js_str(v)


def load_rows():
    """读 CSV，返回 (部位原值, 部件原值, 测点名, 设备点名, 单位) 的列表，保持行序。"""
    with io.open(CSV_MAP, 'r', encoding='utf-8-sig', newline='') as f:
        rows = list(csv.reader(f))
    if not rows:
        raise SystemExit('CSV 为空：' + CSV_MAP)
    head = [h.strip() for h in rows[0]]

    def col(needle):
        for i, h in enumerate(head):
            if needle in h.replace(' ', ''):
                return i
        raise SystemExit('CSV 缺少列：' + needle + '（现有表头 ' + repr(head) + '）')

    i_area, i_part = col('所属部位'), col('部件')
    i_pt, i_dev, i_unit = col('测点'), col('设备点名'), col('单位')

    out = []
    for r in rows[1:]:
        if not r or not any(x.strip() for x in r):
            continue
        out.append((r[i_area].strip(), r[i_part].strip(),
                    r[i_pt].strip(), r[i_dev].strip(), r[i_unit].strip()))
    return out


def build(rows):
    """按（部位 + 部件）分组，保持 CSV 行序；未知的部位 / 部件直接报错退出。"""
    known_areas = {a[0] for a in AREAS}
    seen_areas = [a for a, _, _, _, _ in rows]
    for a in sorted(set(seen_areas)):
        if a not in known_areas:
            raise SystemExit(
                'CSV 出现了生成器未登记的部位 %r。\n'
                '  请在 gen_assembly.py 的 AREAS 里补一行（CSV 原值 → id → 显示名），\n'
                '  不要去改 CSV —— CSV 是真相源。' % a)

    out = [HEADER]
    for csv_area, area_id, area_name in AREAS:
        parts = []
        for p_name in _part_order(rows, csv_area):
            rows_of = [r for r in rows if r[0] == csv_area and r[1] == p_name]
            if p_name not in BIND:
                raise SystemExit(
                    'CSV 出现了生成器未登记的部件 %r（部位 %r）。\n'
                    '  请在 gen_assembly.py 的 BIND 里补一行。' % (p_name, csv_area))
            pid, dev, key, unit, mul = BIND[p_name]
            pts = ',\n'.join(
                '              { n: %s, d: %s, u: %s }'
                % (js_str(r[2]), js_str(r[3]), js_str(r[4]))
                for r in rows_of)
            parts.append(
                '        {\n'
                '          id: %s,\n'
                '          name: %s,\n'
                '          src: %s,\n'
                '          n: %d,\n'
                '          dev: %s,\n'
                '          key: %s,\n'
                '          unit: %s,\n'
                '          mul: %s,\n'
                '          points: [\n%s\n'
                '          ]\n'
                '        }' % (js_str(pid), js_str(p_name), js_str(p_name),
                               len(rows_of), js_val(dev), js_val(key),
                               js_str(unit), mul, pts))
        out.append(
            '    {\n'
            '      id: %s,\n'
            '      name: %s,\n'
            '      src: %s,\n'
            '      parts: [\n%s\n'
            '      ]\n'
            '    },\n' % (js_str(area_id), js_str(area_name), js_str(csv_area),
                          ',\n'.join(parts)))

    text = ''.join(out)
    text = text.rstrip(',\n') + '\n' + FOOTER
    return text


def _part_order(rows, csv_area):
    """部位内部件的顺序 = CSV 首次出现的顺序（不排序，保持现场清单的行序）"""
    order, seen = [], set()
    for a, p, _, _, _ in rows:
        if a == csv_area and p not in seen:
            seen.add(p)
            order.append(p)
    return order


def main():
    check = '--check' in sys.argv
    rows = load_rows()
    text = build(rows)

    if check:
        if not os.path.isfile(OUT_JS):
            print('[FAIL] 目标文件不存在：' + OUT_JS)
            return 1
        with io.open(OUT_JS, 'r', encoding='utf-8') as f:
            old = f.read()
        if old == text:
            print('[ok] 15/assets/assembly.js 与 CSV 一致（%d 行测点）' % len(rows))
            return 0
        print('[FAIL] 15/assets/assembly.js 与 CSV 不一致，请重跑：')
        print('       python 15/scripts/gen_assembly.py')
        return 1

    with io.open(OUT_JS, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    n_parts = len({(a, p) for a, p, _, _, _ in rows})
    print('[ok] 已生成 15/assets/assembly.js：%d 部位 / %d 部件 / %d 测点'
          % (len({a for a, _, _, _, _ in rows}), n_parts, len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
