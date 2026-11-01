#!/bin/sh

PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOG_DIR=${LOGS_PATH:-$PAK_DIR}

mkdir -p "$LOG_DIR"
cd "$PAK_DIR" || exit 1

exec >>"$LOG_DIR/ChooChooTracker.txt" 2>&1
exec ./choochootracker.aarch64
