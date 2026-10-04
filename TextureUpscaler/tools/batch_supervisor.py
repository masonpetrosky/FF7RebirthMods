"""Runs a long upscaling batch only while the game is closed.

Usage: batch_supervisor.py <log file> -- <build command ...>

Every 30 s: if FF7 Rebirth is running, the batch is stopped (it resumes from its cache later);
once the game has been closed for IDLE_MINUTES, the batch is (re)started. When the batch
finishes successfully the supervisor exits.
"""

from __future__ import annotations

import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

IDLE_MINUTES = 10
POLL_SECONDS = 30
GAME = "ff7rebirth_.exe"


def game_running() -> bool:
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {GAME}", "/NH"], capture_output=True, text=True).stdout
    return GAME in out.lower()


def log(path: Path, message: str) -> None:
    with open(path, "a", encoding="utf-8") as f:
        f.write(f"[supervisor {datetime.now():%Y-%m-%d %H:%M:%S}] {message}\n")


def main() -> None:
    log_path = Path(sys.argv[1])
    command = sys.argv[sys.argv.index("--") + 1:]
    batch: subprocess.Popen | None = None
    last_game_seen = time.time() if game_running() else 0.0
    log(log_path, f"started; batch runs {IDLE_MINUTES} min after the game closes")
    while True:
        if game_running():
            last_game_seen = time.time()
            if batch and batch.poll() is None:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(batch.pid)], capture_output=True)
                batch.wait()
                log(log_path, "game started: batch paused")
            batch = None
        elif batch is None and time.time() - last_game_seen >= IDLE_MINUTES * 60:
            log(log_path, "game closed: batch resumed")
            batch = subprocess.Popen(command, stdout=open(log_path, "a", encoding="utf-8"), stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
        if batch is not None and batch.poll() is not None:
            if batch.returncode == 0:
                log(log_path, "batch finished")
                return
            log(log_path, f"batch exited with code {batch.returncode}; retrying after the idle delay")
            batch = None
            last_game_seen = time.time()
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
