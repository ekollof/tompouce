import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

// Exercise the browser scheduling contract with a deliberately blocked slow
// status request. Fast telemetry and animation must continue independently.
let now = 100;
const requests = [], timers = [], frames = [], rectangles = [];
const context2d = Object.fromEntries(['setTransform', 'clearRect', 'beginPath', 'moveTo', 'lineTo', 'stroke', 'fillText'].map(name => [name, () => {}]));
context2d.fillRect = (...args) => rectangles.push(args);
const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {value: '', textContent: '', addEventListener() {},
    width: 0, height: 0, clientWidth: 800, getContext: () => context2d});
  return elements.get(id);
}
const document = {hidden: false, activeElement: null, getElementById: element,
  querySelector: () => ({content: 'test-token'})};
const sandbox = vm.createContext({document, devicePixelRatio: 1, performance: {now: () => now},
  requestAnimationFrame: callback => frames.push(callback),
  setInterval() {}, setTimeout: (callback, delay) => timers.push({callback, delay}),
  fetch: (url, options) => new Promise(resolve => requests.push({body: JSON.parse(options.body), resolve})),
  confirm: () => true});
vm.runInContext(fs.readFileSync('web/companion.js', 'utf8'), sandbox);
assert.deepEqual(requests.map(request => request.body.name), ['music_get_status', 'music_debug']);
assert.equal(requests[1].body.arguments.limit, 32);
const debug = 't=0 playing=yes pool=1/128\nv0 tok=piano freq=440 gain=.5 age=0 gate=1 rel=.1 pos=100 len=10000 step=1';
requests[1].resolve({ok: true, json: async () => ({result: {content: [{text: debug}]}})});
await new Promise(resolve => setImmediate(resolve));
assert.equal(timers.length, 1);
assert.ok(timers[0].delay >= 30 && timers[0].delay <= 34);
assert.equal(requests.length, 2); // No overlapping voice requests.
now = 150;
frames.shift()(now);
assert.equal(rectangles.length, 2);
assert.ok(Math.abs(rectangles[1][2] - 80 * (100 + .05 * 44100) / 10000) < 1e-9);
const width = element('voices').width;
rectangles.length = 0;
now = 400;
frames.shift()(now);
assert.equal(rectangles.length, 0); // Stale snapshots must not freeze voices.
assert.equal(element('voices').width, width);
document.hidden = true;
await timers.shift().callback();
assert.equal(requests.length, 2);
assert.equal(timers.at(-1).delay, 250);
sandbox.source('original source');
element('code').value = 'pasted edits';
sandbox.setDirty(true);
sandbox.source('terminal changed while source request was in flight');
assert.equal(element('code').value, 'pasted edits');
assert.equal(vm.runInContext('currentSource', sandbox), 'original source');
async function reply(text,isError=false) {
  requests.at(-1).resolve({ok:true,json:async()=>({result:{isError,content:[{text}]}})});
  await new Promise(resolve=>setImmediate(resolve));
}
let apply=element('apply').onclick({target:element('apply')});
assert.equal(requests.at(-1).body.name,'music_get_code');
await reply('original source');
assert.equal(requests.at(-1).body.name,'music_set_code');
assert.equal(requests.at(-1).body.arguments.code,'pasted edits');
assert.equal(requests.at(-1).body.arguments.autostart,true);
await reply('Loaded 1 tracks.'); await apply;
assert.equal(element('dirty').textContent,'Following session');
// Telemetry may still say playing during a quick Stop/Play. Activation must
// submit current source regardless of that stale display snapshot.
element('code').value='pending Play edits'; sandbox.setDirty(true);
apply=element('play').onclick({target:element('play')});
await reply('pasted edits');
assert.equal(requests.at(-1).body.arguments.code,'pending Play edits');
assert.equal(requests.at(-1).body.arguments.autostart,true);
await reply('Loaded 1 tracks.'); await apply;
element('code').value='failed edit'; sandbox.setDirty(true);
apply=element('evaluate').onclick({target:element('evaluate')});
await reply('pending Play edits');
assert.equal(requests.at(-1).body.arguments.code,'failed edit');
await reply('Evaluation failed; last playable pattern retained.',true); await apply;
assert.match(element('error').textContent,/Evaluation failed/);
assert.equal(element('code').value,'failed edit');
assert.equal(element('dirty').textContent,'Unapplied edits');
assert.equal(element('evaluate').disabled,false);
console.log('companion fast polling, independent animation, interpolation and stale/hidden handling ok');
