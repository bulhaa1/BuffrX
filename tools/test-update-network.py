"""Exercise the update feed and verified download against a local, disposable HTTP server."""

import hashlib
import http.server
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
ENGINE = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "runtime/bin/64bit/buffrx-engine.exe"
ASSET = b"BuffrX update verification fixture"
DIGEST = hashlib.sha256(ASSET).hexdigest()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/redirect"):
            self.send_response(302)
            self.send_header("Location", "http://example.com/asset")
            self.end_headers()
            return
        if self.path.startswith("/slow-header"):
            for byte in b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n":
                try:
                    self.connection.sendall(bytes([byte]))
                except OSError:
                    break
                time.sleep(0.5)
            return
        if self.path.startswith("/slow"):
            self.send_response(200)
            self.send_header("Content-Length", "100")
            self.end_headers()
            for _ in range(100):
                try:
                    self.wfile.write(b"x")
                    self.wfile.flush()
                except OSError:
                    break
                time.sleep(0.5)
            return
        body = (b'{"tag_name":"0.6.0","body":"notes","assets":[]}'
                if self.path.startswith("/feed") else ASSET)
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


with tempfile.TemporaryDirectory(prefix="buffrx-update-network-") as root:
    folder = pathlib.Path(root)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}"
    env = os.environ.copy()
    env["PATH"] = str(ROOT / "runtime/bin/64bit") + os.pathsep + env["PATH"]
    env["BUFFRX_DATA_ROOT"] = root
    env["BUFFRX_RUNTIME_ROOT"] = str(ROOT / "runtime")

    def run(args, feed="/feed?fixture=1"):
        child_env = env.copy()
        child_env["BUFFRX_UPDATE_FEED"] = base + feed
        return subprocess.run([str(ENGINE), *args], env=child_env, capture_output=True, text=True, timeout=23)

    try:
        feed = run(["--update-probe"])
        assert feed.returncode == 0 and "latest=0.6.0" in feed.stdout, feed.stdout + feed.stderr
        good_path = folder / "good.bin"
        good = run(["--update-download", base + "/asset?fixture=1", "sha256:" + DIGEST, str(good_path)])
        assert good.returncode == 0 and good_path.read_bytes() == ASSET, good.stdout + good.stderr
        bad_path = folder / "bad.bin"
        bad = run(["--update-download", base + "/asset", "sha256:" + "0" * 64, str(bad_path)])
        assert bad.returncode != 0 and not bad_path.exists() and not (folder / "bad.bin.partial").exists()
        remote_http = run(["--update-download", "http://example.com/asset", "sha256:" + DIGEST, str(bad_path)])
        assert remote_http.returncode != 0 and "not https" in remote_http.stdout
        redirect = run(["--update-download", base + "/redirect", "sha256:" + DIGEST, str(bad_path)])
        assert redirect.returncode != 0 and "answered 302" in redirect.stdout

        results = []

        def slow(args, feed, kind):
            start = time.monotonic()
            result = run(args, feed)
            results.append((kind, time.monotonic() - start, result.returncode))

        jobs = [
            threading.Thread(target=slow, args=(["--update-probe"], "/slow", "body")),
            threading.Thread(target=slow, args=(
                ["--update-download", base + "/slow", "sha256:" + "0" * 64, str(bad_path)], "/feed", "body")),
            threading.Thread(target=slow, args=(["--update-probe"], "/slow-header", "headers")),
            threading.Thread(target=slow, args=(
                ["--update-download", base + "/slow-header", "sha256:" + "0" * 64, str(folder / "headers.bin")],
                "/feed", "headers")),
        ]
        for job in jobs:
            job.start()
        for job in jobs:
            job.join()
        assert len(results) == 4 and all(code != 0 and seconds < 18 for _, seconds, code in results), results
        assert not bad_path.exists() and not (folder / "bad.bin.partial").exists()
        print("UPDATE_NETWORK_TEST_OK")
    finally:
        server.shutdown()
        server.server_close()
