#!/usr/bin/env python3
"""Pull dongle log files over HTTP every 0.5s, append new bytes locally.

The dongle symlinks k.txt (kernel), m.txt (ram snapshot) and log4.txt (logcat)
into Boa's docroot on the RAM disk. Files are append-only until truncation or
reboot, so dedupe by prefix; log a marker when the feed is lost or reset.
"""
import os
import time
import urllib.request

BASE = os.path.dirname(os.path.abspath(__file__))
DONGLE = "http://192.168.8.160"
POLL = [("k.txt", "k_stream.log"), ("m.txt", "m_stream.log"), ("log4.txt", "logcat_stream.log")]

state = {}
last_ok = {}
logs = {}
for name, fn in POLL:
    state[name] = b""
    last_ok[name] = time.time()
    logs[name] = open(os.path.join(BASE, fn), "ab", 0)

while True:
    for name, fn in POLL:
        try:
            with urllib.request.urlopen("%s/%s" % (DONGLE, name), timeout=2) as r:
                data = r.read()
        except Exception:
            if time.time() - last_ok[name] > 5:
                logs[name].write(("\n=== LOST %s ===\n" % time.strftime("%H:%M:%S")).encode())
                last_ok[name] = time.time()
            continue
        prev = state[name]
        if data.startswith(prev):
            new = data[len(prev):]
            if new:
                logs[name].write(new)
            state[name] = data
        else:
            logs[name].write(("\n=== RESET %s ===\n" % time.strftime("%H:%M:%S")).encode())
            logs[name].write(data)
            state[name] = data
        last_ok[name] = time.time()
    time.sleep(0.5)
