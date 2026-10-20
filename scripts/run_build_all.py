# -*- coding: utf-8 -*-
"""跑全量构建：以 cmd 调起 scripts\\build_all.bat，日志落 build_all.log。

只设 EMS_NODE（钉死 Node 运行时）；**刻意不设 EMS_PYTHON** ——
设了会顶掉 13/ 对自己 .venv 的探测，跨语言层静默跳过、基线少 78。
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'build_all.log')
BAT = os.path.join(ROOT, 'scripts', 'build_all.bat')


def main():
    env = dict(os.environ)
    env.pop('EMS_PYTHON', None)
    env['EMS_NODE'] = r'C:/Users/31857/.workbuddy/binaries/node/versions/22.22.2-6/node.exe'
    assert os.path.exists(env['EMS_NODE']), env['EMS_NODE']
    assert os.path.exists(BAT), BAT

    t0 = time.time()
    # ★ 用**相对路径**调起：根目录含括号 `(cn)`，cmd 会把绝对路径里的括号当分组符号，
    #   表现是 `'D:\wb' 不是内部或外部命令`。cwd 已经钉在 ROOT，相对路径一样准。
    with open(LOG, 'wb') as f:
        p = subprocess.run(['cmd', '/c', r'scripts\build_all.bat'], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
    dt = time.time() - t0
    print('BUILD_RC=%d  elapsed=%.1fs  log=%s' % (p.returncode, dt, LOG))
    return 0


sys.exit(main())
