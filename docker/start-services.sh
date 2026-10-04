#!/usr/bin/env bash
# Start the state-sync backend (and optionally the sync agent).
set -euo pipefail
export PYTHONPATH="/app/sync:${PYTHONPATH:-}"
SYNC_DB="${SYNC_DB:-/app/data/state-sync.db}"
SYNC_HOST="${SYNC_HOST:-127.0.0.1}"
SYNC_PORT="${SYNC_PORT:-8080}"
mkdir -p "$(dirname "$SYNC_DB")"

python3 /app/sync/synckit/server/app.py \
  --db "$SYNC_DB" --host "$SYNC_HOST" --port "$SYNC_PORT" &
SERVER_PID=$!

# Run a terminal agent for the local C client when requested (off by default;
# the C client also works fully offline with local recovery).
if [[ "${RUN_AGENT:-0}" == "1" ]]; then
  export VW_STATE_DIR="${VW_STATE_DIR:-$HOME/.local/state/visual-window}"
  export VW_SYNC_URL="http://${SYNC_HOST}:${SYNC_PORT}"
  python3 /app/sync/agent/agent.py &
  AGENT_PID=$!
fi

wait "$SERVER_PID"
