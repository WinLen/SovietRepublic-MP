#!/usr/bin/env python3
"""Unprivileged HTTP crash/desync ZIP collector for RepublicMP.

Receives compressed ZIP bundles from RepublicMP clients on 127.0.0.1:8095
behind nginx, validates key, size, rate limit, and ZIP integrity, deduplicates
by SHA-256, and atomically writes reports into /var/republicmp/spool/ready/.
This process has zero access to Google Drive tokens or rclone configs.
"""

import collections
import datetime
import hashlib
import hmac
import http.server
import io
import json
import os
import re
import shutil
import socketserver
import threading
import time
import zipfile

BIND_HOST = os.environ.get("BIND_HOST", "127.0.0.1")
BIND_PORT = int(os.environ.get("BIND_PORT", "8095"))
SPOOL_ROOT = os.environ.get("SPOOL_ROOT", "/var/republicmp/spool")
INCOMING_DIR = os.path.join(SPOOL_ROOT, ".incoming")
READY_DIR = os.path.join(SPOOL_ROOT, "ready")
INGEST_KEY = os.environ.get("REPUBLICMP_INGEST_KEY", "")

MAX_UPLOAD_BYTES = int(os.environ.get("MAX_UPLOAD_BYTES", str(2 * 1024 * 1024)))
MAX_UNZIPPED_BYTES = int(os.environ.get("MAX_UNZIPPED_BYTES", str(10 * 1024 * 1024)))
MAX_ZIP_FILES = int(os.environ.get("MAX_ZIP_FILES", "16"))
MAX_SPOOL_BYTES = int(os.environ.get("MAX_SPOOL_BYTES", str(2 * 1024 * 1024 * 1024)))
MIN_FREE_BYTES = int(os.environ.get("MIN_FREE_BYTES", str(1024 * 1024 * 1024)))
RATE_LIMIT_WINDOW_SEC = int(os.environ.get("RATE_LIMIT_WINDOW_SEC", "600"))
RATE_LIMIT_MAX_REQ = int(os.environ.get("RATE_LIMIT_MAX_REQ", "10"))

ALLOWED_REASONS = {"CRASH", "PREV_CRASH", "DESYNC", "MANUAL", "TEST_SMOKE", "OTHER"}
ALLOWED_ROLES = {"HOST", "CLIENT", "MENU", "UNKNOWN"}
ALLOWED_EXTS = {".log", ".txt", ".json", ".ini", ".dmp"}
VERSION_RE = re.compile(r"^[0-9A-Za-z._-]{1,24}$")
SHA12_SUFFIX_RE = re.compile(r"_([0-9a-f]{12})\.zip$")

_rate_lock = threading.Lock()
_rate_buckets = collections.defaultdict(collections.deque)

_dedup_lock = threading.Lock()
_known_sha12 = {}  # sha12 -> relative path under READY_DIR
_spool_bytes_estimate = 0


def _init_spool_state():
    global _spool_bytes_estimate
    os.makedirs(INCOMING_DIR, exist_ok=True)
    os.makedirs(READY_DIR, exist_ok=True)
    total = 0
    known = {}
    for root, _dirs, files in os.walk(READY_DIR):
        for name in files:
            full = os.path.join(root, name)
            try:
                total += os.path.getsize(full)
            except OSError:
                continue
            m = SHA12_SUFFIX_RE.search(name)
            if m:
                rel = os.path.relpath(full, READY_DIR).replace("\\", "/")
                known[m.group(1)] = rel
    with _dedup_lock:
        _known_sha12.update(known)
        _spool_bytes_estimate = total


def _check_rate_limit(client_ip):
    now = time.monotonic()
    cutoff = now - RATE_LIMIT_WINDOW_SEC
    with _rate_lock:
        dq = _rate_buckets[client_ip]
        while dq and dq[0] < cutoff:
            dq.popleft()
        if len(dq) >= RATE_LIMIT_MAX_REQ:
            return False
        dq.append(now)
        if len(_rate_buckets) > 4096:
            stale = [ip for ip, q in _rate_buckets.items() if not q or q[-1] < cutoff]
            for ip in stale:
                del _rate_buckets[ip]
    return True


def _validate_zip_payload(data):
    try:
        zf = zipfile.ZipFile(io.BytesIO(data), "r")
    except (zipfile.BadZipFile, OSError, ValueError):
        return False, "invalid zip header"

    with zf:
        try:
            bad_member = zf.testzip()
        except Exception:
            return False, "zip crc check failed"
        if bad_member is not None:
            return False, "corrupt zip entry"

        infos = zf.infolist()
        if not infos or len(infos) > MAX_ZIP_FILES:
            return False, "invalid zip entry count"

        total_uncompressed = 0
        file_count = 0
        for info in infos:
            name = info.filename or ""
            if name.endswith("/"):
                continue
            file_count += 1
            if not name or name.startswith("/") or name.startswith("\\"):
                return False, "unsafe absolute zip path"
            if ":" in name or "\\" in name:
                return False, "unsafe path characters"
            parts = name.split("/")
            if any(p in ("", ".", "..") for p in parts):
                return False, "zip path traversal"
            if any(ord(ch) < 32 or ord(ch) == 127 for ch in name):
                return False, "control chars in zip entry"
            _stem, ext = os.path.splitext(name)
            if ext.lower() not in ALLOWED_EXTS:
                return False, "disallowed file extension"
            if info.file_size < 0 or info.file_size > MAX_UNZIPPED_BYTES:
                return False, "zip entry too large"
            total_uncompressed += info.file_size
            if total_uncompressed > MAX_UNZIPPED_BYTES:
                return False, "zip bomb limit exceeded"

        if file_count == 0:
            return False, "empty zip archive"
    return True, ""


