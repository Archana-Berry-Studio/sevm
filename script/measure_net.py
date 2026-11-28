"""script/measure_net.py — spawn the SEVM server, fire N requests, report.

usage: python script/measure_net.py [count] [route] [engine]
"""
import subprocess, sys, time, os, urllib.request, urllib.error, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EX = os.path.join(ROOT, 'examples', 'sevm-notes')
ENGINE = sys.argv[3] if len(sys.argv) > 3 else r'E:/bcf/mochiberry/build_manual/mbjs-host.exe'
N = int(sys.argv[1]) if len(sys.argv) > 1 else 60
ROUTE = sys.argv[2] if len(sys.argv) > 2 else '/api/notes'
PORT = 8080

log = open(os.path.join(HERE, '..', 'scratch', 'measure_server.log'), 'wb')
srv = subprocess.Popen([ENGINE, '-m', 'src/server.js'], cwd=EX, stdout=log,
                       stderr=subprocess.STDOUT)

# wait for the listener
up = False
for _ in range(80):
    time.sleep(0.25)
    try:
        urllib.request.urlopen(f'http://127.0.0.1:{PORT}/api/health', timeout=2).read()
        up = True
        break
    except Exception:
        pass
if not up:
    print('SERVER DID NOT COME UP')
    srv.kill()
    sys.exit(2)

ok = fail = 0
first_fail = None
codes = []
for i in range(N):
    try:
        r = urllib.request.urlopen(f'http://127.0.0.1:{PORT}{ROUTE}', timeout=5)
        r.read()
        codes.append(r.status)
        ok += 1
    except urllib.error.HTTPError as e:
        codes.append(e.code)
        fail += 1
        if first_fail is None:
            first_fail = i
    except Exception as e:
        codes.append(type(e).__name__)
        fail += 1
        if first_fail is None:
            first_fail = i

print(f'route={ROUTE} n={N} ok={ok} fail={fail} first_fail_at={first_fail}')
print('codes:', ' '.join(str(c) for c in codes))

srv.send_signal(signal.SIGTERM)
try:
    srv.wait(timeout=5)
except Exception:
    srv.kill()
log.close()
tail = open(os.path.join(ROOT, 'scratch', 'measure_server.log'), 'r',
             errors='replace').read().splitlines()[-25:]
print('--- server log tail ---')
for line in tail:
    print(' ', line)
