#!/bin/bash
# Generic hook -> forwards the event JSON to the bridge to drive status/HUD/alerts.
# Fire-and-forget: never blocks Claude Code. $1 = event kind.
# Carries this session's identity: the body already has session_id+cwd (from Claude);
# the headers add the launcher's tmux target + project title so the bridge can route to it.
PORT="${CCDECK_PORT:-8787}"
body="$(cat)"
curl -s -m 1 -X POST "http://127.0.0.1:${PORT}/event?kind=$1" \
  -H 'content-type: application/json' \
  -H "x-claudeq-tmux: ${CLAUDEQ_TMUX:-}" \
  -H "x-claudeq-title: ${CLAUDEQ_TITLE:-}" \
  --data-binary "$body" >/dev/null 2>&1 &
exit 0
