"""Serve one folder over http on localhost with caching off, so a page under edit is never shown from a stale copy.
Byte ranges are answered (a browser cannot seek in a video without them, as on any real host).

    python tools/serve-static.py <folder> <port> [host]

The third argument is the address to bind. It defaults to 127.0.0.1; pass 0.0.0.0 to
reach the page from a phone on the same network.
"""
import functools, http.server, os, re, sys

class NoStore(http.server.SimpleHTTPRequestHandler):
    _left = None

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Accept-Ranges', 'bytes')
        super().end_headers()

    def send_head(self):
        self._left = None
        asked = self.headers.get('Range')
        path = self.translate_path(self.path)
        match = re.match(r'bytes=(\d*)-(\d*)$', (asked or '').strip())
        if not match or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        first, last = match.groups()
        start = int(first) if first else max(0, size - int(last or 0))
        end = min(int(last), size - 1) if (first and last) else size - 1
        if start > end:
            self.send_error(416, 'Requested Range Not Satisfiable')
            return None
        source = open(path, 'rb')
        source.seek(start)
        self._left = end - start + 1
        self.send_response(206)
        self.send_header('Content-Type', self.guess_type(path))
        self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
        self.send_header('Content-Length', str(self._left))
        self.end_headers()
        return source

    def copyfile(self, source, outputfile):
        if self._left is None:
            return super().copyfile(source, outputfile)
        left = self._left
        while left > 0:
            chunk = source.read(min(65536, left))
            if not chunk:
                break
            outputfile.write(chunk)
            left -= len(chunk)

http.server.test(HandlerClass=functools.partial(NoStore, directory=sys.argv[1]), port=int(sys.argv[2]), bind=sys.argv[3] if len(sys.argv) > 3 else '127.0.0.1')
