#!/usr/bin/env python3
"""Verified local-only retention pruner for RepublicMP spool on VPS.

Never modifies or deletes anything on Google Drive. After one-way `rclone copy`
finishes, inspects local files older than REPUBLICMP_LOCAL_RETENTION_DAYS (default 14)
and removes ONLY those local files whose exact path and size are confirmed present
on `gdrive:republicmp-logs` via `rclone lsjson --files-only`.
"""

import json
import os
import re
import subprocess
import sys
import time

SPOOL_READY = os.environ.get("REPUBLICMP_SPOOL_READY", "/var/republicmp/spool/ready")
GDRIVE_TARGET = os.environ.get("REPUBLICMP_GDRIVE_TARGET", "gdrive:republicmp-logs").rstrip("/")
RCLONE_BIN = os.environ.get("RCLONE_BIN", "/usr/local/bin/rclone")
RCLONE_CONFIG = os.environ.get("RCLONE_CONFIG", "/root/.config/rclone/rclone.conf")
RETENTION_DAYS = int(os.environ.get("REPUBLICMP_LOCAL_RETENTION_DAYS", "14"))

SECRET_RE = re.compile(
    r"(?i)(access_token|refresh_token|id_token|client_secret|token|password|secret)([:=]\s*)([^\s\"',;)\]}]+)"
)


def _redact(text):
    return SECRET_RE.sub(r"\1\2[REDACTED]", text or "")


def _clean_rclone_env():
    return {
        "HOME": "/root",
        "PATH": "/usr/local/bin:/usr/bin:/bin",
        "RCLONE_CONFIG": RCLONE_CONFIG,
    }


def _verify_remote_file(rel_posix, expected_size):
    remote_path = "%s/%s" % (GDRIVE_TARGET, rel_posix)
    proc = subprocess.run(
        [RCLONE_BIN, "lsjson", "--files-only", "--", remote_path],
        env=_clean_rclone_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
    )
    if proc.returncode != 0:
        err = _redact(proc.stderr.decode("utf-8", errors="replace").strip())
        print("verify skipped for %s (rclone exit %d: %s)" % (rel_posix, proc.returncode, err[:200]), file=sys.stderr)
        return False
    try:
        items = json.loads(proc.stdout.decode("utf-8", errors="replace") or "[]")
    except ValueError:
        return False
    base = os.path.basename(rel_posix)
    for item in items:
        if item.get("Name") == base and int(item.get("Size", -1)) == expected_size:
            return True
    return False


def main():
    if not (GDRIVE_TARGET == "gdrive:republicmp-logs" or GDRIVE_TARGET.startswith("gdrive:republicmp-logs/")):
        print("Refusing unsafe GDRIVE_TARGET: %s" % GDRIVE_TARGET, file=sys.stderr)
        return 2
    if not os.path.isdir(SPOOL_READY):
        return 0

    cutoff_epoch = time.time() - (RETENTION_DAYS * 86400)
    pruned = 0
    for root, _dirs, files in os.walk(SPOOL_READY):
        for name in files:
            full = os.path.join(root, name)
            try:
                st = os.stat(full)
            except OSError:
                continue
            if st.st_mtime >= cutoff_epoch:
                continue
            rel = os.path.relpath(full, SPOOL_READY).replace("\\", "/")
            if not rel or rel.startswith("/") or ".." in rel.split("/"):
                continue
            if _verify_remote_file(rel, st.st_size):
                try:
                    os.unlink(full)
                    pruned += 1
                except OSError:
                    pass

    # Prune empty date subdirectories older than 60 minutes.
    dir_cutoff = time.time() - 3600
    for root, dirs, files in os.walk(SPOOL_READY, topdown=False):
        if root == SPOOL_READY:
            continue
        if not dirs and not files:
            try:
                if os.stat(root).st_mtime < dir_cutoff:
                    os.rmdir(root)
            except OSError:
                pass

    if pruned > 0:
        print("Pruned %d verified archived local file(s) older than %d days." % (pruned, RETENTION_DAYS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
