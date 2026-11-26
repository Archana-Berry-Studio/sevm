#!/usr/bin/env python3
"""Client for the SEVM AOT remote debugger (debug level 3).

The debug binary opens a two-way line on 127.0.0.1:9229 (SEVM_DEBUG_PORT to
override).  This client speaks it:

  python tools/aot/dbg_client.py                      one-shot REPL
  python tools/aot/dbg_client.py ping                 one command
  python tools/aot/dbg_client.py --tail 5             follow the log stream
  python tools/aot/dbg_client.py --port 9333 ...      other port

Commands on the other side: ping, info, modules, bakes, logs [n],
stream, nostream, mem <addr> <len>, sym <addr>, eval <expr>, shutdown.
"""
import socket
import sys
import time


def connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    return s


def recv_line(s, timeout=6.0):
    """Read until the newline the debugger always terminates its answer with."""
    s.settimeout(timeout)
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    return buf.decode("utf-8", "replace")


def one(s, cmd, timeout=6.0):
    s.sendall((cmd + "\n").encode("utf-8"))
    return recv_line(s, timeout)


def tail(s, seconds):
    s.sendall(b"stream\n")
    s.settimeout(seconds)
    buf = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            sys.stdout.write(buf.decode("utf-8", "replace"))
            sys.stdout.flush()
            buf = b""
    except socket.timeout:
        pass
    s.sendall(b"nostream\n")
    return b""


def repl(s, host, port):
    print("sevm debugger %s:%d  —  'help' lists the commands, 'exit' leaves"
          % (host, port))
    while True:
        try:
            line = input("dbg> ")
        except (EOFError, KeyboardInterrupt):
            break
        line = line.strip()
        if not line:
            continue
        if line in ("exit", "quit"):
            break
        if line == "tail":
            tail(s, 5)
            continue
        try:
            sys.stdout.write(one(s, line))
        except socket.timeout:
            print("(no reply within 6 s)")
    s.close()


def main():
    args = sys.argv[1:]
    host, port, tail_for = "127.0.0.1", 9229, None
    cmd = None
    i = 0
    while i < len(args):
        if args[i] == "--port" and i + 1 < len(args):
            port = int(args[i + 1]); i += 2; continue
        if args[i] == "--tail" and i + 1 < len(args):
            tail_for = float(args[i + 1]); i += 2; continue
        if args[i] == "--tail":
            tail_for = 5.0; i += 1; continue
        cmd = " ".join(args[i:])
        break
    try:
        s = connect(host, port)
    except OSError as e:
        print("cannot reach the debugger at %s:%d: %s" % (host, port, e),
              file=sys.stderr)
        return 2
    if tail_for:
        tail(s, tail_for)
        s.close()
        return 0
    if cmd:
        sys.stdout.write(one(s, cmd))
        s.close()
        return 0
    repl(s, host, port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
