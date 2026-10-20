# -*- coding: utf-8 -*-
"""把平台库恢复到「开箱默认」：数据源 = 模拟、清掉上一轮采集留下的 failed 状态。

为什么要这一步：全量构建的 14/ 自检会真的把数据源切到 `real`（[G7] 段），
而本机 `upstream_link` 原本是空表；我之前冒烟跑出来的 `live_state=failed` 也会留着。
用户重启服务后打开「设备管理」，会看到一个自己没设过的现场数据源 + 一条失败横幅。

★ 走的是**真实接口路径**（`api.dispatch`），不是手写 SQL —— 免得"修数据"和
  "业务逻辑"两套写法以后对不上。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, '14', 'src'))

import api          # noqa: E402
import auth         # noqa: E402
import emsdb        # noqa: E402


def show(conn, tag):
    kinds = [r["source_kind"] for r in emsdb.query(
        conn, "SELECT DISTINCT source_kind FROM device_conn")]
    up = emsdb.query(conn, "SELECT link_id, kind FROM upstream_link")
    meta = {r["key"]: r["value"] for r in emsdb.query(conn, "SELECT * FROM meta")}
    print('%-14s device_conn=%s  upstream=%s  meta.data_source_kind=%s  live_state=%s'
          % (tag, kinds, up, meta.get('data_source_kind'), meta.get('live_state')))


def main():
    conn = emsdb.connect()
    show(conn, '[当前]')

    u = auth.authenticate(conn, 'operator', 'operator123')
    assert u, 'operator 账号不存在'

    # 两端各切一次（v1.7 起是两个独立开关：一次只切一端）。
    # 开箱默认两端都是模拟 —— 这样界面不会一进去就挂着「下端模拟 → 上端真实」
    # 的红字（那条提示本身没错，但它不该是别人没设过就出现的）。
    for end, kind in (('downstream', 'sim'), ('upstream', 'sim')):
        st, out = api.dispatch(conn, 'POST', '/api/data-source', {},
                               {'end': end, 'kind': kind}, u)
        got = ''
        for e in (out.get('ends') or []):
            if e['id'] == end:
                got = '%s(%s)' % (e['kind'], e['peer'])
        print('[data-source] %-10s -> %s status=%d' % (end, got, st))
        if st != 200:
            print('!! 切换失败：', out)
            return 1

    st, out = api.dispatch(conn, 'POST', '/api/engine/live/reset', {}, {}, u)
    print('[live/reset] status=%d %s' % (st, str(out)[:120]))

    # 清掉上一轮冒烟（04:03）留在 meta 里的伪失败。
    # 判据：那条 `attempt to write a readonly database` 是**过期事实** ——
    # 上面这次切换刚刚成功写库，库现在是可写的；而 live_state 会一直挂在界面上
    # 当成"当前错误"，用户会去查一个不存在的只读问题。
    for k, v in (('live_state', 'stopped'), ('live_error', ''),
                 ('live_rows', '0'), ('live_started_at', ''), ('live_csv', ''),
                 ('live_source', ''), ('live_peer_pid', '0')):
        emsdb.set_meta(conn, k, v)
    conn.commit()
    print('[meta] live_state -> stopped，live_error 清空（原值是过期的冒烟残留）')

    show(conn, '[复位后]')
    st, v = api.dispatch(conn, 'GET', '/api/data-source', {}, {}, u)
    col = v['collector']
    print('[采集器] state=%s running=%s rows=%s error=%r'
          % (col['state'], col['running'], col['rows'], col['error']))
    print('[下端] %s' % (v['downstream']['summary'],))
    print('[上端] %s' % (v['upstream']['summary'],))
    conn.close()
    return 0


sys.exit(main())
