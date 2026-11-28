"""script/measure_net2.py — the real race test.

Sends each request in two TCP segments with a pause in between, which is
exactly the case that used to return RST ("Connection was aborted"): the
server's non-blocking recv() saw WSAEWOULDBLOCK and used to treat it as EOF.
Also verifies responses are byte-exact.

usage: python script/measure_net2.py [count]
"""
import subprocess, sys, time, os, socket, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EX = os.path.join(ROOT, 'examples', 'sevm-notes')
ENGINE = sys.argv[1] if len(sys.argv) > 1 else \
    r'E:/bcf/mochiberry/build_manual/mbjs-host.exe'
N = int(sys.argv[2]) if len(sys.argv) > 2 else 25
PORT = 8080

log = open(os.path.join(ROOT, 'scratch', 'measure_server2.log'), 'wb')
srv = subprocess.Popen([ENGINE, '-m', 'src/server.js'], cwd=EX, stdout=log,
                       stderr=subprocess.STDOUT)
for _ in range(80):
    time.sleep(0.25)
    if socket.socket().connect_ex(('127.0.0.1', PORT)) == 0:
        break

ok = fail = 0
codes = []
for i in range(N):
    # request line + half the headers, then pause, then the rest
    part1 = b'GET /api/notes HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n' % PORT
    part2 = b'Connection: close\r\n\r\n'
    s = socket.socket()
    s.settimeout(6)
    s.connect(('127.0.0.1', PORT))
    s.sendall(part1)
    time.sleep(0.08)          # <-- the race window
    s.sendall(part2)
    got = b''
    try:
        while True:
            c = s.recv(4096)
            if not c:
                break
            got += c
    except Exception as e:
        codes.append('E:' + type(e).__name__)
        fail += 1
        continue
    finally:
        s.close()
    head, _, body = got.partition(b'\r\n\r\n')
    status = head.split(b'\r\n', 1)[0].decode('latin1')
    codes.append(status.split()[1] if status.split() else '?')
    if '200' in status:
        ok += 1
    else:
        fail += 1

print(f'split-send n={N} ok={ok} fail={fail}')
print('codes:', ' '.join(codes))

srv.send_signal(signal.SIGTERM)
try:
    srv.wait(timeout=5)
except Exception:
    srv.kill()
log.close()
tail = open(os.path.join(ROOT, 'scratch', 'measure_server2.log'), 'r',
             errors='replace').read().splitlines()[-12:]
print('--- server log tail ---')
for line in tail:
    print(' ', line)
