import subprocess, sys
ROOT = 'D:/wb(cn)/BMS'
SCRIPTS = ROOT + '/scripts'
LOG = ROOT + '/_build_all.log'
with open(LOG, 'w', encoding='utf-8', errors='replace') as f:
    p = subprocess.Popen(['cmd', '/c', 'build_all.bat'], cwd=SCRIPTS,
                         stdout=f, stderr=subprocess.STDOUT)
    rc = p.wait()
print('RC =', rc)
