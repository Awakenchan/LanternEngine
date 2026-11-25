"""Run the native session-lock probe in separate processes with disposable saves."""
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def main():
    probe = Path(sys.argv[1]).resolve(strict=True)
    with TemporaryDirectory(prefix="lantern-save-sessions-") as folder:
        root = Path(folder)
        source = root / "user"

        def env(backup):
            return dict(os.environ, BB_SAVE_BACKUP_DIR=str(root / backup),
                        BB_CONFIG=str(root / "missing.ini"), BB_SAVE_BACKUPS="0")

        with subprocess.Popen([probe, "--session", source], cwd=root, env=env("a"),
                              text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE) as first:
            try:
                assert first.stdout.readline().startswith("Saves:")
                assert first.stdout.readline().strip() == "READY"
                for backup in ("a", "b"):
                    other = subprocess.run([probe, "--session", source], cwd=root,
                                           env=env(backup), input="", text=True,
                                           capture_output=True, timeout=10)
                    assert other.returncode == 2, other.stdout + other.stderr
                separate = subprocess.run([probe, "--session", root / "other-user"], cwd=root,
                                          env=env("a"), input="", text=True,
                                          capture_output=True, timeout=10)
                assert separate.returncode == 0, separate.stdout + separate.stderr
            finally:
                first.communicate(input="\n", timeout=10)
            assert first.returncode == 0
        print("PASS: live session exclusion across backup destinations and independent profiles")


if __name__ == "__main__":
    main()
