const $ = id => document.getElementById(id);
const token = document.querySelector('meta[name="companion-token"]').content;
let dirty = false, currentSource = '', inFlight = false;
let voiceSnapshot = [], snapshotAt = -Infinity, voicesPlaying = false;
let sourceEditor;
async function tool(name, args = {}) {
  const response = await fetch('/api/tool', {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Companion-Token': token}, body: JSON.stringify({name, arguments: args})});
  const result = await response.json();
  if (!response.ok || result.error || result.result?.isError) throw new Error(result.error?.message ?? result.error ?? result.result?.content?.[0]?.text ?? 'Request failed');
  return result.result.content.map(item => item.text ?? '').join('\n');
}
function setDirty(value) { dirty = value; $('dirty').textContent = value ? 'Unapplied edits' : 'Following session'; }
function source(value) {
  if (dirty) return; // An in-flight poll must not change an editing baseline.
  currentSource = value;
  if (sourceEditor) sourceEditor.setValue(value);
  else if ($('code').value !== value) $('code').value = value;
}
async function action(button, name, args = {}) {
  button.disabled = true; $('error').textContent = '';
  try { await tool(name, args); await refresh(); }
  catch (error) { $('error').textContent = error.message; }
  finally { button.disabled = false; }
}
$('code').addEventListener('input', () => setDirty($('code').value !== currentSource));
if (globalThis.TompouceEditor) sourceEditor = TompouceEditor.attach($('code'), document.querySelector('meta[name="editor-style-nonce"]').content);
globalThis.TompouceCompanion = { tool, insertSnippet(text) {
  if (sourceEditor) sourceEditor.insertText(text);
  else {
    const code = $('code');
    code.setRangeText(text, code.selectionStart, code.selectionEnd, 'end');
    code.dispatchEvent(new Event('input', {bubbles: true})); code.focus();
  }
} };
$('play').onclick = event => action(event.target, 'music_play');
$('stop').onclick = event => action(event.target, 'music_stop');
$('evaluate').onclick = event => action(event.target, 'music_evaluate');
$('tempo').onclick = event => {
  const bpm = Number($('bpm').value);
  if (!Number.isFinite(bpm) || bpm < 20 || bpm > 400) { $('error').textContent = 'BPM must be between 20 and 400.'; return; }
  action(event.target, 'music_set_bpm', {bpm});
};
$('apply').onclick = async event => {
  const button = event.target, code = $('code').value;
  button.disabled = true;
  try {
    // Detect edits made in the terminal since this tab last followed it.
    const remote = await tool('music_get_code');
    if (remote !== currentSource && !confirm('The terminal source changed. Replace it with this tab’s edits?')) return;
    await tool('music_set_code', {code});
    currentSource = code; setDirty($('code').value !== code); $('error').textContent = '';
  } catch (error) { $('error').textContent = error.message; }
  finally { button.disabled = false; }
};
$('reload').onclick = async () => {
  if (dirty && !confirm('Discard this tab’s unapplied edits?')) return;
  try { const code = await tool('music_get_code'); setDirty(false); source(code); }
  catch (error) { $('error').textContent = error.message; }
};
function acceptVoices(debug, now = performance.now()) {
  voicesPlaying = /\bplaying=yes\b/.test(debug.split('\n')[0]);
  voiceSnapshot = debug.split('\n').filter(line => /^v\d+ /.test(line)).map(row =>
    Object.fromEntries([...row.matchAll(/(\w+)=([^ ]+)/g)].map(match => [match[1], match[2]])));
  snapshotAt = now;
}
function draw(now) {
  const canvas = $('voices'), ratio = devicePixelRatio || 1;
  const pixelWidth = Math.round(canvas.clientWidth * ratio), pixelHeight = 240 * ratio;
  if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
    canvas.width = pixelWidth; canvas.height = pixelHeight;
  }
  const context = canvas.getContext('2d'); context.setTransform(ratio, 0, 0, ratio, 0, 0);
  const width = canvas.clientWidth;
  context.clearRect(0, 0, width, 240); context.font = '11px system-ui';
  for (let midi = 24; midi <= 108; midi += 12) {
    const x = (midi - 24) / 84 * width;
    context.strokeStyle = '#30433c'; context.beginPath(); context.moveTo(x, 0); context.lineTo(x, 220); context.stroke();
    context.fillStyle = '#9aaea5'; context.fillText(`C${midi / 12 - 1}`, x + 4, 235);
  }
  const elapsed = voicesPlaying ? Math.max(0, (now - snapshotAt) / 1000) : 0;
  const rows = now - snapshotAt <= 250 ? voiceSnapshot : [];
  rows.forEach((value, index) => {
    if (voicesPlaying && Number(value.age) + elapsed >= Number(value.gate) + Number(value.rel)) return;
    const midi = Number(value.freq) > 0 ? 69 + 12 * Math.log2(Number(value.freq) / 440) : 24 + index * 3;
    const x = Math.max(0, Math.min(width - 70, (midi - 24) / 84 * width));
    const y = 8 + index % 8 * 26;
    context.fillStyle = '#85d6b5'; context.globalAlpha = Math.max(.3, Math.min(1, Number(value.gain) || .3));
    context.fillRect(x, y, Math.max(12, Math.min(100, (Number(value.gain) || .3) * 80)), 6);
    if (Number(value.len) > 0) {
      context.fillStyle = '#648cad';
      const position = Number(value.pos) + elapsed * 44100 * Number(value.step);
      context.fillRect(x, y + 7, 80 * Math.max(0, Math.min(1, position / Number(value.len))), 2);
    }
    context.globalAlpha = 1; context.fillStyle = '#e4eeea'; context.fillText(value.tok ?? 'voice', x, y + 19);
  });
  requestAnimationFrame(draw);
}
async function pollVoices() {
  const started = performance.now();
  try {
    if (!document.hidden) acceptVoices(await tool('music_debug', {limit: 32}));
  } catch (error) {
    voiceSnapshot = [];
    $('connection').textContent = 'Disconnected'; $('error').textContent = error.message;
  } finally {
    // One request at a time; slow engines cannot accumulate stale frames.
    setTimeout(pollVoices, document.hidden ? 250 : Math.max(0, 1000 / 30 - (performance.now() - started)));
  }
}
async function refresh() {
  if (inFlight) return;
  inFlight = true;
  try {
    // The engine accepts one MCP connection at a time; keep these sequential.
    const status = await tool('music_get_status'); $('status').textContent = status.split('\n')[0];
    const bpm = status.match(/bpm=([\d.]+)/)?.[1];
    if (bpm && document.activeElement !== $('bpm')) $('bpm').value = bpm;
    $('console').textContent = await tool('music_get_console', {lines: 16});
    $('health').textContent = await tool('music_health');
    if (!dirty) source(await tool('music_get_code'));
    $('connection').textContent = 'Connected · native audio';
  } catch (error) { $('connection').textContent = 'Disconnected'; $('error').textContent = error.message; }
  finally { inFlight = false; }
}
refresh(); setInterval(refresh, 1000);
pollVoices(); requestAnimationFrame(draw);
