import subprocess

ROOT = 'D:/wb(cn)/BMS'
SCRIPTS = ROOT + '/P3/scripts'
LOG = SCRIPTS + '/_tcp_run.log'
LIMIT = 240

with open(LOG, 'w', encoding='utf-8', errors='replace') as f:
    p = subprocess.Popen(['cmd', '/c', 'build_test_tcp.bat'], cwd=SCRIPTS,
                         stdout=f, stderr=subprocess.STDOUT)
    try:
        rc = p.wait(timeout=LIMIT)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        rc = 'TIMEOUT after %ds' % LIMIT

print('RC =', rc)
txt = open(LOG, encoding='utf-8', errors='replace').read()
print(txt[-5000:])
