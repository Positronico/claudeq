#!/usr/bin/env node
// PreToolUse hook for AskUserQuestion.
// Long-polls the bridge for the device's choice; if it answers, returns updatedInput.answers
// so Claude Code short-circuits the interactive picker. If the bridge/device is unavailable
// or times out, emits nothing -> Claude Code falls back to its normal TUI picker.
const PORT = process.env.CCDECK_PORT || '8787';
let input = '';
process.stdin.on('data', (d) => (input += d));
process.stdin.on('end', async () => {
  let data; try { data = JSON.parse(input); } catch { process.exit(0); }
  const ti = data.tool_input || {};
  const id = data.tool_use_id || 'ask-' + Date.now();
  if (!ti.questions || !ti.questions.length) process.exit(0);
  // Identify which session is asking so the bridge can label + route it on the deck.
  const session = {
    sid: data.session_id || process.env.CLAUDEQ_TMUX || null,
    tmux: process.env.CLAUDEQ_TMUX || null,
    title: process.env.CLAUDEQ_TITLE || null,
    cwd: data.cwd || null,
  };
  const ctrl = new AbortController();
  const to = setTimeout(() => ctrl.abort(), 295000);
  try {
    const res = await fetch(`http://127.0.0.1:${PORT}/ask`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ id, tool_input: ti, session }), signal: ctrl.signal,
    });
    const j = await res.json();
    if (j && j.answers) {
      process.stdout.write(JSON.stringify({
        hookSpecificOutput: {
          hookEventName: 'PreToolUse',
          permissionDecision: 'allow',
          updatedInput: { questions: ti.questions, answers: j.answers },
        },
      }));
    }
  } catch { /* bridge down / aborted -> fall through to normal picker */ }
  clearTimeout(to);
  process.exit(0);
});
