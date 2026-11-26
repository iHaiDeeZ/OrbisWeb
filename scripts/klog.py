#!/usr/bin/env python3
# Reads the PS4's kernel log from GoldHEN's klog server for a few seconds and saves it.
#
#   python scripts/klog.py <console ip> <out file> [seconds, default 12]
#
# The server sends the kernel's message buffer first, so read right after a test: a crash that left no
# crash.log (the PS4 closed the app) is still described there: the system's "A user thread receives a fatal
# signal" report (signal, thread name, process, registers, backtrace with absolute addresses: subtract the
# image base 0x400000, and 1 for a return address, then symbolize against out/../oelf/<binary>-vXX.YY.elf),
# its AppCrash / KillApp lines, and ShellCore's "FMEM used/peak" list per process.
import socket
import sys
import time

host = sys.argv[1]
out = sys.argv[2]
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0

data = b""
with socket.create_connection((host, 3232), timeout=5) as s:
    s.settimeout(1.0)
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        data += chunk
with open(out, "wb") as f:
    f.write(data.replace(b"\r", b""))
print(f"{len(data)} bytes -> {out}")