def _sanitize_header_token(val, allowed, default):
    clean = (val or "").strip().upper()
    return clean if clean in allowed else default


def _sanitize_version(val):
    clean = (val or "").strip()
    if VERSION_RE.match(clean):
        return clean
    return "unknown"


class CollectorHandler(http.server.BaseHTTPRequestHandler):
    server_version = "RepublicMPCollector/1.0"
    sys_version = ""

    def log_message(self, fmt, *args):
        # Suppress default Apache-style logs so sensitive headers are never reflected.
        return

    def _send_json(self, status, obj):
        payload = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _client_ip(self):
        xff = self.headers.get("X-Real-IP") or ""
        if xff:
            return xff.split(",")[0].strip()[:64]
        return str(self.client_address[0])[:64]

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/healthz", "/republicmp-logs/healthz"):
            ok = os.path.isdir(READY_DIR) and os.access(READY_DIR, os.W_OK)
            if ok:
                self._send_json(200, {"status": "ok"})
            else:
                self._send_json(503, {"status": "spool_unavailable"})
            return
        self._send_json(404, {"error": "not_found"})

    def do_POST(self):
        global _spool_bytes_estimate
        path = self.path.split("?", 1)[0]
        if path not in ("/v1/upload", "/republicmp-logs/v1/upload"):
            self._send_json(404, {"error": "not_found"})
            return

        if not INGEST_KEY:
            self._send_json(503, {"error": "collector_unconfigured"})
            return

        provided_key = self.headers.get("X-RepublicMP-Key", "")
        if not hmac.compare_digest(provided_key.encode("utf-8"), INGEST_KEY.encode("utf-8")):
            self._send_json(403, {"error": "forbidden"})
            return

        ip = self._client_ip()
        if not _check_rate_limit(ip):
            self._send_json(429, {"error": "rate_limited"})
            return

        raw_len = self.headers.get("Content-Length")
        if raw_len is None:
            self._send_json(411, {"error": "length_required"})
            return
        try:
            content_len = int(raw_len)
        except ValueError:
            self._send_json(400, {"error": "invalid_content_length"})
            return
        if content_len <= 0:
            self._send_json(400, {"error": "empty_body"})
            return
        if content_len > MAX_UPLOAD_BYTES:
            self._send_json(413, {"error": "payload_too_large"})
            return

        chunks = []
        remaining = content_len
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 65536))
            if not chunk:
                self._send_json(400, {"error": "truncated_body"})
                return
            chunks.append(chunk)
            remaining -= len(chunk)
        body = b"".join(chunks)

        usage = shutil.disk_usage(SPOOL_ROOT)
        if usage.free < MIN_FREE_BYTES:
            self._send_json(507, {"error": "disk_low"})
            return
        with _dedup_lock:
            if _spool_bytes_estimate + len(body) > MAX_SPOOL_BYTES:
                self._send_json(507, {"error": "spool_quota_exceeded"})
                return

        valid, err_reason = _validate_zip_payload(body)
        if not valid:
            self._send_json(400, {"error": err_reason})
            return

        reason = _sanitize_header_token(self.headers.get("X-RepublicMP-Reason"), ALLOWED_REASONS, "OTHER")
        role = _sanitize_header_token(self.headers.get("X-RepublicMP-Role"), ALLOWED_ROLES, "UNKNOWN")
        version = _sanitize_version(self.headers.get("X-RepublicMP-Version"))

        sha12 = hashlib.sha256(body).hexdigest()[:12]
        with _dedup_lock:
            existing_rel = _known_sha12.get(sha12)
            if existing_rel and os.path.isfile(os.path.join(READY_DIR, existing_rel)):
                print("duplicate id=%s reason=%s version=%s role=%s bytes=%d" % (
                    sha12, reason, version, role, len(body)), flush=True)
                self._send_json(200, {"status": "duplicate", "id": sha12, "path": existing_rel})
                return

        now_utc = datetime.datetime.utcnow()
        day_dir = now_utc.strftime("%Y-%m-%d")
        time_tag = now_utc.strftime("%H%M%S")
        rel_name = "%s/%s_%s_v%s_%s_%s.zip" % (day_dir, time_tag, reason, version, role, sha12)
        dest_dir = os.path.join(READY_DIR, day_dir)
        dest_path = os.path.join(READY_DIR, rel_name)
        tmp_name = "%s_%d_%d_%s.tmp" % (time_tag, os.getpid(), threading.get_ident(), sha12)
        tmp_path = os.path.join(INCOMING_DIR, tmp_name)

        try:
            os.makedirs(dest_dir, mode=0o750, exist_ok=True)
            with open(tmp_path, "wb") as fh:
                fh.write(body)
                fh.flush()
                os.fsync(fh.fileno())
            os.chmod(tmp_path, 0o640)
            os.replace(tmp_path, dest_path)
        except OSError as exc:
            try:
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)
            except OSError:
                pass
            print("write_error id=%s err=%s" % (sha12, type(exc).__name__), flush=True)
            self._send_json(500, {"error": "spool_write_failed"})
            return

        with _dedup_lock:
            _known_sha12[sha12] = rel_name
            _spool_bytes_estimate += len(body)

        print("accepted id=%s reason=%s version=%s role=%s bytes=%d" % (
            sha12, reason, version, role, len(body)), flush=True)
        self._send_json(200, {"status": "accepted", "id": sha12, "path": rel_name})


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    if not INGEST_KEY:
        raise SystemExit("REPUBLICMP_INGEST_KEY must be set in environment")
    _init_spool_state()
    srv = ThreadedHTTPServer((BIND_HOST, BIND_PORT), CollectorHandler)
    print("RepublicMP collector listening on %s:%d (spool=%s)" % (BIND_HOST, BIND_PORT, SPOOL_ROOT), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
