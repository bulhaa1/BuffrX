"""Test automatic download and damaged-download recovery against a local release feed."""

import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent
ASSET = b"BuffrX disposable update fixture"
DIGEST = hashlib.sha256(ASSET).hexdigest()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/release":
            url = f"http://127.0.0.1:{self.server.server_port}/asset"
            body = json.dumps({"tag_name": "0.6.0", "body": "test", "assets": [{
                "name": "BuffrX-Setup.exe", "browser_download_url": url, "digest": "sha256:" + DIGEST
            }]}).encode()
        else:
            body = ASSET
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def ask(pipe, command, args=None):
    request = json.dumps({"id": 1, "cmd": command, "args": args or {}}).encode()
    with open("\\\\.\\pipe\\" + pipe, "r+b", buffering=0) as stream:
        stream.write(struct.pack("<I", len(request)) + request)
        while True:
            header = stream.read(4)
            if len(header) != 4:
                raise RuntimeError("engine closed the pipe")
            size = struct.unpack("<I", header)[0]
            body = bytearray()
            while len(body) < size:
                part = stream.read(size - len(body))
                if not part:
                    raise RuntimeError("engine closed the pipe")
                body.extend(part)
            reply = json.loads(body)
            if reply.get("id") == 1:
                return reply


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=Path, help="engine built with BUFFRX_AUTO_UPDATE=ON")
    args = parser.parse_args()
    paths = json.loads((ROOT / "work" / "paths.json").read_text(encoding="utf-8"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="buffrx-update-auto-", ignore_cleanup_errors=True) as temporary:
            root = Path(temporary)
            clips = root / "clips"
            clips.mkdir()
            (root / "config.json").write_text(json.dumps({
                "start_buffering": False, "launch_on_startup": False, "notifications": False,
                "clips_dir": str(clips), "screenshots_dir": str(root / "shots"), "recordings_dir": str(root / "records")
            }), encoding="utf-8")
            pipe = "buffrx-update-test-" + uuid.uuid4().hex
            env = os.environ.copy()
            env.update(BUFFRX_DATA_ROOT=str(root), BUFFRX_PIPE=pipe, BUFFRX_RUNTIME_ROOT=str(ROOT / "runtime"),
                       BUFFRX_NO_HOTKEYS="1", BUFFRX_UPDATE_FEED=f"http://127.0.0.1:{server.server_port}/release")
            env["PATH"] = paths["llvm_bin"] + os.pathsep + str(ROOT / "runtime" / "bin" / "64bit") + os.pathsep + env["PATH"]
            process = subprocess.Popen([str(args.engine.resolve())], env=env, creationflags=subprocess.CREATE_NO_WINDOW,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 20
                while True:
                    try:
                        reply = ask(pipe, "update.check", {"force": True})
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("test engine did not open its pipe")
                        time.sleep(0.1)
                assert reply["ok"] and reply["data"]["latest"] == "0.6.0", reply

                def wait_ready():
                    limit = time.monotonic() + 15
                    while time.monotonic() < limit:
                        state = ask(pipe, "status.get")["data"]["update"]
                        if state["state"] == "ready":
                            return
                        if state["state"] == "failed":
                            raise AssertionError(state)
                        time.sleep(0.1)
                    raise AssertionError("download did not become ready")

                wait_ready()
                setup = root / "update" / "BuffrX-Setup.exe"
                assert setup.read_bytes() == ASSET and (root / "update" / "apply").exists()

                # Restart now reaches the action worker, which cannot find the launcher at this test's app root:
                # the command must answer, and the refusal must come back as state, never as a hang.
                accepted = ask(pipe, "update.install")
                assert accepted["ok"] and accepted["data"]["accepted"], accepted
                limit = time.monotonic() + 10
                while time.monotonic() < limit:
                    state = ask(pipe, "status.get")["data"]["update"]
                    if state["state"] == "failed":
                        assert "restart" in state["error"], state
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("the failed install never reported back")

                setup.write_bytes(b"corrupted")
                refused = ask(pipe, "update.install")
                assert not refused["ok"] and not setup.exists(), refused
                assert ask(pipe, "update.check", {"force": True})["ok"]
                wait_ready()
                assert setup.read_bytes() == ASSET
                print("update auto: download and damaged-file recovery passed")
            finally:
                process.terminate()
                process.wait(timeout=15)
                time.sleep(0.5)  # Windows may release the engine's log handle just after process exit.
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
