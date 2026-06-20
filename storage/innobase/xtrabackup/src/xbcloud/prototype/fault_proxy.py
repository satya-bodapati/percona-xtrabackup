#!/usr/bin/env python3
"""
fault_proxy.py - HTTP proxy that injects 503 errors on a fraction of
requests, otherwise transparently forwards to an upstream server.

Used by smoke_retry.sh to exercise xbcloud's retry/backoff path
(http.cc) without needing LocalStack Pro's failure-injection feature.

Behavior:
  - Listens on --listen-port (default 4567).
  - Forwards every request to UPSTREAM (default http://localhost:4566).
  - Returns HTTP 503 for FAULT_RATE percent of requests instead of
    forwarding. The decision uses request count modulo (1/rate) so the
    pattern is deterministic and reproducible across runs.
  - Emits a one-line log per request (timestamp, method, path, action).

Run:
    python3 fault_proxy.py --listen-port 4567 \
                           --upstream http://localhost:4566 \
                           --fault-rate 0.3

Stop with SIGINT/SIGTERM. The script handles connection close cleanly
so xbcloud's retry path sees a real HTTP 503 (not a TCP reset).
"""

import argparse
import http.server
import socketserver
import sys
import time
import urllib.error
import urllib.request


def make_handler(upstream, fault_rate, inject_methods, delay_ms):
    counter = {"n": 0}
    inject_every = 0 if fault_rate <= 0 else int(round(1.0 / fault_rate))

    class Proxy(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            sys.stderr.write("%s [fault_proxy] %s\n" % (
                self.log_date_time_string(), fmt % args))

        def _serve(self, method):
            counter["n"] += 1
            n = counter["n"]
            # WAN latency simulation: sleep BEFORE doing anything else so
            # the delay shows up in curl's CONNECT_TIME / STARTTRANSFER_TIME
            # (it's a delay on the server-response side, which is exactly
            # what a real high-latency endpoint looks like).
            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)
            eligible = (method in inject_methods) if inject_methods else True
            injected = eligible and (inject_every > 0
                                     and (n % inject_every) == 0)
            if injected:
                body = b"<Error><Code>SlowDown</Code><Message>"
                body += b"injected by fault_proxy</Message></Error>"
                self.send_response(503)
                self.send_header("Content-Type", "application/xml")
                self.send_header("Content-Length", str(len(body)))
                # NO Connection: close -- we want curl to reuse the TCP
                # connection across calls. HTTP/1.1 default is keep-alive.
                # Content-Length is set above so the response framing is
                # unambiguous and the connection stays safely poolable.
                self.end_headers()
                self.wfile.write(body)
                self.log_message("INJECT 503 %s %s (#%d)", method, self.path, n)
                return

            url = upstream.rstrip("/") + self.path
            length = int(self.headers.get("Content-Length", "0") or "0")
            data = self.rfile.read(length) if length > 0 else None

            req = urllib.request.Request(url, data=data, method=method)
            for h, v in self.headers.items():
                if h.lower() in ("host", "content-length"): continue
                req.add_header(h, v)

            try:
                with urllib.request.urlopen(req, timeout=60) as up:
                    body = up.read()
                    self.send_response(up.status)
                    for h, v in up.getheaders():
                        if h.lower() in ("transfer-encoding", "connection"):
                            continue
                        self.send_header(h, v)
                    self.send_header("Content-Length", str(len(body)))
                    # NO Connection: close -- we want curl to reuse the TCP
                # connection across calls. HTTP/1.1 default is keep-alive.
                # Content-Length is set above so the response framing is
                # unambiguous and the connection stays safely poolable.
                    self.end_headers()
                    self.wfile.write(body)
                    self.log_message("FWD %d %s %s (#%d)", up.status, method,
                                     self.path, n)
            except urllib.error.HTTPError as e:
                body = e.read()
                self.send_response(e.code)
                self.send_header("Content-Length", str(len(body)))
                # NO Connection: close -- we want curl to reuse the TCP
                # connection across calls. HTTP/1.1 default is keep-alive.
                # Content-Length is set above so the response framing is
                # unambiguous and the connection stays safely poolable.
                self.end_headers()
                self.wfile.write(body)
                self.log_message("FWD %d %s %s (#%d)", e.code, method,
                                 self.path, n)

        def do_GET(self):    self._serve("GET")
        def do_PUT(self):    self._serve("PUT")
        def do_POST(self):   self._serve("POST")
        def do_DELETE(self): self._serve("DELETE")
        def do_HEAD(self):   self._serve("HEAD")

    return Proxy


class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen-port", type=int, default=4567)
    ap.add_argument("--upstream", default="http://localhost:4566")
    ap.add_argument("--fault-rate", type=float, default=0.3)
    ap.add_argument("--inject-methods", default="PUT",
                    help="comma-separated HTTP methods eligible for injection")
    ap.add_argument("--delay-ms", type=int, default=0,
                    help="add this many ms of latency to every request "
                         "(WAN simulation). 0 = disabled.")
    args = ap.parse_args()

    methods = set(m.strip().upper() for m in args.inject_methods.split(",")
                  if m.strip())
    handler = make_handler(args.upstream, args.fault_rate, methods,
                           args.delay_ms)
    server = ThreadingServer(("0.0.0.0", args.listen_port), handler)
    sys.stderr.write(
        "fault_proxy listening on :%d -> %s (fault_rate=%.2f)\n"
        % (args.listen_port, args.upstream, args.fault_rate))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
