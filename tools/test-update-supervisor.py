"""Exercise the update supervisor with fake executables in an isolated directory on Windows."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
PATHS = json.loads((ROOT / "work" / "paths.json").read_text(encoding="utf-8"))
SOURCE = r'''
#include <windows.h>
#include <cstdio>
#include <string>
int main() {
  char path[MAX_PATH * 2] = {};
  GetModuleFileNameA(nullptr, path, sizeof(path));
  std::string dir(path);
  dir.resize(dir.find_last_of("\\/"));
#ifdef FAKE_SETUP
  const std::string output = dir + "\\setup-args.txt";
#else
  const std::string output = dir + "\\launch-args.txt";
#endif
  FILE* file = std::fopen(output.c_str(), "wb");
  if (!file) return 2;
  std::fputs(GetCommandLineA(), file);
  std::fclose(file);
#ifdef FAKE_SETUP
  if (GetFileAttributesA((dir + "\\fail").c_str()) != INVALID_FILE_ATTRIBUTES) return 7;
  char app[MAX_PATH * 2] = {};
  if (GetEnvironmentVariableA("BUFFRX_TEST_APP_ROOT", app, sizeof(app))) {
    if (!CopyFileA((dir + "\\fake-next.exe").c_str(), (std::string(app) + "\\BuffrX.exe").c_str(), FALSE)) return 8;
  }
  return 0;
#else
  return 0;
#endif
}
'''


def wait_for(path):
    deadline = time.monotonic() + 5
    while not path.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    assert path.exists(), f"not created: {path}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--engine", action="store_true", help="also test the auto-update-enabled engine")
    args = parser.parse_args()
    launcher = args.build / "buffrx-launcher.exe"
    assert launcher.exists(), "Build buffrx-launcher first"
    with tempfile.TemporaryDirectory(prefix="buffrx-update-test-", ignore_cleanup_errors=True) as tmp:
        root = Path(tmp)
        app = root / "app with spaces"
        update = root / "data with spaces" / "update"
        app.mkdir()
        update.mkdir(parents=True)
        source = root / "fixture.cpp"
        source.write_text(SOURCE, encoding="utf-8")
        for output, flags in ((app / "BuffrX.exe", []), (update / "BuffrX-Setup.exe", ["-DFAKE_SETUP"])):
            subprocess.run([PATHS["clangxx"], "-static", str(source), "-o", str(output), *flags], check=True)

        def supervise(autostart):
            subprocess.run([str(launcher), "--update-supervisor", str(app), str(update.parent), "0", autostart],
                           check=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
            wait_for(app / "launch-args.txt")
            return (app / "launch-args.txt").read_text()

        launch = supervise("1")
        setup = (update / "setup-args.txt").read_text()
        assert "/VERYSILENT" in setup and "/SUPPRESSMSGBOXES" in setup and "/NORESTART" in setup
        assert f'/DIR="{app}"' in setup
        assert "--update-resume" in launch and "--autostart" in launch
        assert not (update / "failed").exists()

        (app / "launch-args.txt").unlink()
        (update / "fail").touch()
        (update / "apply").touch()
        launch = supervise("0")
        assert (update / "failed").exists()
        assert not (update / "apply").exists()
        assert "--update-resume" in launch and "--autostart" not in launch
        print("update supervisor: success and failure recovery passed")

        if args.engine:
            (update / "fail").unlink()
            (update / "failed").unlink()
            (app / "launch-args.txt").unlink()
            shutil.copy2(app / "BuffrX.exe", update / "fake-next.exe")
            shutil.copy2(launcher, app / "BuffrX.exe")
            setup_path = update / "BuffrX-Setup.exe"
            digest = hashlib.sha256(setup_path.read_bytes()).hexdigest()
            (update / "pending.json").write_text(json.dumps({
                "version": "99.0.0", "setup": str(setup_path), "sha256": "sha256:" + digest
            }), encoding="utf-8")
            (update / "apply").touch()
            env = os.environ.copy()
            env.update(BUFFRX_DATA_ROOT=str(update.parent), BUFFRX_RUNTIME_ROOT=str(app), BUFFRX_TEST_APP_ROOT=str(app))
            env["PATH"] = PATHS["llvm_bin"] + os.pathsep + str(ROOT / "runtime" / "bin" / "64bit") + os.pathsep + env["PATH"]
            subprocess.run([str(args.build / "buffrx-engine.exe"), "--autostart"], env=env,
                           check=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
            wait_for(app / "launch-args.txt")
            launch = (app / "launch-args.txt").read_text()
            assert "--update-resume" in launch and "--autostart" in launch
            assert json.loads((update / "pending.json").read_text())["install_attempts"] == 1
            print("update engine: Windows startup handoff and restart passed")

            (app / "launch-args.txt").unlink()
            (update / "apply").touch()
            pending = json.loads((update / "pending.json").read_text())
            pending["sha256"] = "sha256:" + "0" * 64
            (update / "pending.json").write_text(json.dumps(pending), encoding="utf-8")
            result = subprocess.run([str(args.build / "buffrx-engine.exe"), "--apply-update"], env=env,
                                    timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
            assert result.returncode != 0
            assert not setup_path.exists() and not (update / "apply").exists()
            assert not (update / "pending.json").exists()
            assert not (app / "launch-args.txt").exists()
            print("update engine: corrupted installer rejected")


if __name__ == "__main__":
    main()
