#!/usr/bin/env bash
set -euo pipefail

DISPLAY_NUM=":99"
SCREEN_GEOMETRY="1280x720x24"
VNC_PORT="5900"
NOVNC_PORT="6080"

cleanup() {
  local code=$?
  for pid in "${APP_PID:-}" "${AGENT_SYNC_PID:-}" "${SERVER_PID:-}" "${NOVNC_PID:-}" "${VNC_PID:-}" "${WM_PID:-}" "${XVFB_PID:-}"; do
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
  exit "${code}"
}
trap cleanup EXIT INT TERM

Xvfb "${DISPLAY_NUM}" -screen 0 "${SCREEN_GEOMETRY}" -ac +extension GLX +render -noreset &
XVFB_PID=$!

export DISPLAY="${DISPLAY_NUM}"

for _ in $(seq 1 50); do
  if xdpyinfo >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

openbox >/tmp/openbox.log 2>&1 &
WM_PID=$!

x11vnc \
  -display "${DISPLAY}" \
  -forever \
  -shared \
  -rfbport "${VNC_PORT}" \
  -localhost \
  -nopw \
  -noxdamage \
  >/tmp/x11vnc.log 2>&1 &
VNC_PID=$!

if [[ -x /usr/share/novnc/utils/novnc_proxy ]]; then
  cat >/usr/share/novnc/index.html <<'HTML'
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta http-equiv="refresh" content="0; url=/vnc.html?autoconnect=1&resize=scale&path=websockify" />
    <title>noVNC Redirect</title>
  </head>
  <body>
    <p>Redirecting to noVNC...</p>
  </body>
</html>
HTML
  /usr/share/novnc/utils/novnc_proxy --listen "${NOVNC_PORT}" --vnc "localhost:${VNC_PORT}" >/tmp/novnc.log 2>&1 &
else
  websockify --web=/usr/share/novnc/ "${NOVNC_PORT}" "localhost:${VNC_PORT}" >/tmp/novnc.log 2>&1 &
fi
NOVNC_PID=$!

# Optional state-sync backend + terminal agent for the local container.
# Pure local recovery works regardless; set SYNC_SERVER=1 to enable.
if [[ "${SYNC_SERVER:-0}" == "1" ]]; then
  export PYTHONPATH="/app/sync:${PYTHONPATH:-}"
  mkdir -p /app/data
  python3 /app/sync/synckit/server/app.py --db /app/data/state-sync.db \
    --host 127.0.0.1 --port 8080 >/tmp/sync-server.log 2>&1 &
  SERVER_PID=$!
  export VW_SYNC_URL="http://127.0.0.1:8080"
  for _ in $(seq 1 30); do
    if curl -sf http://127.0.0.1:8080/api/health >/dev/null 2>&1 \
       || wget -qO- http://127.0.0.1:8080/api/health >/dev/null 2>&1; then
      break
    fi
    sleep 0.2
  done
  python3 /app/sync/agent/agent.py >/tmp/sync-agent.log 2>&1 &
  AGENT_SYNC_PID=$!
fi

./visual-window-app &
APP_PID=$!

wait "${APP_PID}"
