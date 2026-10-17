#!/usr/bin/env bash
set -euo pipefail
HOST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$HOST_DIR")"
PYTHON="$PROJECT_ROOT/.venv/bin/python"

if [[ ! -x "$PYTHON" ]]; then
    echo "Create the environment first: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt" >&2
    exit 1
fi

cd "$HOST_DIR"

"$PYTHON" spectrum_transmitter.py &
audio_pid=$!
"$PYTHON" video_transmitter.py "$@" &
video_pid=$!

cleanup() {
    kill "$audio_pid" "$video_pid" 2>/dev/null || true
    wait "$audio_pid" "$video_pid" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
wait -n "$audio_pid" "$video_pid"
