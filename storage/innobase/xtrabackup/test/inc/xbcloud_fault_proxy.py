#!/usr/bin/env python3
"""Proxy for xbcloud tests that breaks requests to the object storage.

The proxy listens on BIND (127.0.0.1 by default) and PORT (a free one by
default), writes the port to PORT_FILE and forwards to UPSTREAM (host:port of
the object storage). Without --match or --duration it only forwards. Faults:

  --mode http (default)
      Requests are forwarded one by one, except those the proxy answers
      itself with HTTP STATUS and an S3 style error body, like a proxy whose
      backend is down (502) or a throttling or busy service (429, 503):
        --match REGEX  requests whose request line ("PUT /path HTTP/1.1")
                       matches REGEX: the first COUNT of them after SKIP
        otherwise      all requests for DURATION seconds once AFTER_BYTES
                       bytes have been sent upstream
      The proxy reads each request and response, so that it can answer
      requests itself on connections that curl keeps open and reuses.

  --mode refuse
      Bytes are copied as they are. Once AFTER_BYTES bytes have been sent
      upstream, the listener and all open connections are closed, so new
      connections are refused, like a dead proxy or load balancer. After
      DURATION seconds the proxy listens on the same port again.

Each fault is logged to stdout with a timestamp.
"""

import argparse
import asyncio
import re
import time


def log(msg):
    print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


class Proxy:
    def __init__(self, args):
        self.args = args
        self.upstream_host, port = args.upstream.rsplit(':', 1)
        self.upstream_port = int(port)
        self.matched = 0
        self.sent = 0
        self.fault_start = None
        self.server = None
        self.port = args.port
        self.writers = set()

    def fault_due(self):
        """Start the fault window once enough bytes went upstream."""
        if self.fault_start is None and self.sent >= self.args.after_bytes:
            self.fault_start = time.time()
            log('fault %s start, %d bytes sent' % (self.args.mode, self.sent))
            return True
        return False

    def inject(self, request_line):
        if not self.args.match and self.args.duration <= 0:
            return False
        if self.args.match:
            if not re.search(self.args.match, request_line):
                return False
            self.matched += 1
            return (self.args.skip < self.matched <=
                    self.args.skip + self.args.count)
        self.fault_due()
        return (self.fault_start is not None and
                time.time() < self.fault_start + self.args.duration)

    async def read_head(self, reader):
        head = await reader.readuntil(b'\r\n\r\n')
        lines = head.decode('latin-1').split('\r\n')
        headers = {}
        for line in lines[1:]:
            if ':' in line:
                k, v = line.split(':', 1)
                headers[k.strip().lower()] = v.strip()
        return head, lines[0], headers

    async def read_body(self, reader, headers):
        if 'chunked' in headers.get('transfer-encoding', '').lower():
            body = b''
            while True:
                size_line = await reader.readuntil(b'\r\n')
                body += size_line
                size = int(size_line.split(b';')[0], 16)
                body += await reader.readexactly(size + 2)
                if size == 0:
                    return body
        return await reader.readexactly(int(headers.get('content-length', 0)))

    async def handle_http(self, client_reader, client_writer):
        """Forward one request at a time over a private upstream connection,
        or answer it with the fault status."""
        upstream = None
        try:
            while True:
                head, request_line, headers = await self.read_head(client_reader)
                method = request_line.split(' ', 1)[0]
                if self.inject(request_line):
                    # Answer before the body is sent: with "Expect:
                    # 100-continue" curl waits for us and then skips the body.
                    if 'expect' not in headers:
                        await self.read_body(client_reader, headers)
                    body = (b'<?xml version="1.0" encoding="UTF-8"?><Error>'
                            b'<Code>InjectedFault</Code><Message>injected by '
                            b'xbcloud_fault_proxy</Message></Error>')
                    client_writer.write(
                        b'HTTP/1.1 %d Injected\r\nContent-Type: application/xml'
                        b'\r\nContent-Length: %d\r\nConnection: close\r\n\r\n'
                        % (self.args.status, len(body)) + body)
                    await client_writer.drain()
                    log('injected %d for %s' % (self.args.status, request_line))
                    break
                if 'expect' in headers:
                    # Answer the 100-continue ourselves and drop the header,
                    # so the upstream sees a plain request.
                    client_writer.write(b'HTTP/1.1 100 Continue\r\n\r\n')
                    await client_writer.drain()
                    head = b'\r\n'.join(
                        l for l in head.split(b'\r\n')
                        if not l.lower().startswith(b'expect:'))
                body = await self.read_body(client_reader, headers)
                self.sent += len(head) + len(body)
                if upstream is None:
                    upstream = await asyncio.open_connection(
                        self.upstream_host, self.upstream_port)
                up_reader, up_writer = upstream
                up_writer.write(head + body)
                await up_writer.drain()
                rhead, status_line, rheaders = await self.read_head(up_reader)
                status = int(status_line.split(' ')[1])
                rbody = b''
                if method != 'HEAD' and status not in (204, 304):
                    rbody = await self.read_body(up_reader, rheaders)
                client_writer.write(rhead + rbody)
                await client_writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            client_writer.close()
            if upstream is not None:
                upstream[1].close()

    async def refuse(self):
        self.server.close()
        for w in list(self.writers):
            w.transport.abort()
        await asyncio.sleep(self.args.duration)
        await self.listen()
        log('fault refuse end, listening again')

    async def pipe(self, reader, writer, upstream):
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                if upstream:
                    self.sent += len(data)
                    if self.fault_due():
                        asyncio.get_running_loop().create_task(self.refuse())
                        break
                writer.write(data)
                await writer.drain()
        except Exception:
            pass
        finally:
            writer.close()

    async def handle_refuse(self, client_reader, client_writer):
        try:
            up_reader, up_writer = await asyncio.open_connection(
                self.upstream_host, self.upstream_port)
        except OSError:
            client_writer.transport.abort()
            return
        self.writers.update((client_writer, up_writer))
        await asyncio.gather(self.pipe(client_reader, up_writer, True),
                             self.pipe(up_reader, client_writer, False))
        self.writers.difference_update((client_writer, up_writer))

    async def listen(self):
        handler = (self.handle_refuse if self.args.mode == 'refuse' else
                   self.handle_http)
        self.server = await asyncio.start_server(handler, self.args.bind,
                                                 self.port)
        self.port = self.server.sockets[0].getsockname()[1]

    async def main(self):
        await self.listen()
        with open(self.args.port_file, 'w') as f:
            f.write(str(self.port))
        log('listening on %s:%d, upstream %s' %
            (self.args.bind, self.port, self.args.upstream))
        while True:
            await asyncio.sleep(3600)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--upstream', required=True, help='host:port')
    parser.add_argument('--port-file', required=True)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--mode', choices=('http', 'refuse'), default='http')
    parser.add_argument('--status', type=int, default=503)
    parser.add_argument('--match', default='')
    parser.add_argument('--skip', type=int, default=0)
    parser.add_argument('--count', type=int, default=1)
    parser.add_argument('--after-bytes', type=int, default=0)
    parser.add_argument('--duration', type=float, default=0)
    asyncio.run(Proxy(parser.parse_args()).main())


if __name__ == '__main__':
    main()
