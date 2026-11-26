#!/usr/bin/env bash
set -euo pipefail
umask 077

SPOOL_READY="${REPUBLICMP_SPOOL_READY:-/var/republicmp/spool/ready}"
GDRIVE_TARGET="${REPUBLICMP_GDRIVE_TARGET:-gdrive:republicmp-logs}"
RCLONE_BIN="${RCLONE_BIN:-/usr/local/bin/rclone}"
RCLONE_CONFIG_PATH="${RCLONE_CONFIG:-/root/.config/rclone/rclone.conf}"
LOCAL_RETENTION_DAYS="${REPUBLICMP_LOCAL_RETENTION_DAYS:-14}"

# Hard guardrail: never write outside gdrive:republicmp-logs
case "$GDRIVE_TARGET" in
  gdrive:republicmp-logs|gdrive:republicmp-logs/*) ;;
  *)
    echo "Refusing unsafe GDRIVE_TARGET: $GDRIVE_TARGET" >&2
    exit 2
    ;;
esac

if [ ! -d "$SPOOL_READY" ]; then
  exit 0
fi

mkdir -p /run/lock
exec 9>/run/lock/republicmp-gdrive-sync.lock
if ! flock -n 9; then
  exit 0
fi

# Fast exit when spool has no ready files
if [ -z "$(find "$SPOOL_READY" -type f -print -quit 2>/dev/null)" ]; then
  exit 0
fi

# One-way rclone copy only (never sync/delete/purge/move); isolated env with host rclone.conf
env -i \
  HOME=/root \
  PATH=/usr/local/bin:/usr/bin:/bin \
  RCLONE_CONFIG="$RCLONE_CONFIG_PATH" \
  "$RCLONE_BIN" copy "$SPOOL_READY" "$GDRIVE_TARGET" \
    --min-age 10s \
    --transfers 2 \
    --checkers 4 \
    --retries 3 \
    --low-level-retries 5 \
    --log-level NOTICE

# Local-only retention: remove local files >14 days old ONLY after rclone confirms exact match on Drive
env -i \
  HOME=/root \
  PATH=/usr/local/bin:/usr/bin:/bin \
  RCLONE_CONFIG="$RCLONE_CONFIG_PATH" \
  REPUBLICMP_SPOOL_READY="$SPOOL_READY" \
  REPUBLICMP_GDRIVE_TARGET="$GDRIVE_TARGET" \
  RCLONE_BIN="$RCLONE_BIN" \
  REPUBLICMP_LOCAL_RETENTION_DAYS="$LOCAL_RETENTION_DAYS" \
  /usr/bin/python3 /opt/republicmp-collector/prune_local_spool.py
