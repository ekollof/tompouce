/* Per-target duck control mapping through real Superdough, owned silent contexts.
   The timing gate covers scalar coercion end to end; this gate covers which
   array element (or scalar) reaches each ducked orbit: multi-orbit targets,
   repeated/invalid/nested/string targets, and per-index onset/attack/depth
   arrays with short, long, empty and null entries. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

async function capture(cases) {
  const enc = (value) => {
    if (value === undefined) return {u: 1};
    if (typeof value === 'number') {
      if (isNaN(value)) return {nan: 1};
      if (value === 0) return {n: '0000000000000000'};
      const bits = new DataView(new ArrayBuffer(8));
      bits.setFloat64(0, value);
      let hex = '';
      for (let i = 0; i < 8; i++) hex += bits.getUint8(i).toString(16).padStart(2, '0');
      return {n: hex};
    }
    if (typeof value === 'string') return {s: value};
    if (typeof value === 'boolean') return {b: value};
    if (value === null) return {z: 1};
    return {o: JSON.stringify(value)};
  };
  const results = [];
  for (const test of cases) {
    const context = new OfflineAudioContext(2, 44100, 44100);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony: 4, multiChannelOrbits: false});
    const controller = getSuperdoughAudioController();
    for (const id of [1, 2, 3]) {
      const orbit = controller.getOrbit(id, [1, 2]);
      orbit.output.gain.value = .8;
    }
    const seen = [];
    for (const id of [1, 2, 3]) {
      const orbit = controller.getOrbit(id, [1, 2]);
      const duck = orbit.duck.bind(orbit);
      orbit.duck = (t, onset, attack, depth) => {
        seen.push({orbit: id, onset: enc(onset), attack: enc(attack), depth: enc(depth)});
        return duck(t, onset, attack, depth);
      };
    }
    await superdough(test.value, .1, .3, .5, 0);
    await context.startRendering();
    results.push({name: test.name, seen});
  }
  return results;
}

// Target order in the matrix stays ascending so the native mask dump and the
// web forEach order compare directly; repeats keep their positions.
const base = {s: 'sine', note: 60, gain: .1, orbit: 7};
const withTargets = (targets, extra = {}) => ({...base, duckorbit: targets, ...extra});
const cases = [];
const add = (name, value) => cases.push({name, value});
// Target selection shapes (scalar depth for isolation).
add('two targets', withTargets([1, 2], {duckdepth: .25}));
add('three targets', withTargets([1, 2, 3], {duckdepth: .25}));
add('repeated targets', withTargets([2, 2], {duckdepth: .25}));
add('scalar target', withTargets(2, {duckdepth: .25}));
add('string target', withTargets('2', {duckdepth: .25}));
add('nested singleton target', withTargets([[2]], {duckdepth: .25}));
add('mixed valid/invalid targets', withTargets([2, 'x'], {duckdepth: .25}));
add('invalid middle target', withTargets([1, 'x', 2], {duckdepth: .25}));
add('empty targets', withTargets([], {duckdepth: .25}));
add('null target', withTargets([null], {duckdepth: .25}));
add('float target', withTargets(2.5, {duckdepth: .25}));
add('negative target', withTargets(-1, {duckdepth: .25}));
add('boolean target', withTargets(true, {duckdepth: .25}));
add('leading-zero target', withTargets('02', {duckdepth: .25}));
add('nested second target', withTargets([1, [2]], {duckdepth: .25}));
add('colon target', withTargets('1:2', {duckdepth: .25}));
add('nested multi target', withTargets([[1, 2]], {duckdepth: .25}));
add('string targets', withTargets(['1', '2'], {duckdepth: .25}));
add('missing targets', {...base, duckdepth: .25});
add('default controls', withTargets([1, 2]));
add('missing depth', withTargets([1, 2], {duckattack: .3}));
add('missing attack', withTargets([1, 2], {duckdepth: .5}));
add('missing onset', withTargets([1, 2], {duckdepth: .5, duckattack: .3}));
// Depth element selection across two targets.
add('scalar depth', withTargets([1, 2], {duckdepth: .5}));
add('depth pair', withTargets([1, 2], {duckdepth: [.2, .8]}));
add('short depth', withTargets([1, 2], {duckdepth: [.2]}));
add('long depth', withTargets([1, 2], {duckdepth: [.2, .8, .9]}));
add('empty depth', withTargets([1, 2], {duckdepth: []}));
add('null depth entry', withTargets([1, 2], {duckdepth: [null]}));
add('depth null tail', withTargets([1, 2], {duckdepth: [.2, null]}));
add('depth null head', withTargets([1, 2], {duckdepth: [null, .8]}));
add('zero depth', withTargets([1, 2], {duckdepth: 0}));
add('null depth', withTargets([1, 2], {duckdepth: null}));
add('third depth fallback', withTargets([1, 2, 3], {duckdepth: [.2, .8]}));
add('repeated depth entries', withTargets([2, 2], {duckdepth: [.2, .8]}));
add('scalar target array depth', withTargets(2, {duckdepth: [.2, .8]}));
add('skipped target depth', withTargets([2, 'x'], {duckdepth: [.2, .8]}));
add('invalid head depth', withTargets(['x', 2], {duckdepth: [.2, .8]}));
add('nested target depth', withTargets([1, [2]], {duckdepth: [.2, .8]}));
add('bool depth entries', withTargets([1, 2], {duckdepth: [true, false]}));
add('string depth entries', withTargets([1, 2], {duckdepth: ['0.5', 'bad']}));
// Attack element selection across two targets.
add('scalar attack', withTargets([1, 2], {duckattack: .3}));
add('attack pair', withTargets([1, 2], {duckattack: [.3, .5]}));
add('short attack', withTargets([1, 2], {duckattack: [.3]}));
add('empty attack', withTargets([1, 2], {duckattack: []}));
add('null attack entry', withTargets([1, 2], {duckattack: [null]}));
add('string attack entries', withTargets([1, 2], {duckattack: ['bad', .3]}));
add('null attack', withTargets([1, 2], {duckattack: null}));
add('string attack', withTargets([1, 2], {duckattack: 'bad'}));
add('bool attack entry', withTargets([1, 2], {duckattack: [true]}));
// Onset element selection across two targets.
add('scalar onset', withTargets([1, 2], {duckonset: .05}));
add('onset pair', withTargets([1, 2], {duckonset: [.05, .09]}));
add('short onset', withTargets([1, 2], {duckonset: [.05]}));
add('empty onset', withTargets([1, 2], {duckonset: []}));
add('null onset entry', withTargets([1, 2], {duckonset: [null]}));
add('string onset entries', withTargets([1, 2], {duckonset: ['0.05', .09]}));
add('bool onset entry', withTargets([1, 2], {duckonset: [true]}));
add('null onset', withTargets([1, 2], {duckonset: null}));
add('string onset', withTargets([1, 2], {duckonset: '0.05'}));

const browser = await BrowserClient.create();
let results;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function"', 20000);
  results = await browser.evaluate('(' + capture.toString() + ')(' + JSON.stringify(cases) + ')');
} finally { await browser.close(); }
const artifacts = process.argv[2] ?? '/tmp/tompouce-duck-mapping-reference';
await fs.mkdir(artifacts, {recursive: true});
await fs.writeFile(path.join(artifacts, 'web.json'), JSON.stringify(results, null, 2) + '\n');

const native = spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY ?? './build/tests/test_live_sliders',
  ['--duck-mapping-reference'], {input: JSON.stringify(cases), encoding: 'utf8', maxBuffer: 2 * 1024 * 1024});
assert.equal(native.status, 0, native.stderr);
const actual = JSON.parse(native.stdout);
assert.equal(actual.length, results.length);
await fs.writeFile(path.join(artifacts, 'native.json'), JSON.stringify(actual, null, 2) + '\n');

// Normalize representational differences whose coercion the timing gate
// proves end to end: JSON null/boolean selections and numeric strings.
const bits = (n) => {
  if (n === 0) return '0000000000000000';
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, n);
  let hex = '';
  for (let i = 0; i < 8; i++) hex += view.getUint8(i).toString(16).padStart(2, '0');
  return hex;
};
const normNumber = (value) => {
  if (value && typeof value === 'object') {
    if (value.nan) return 'NaN';
    if (value.n !== undefined) return value.n;
    if (value.b !== undefined) return bits(value.b ? 1 : 0);
    if (value.z) return bits(0);
    if (value.s !== undefined) {
      const n = Number(value.s);
      return isNaN(n) ? 'NaN' : bits(n);
    }
  }
  throw new Error('unexpected duck mapping value ' + JSON.stringify(value));
};
// An empty control array selects undefined, which reaches the orbit's
// parameter defaults: onset 0, depth 1. (Attack passes through Math.max
// first, so it arrives as NaN instead.)
const normDepth = (value) => {
  if (value && typeof value === 'object' && value.u) return bits(1);
  return normNumber(value);
};
const normOnset = (value) => {
  if (value && typeof value === 'object') {
    if (value.u) return bits(0);
    if (value.s !== undefined) return 's:' + value.s;
    if (value.z) return bits(0);
    if (value.b !== undefined) return bits(value.b ? 1 : 0);
    return normNumber(value);
  }
  throw new Error('unexpected duck onset ' + JSON.stringify(value));
};
const normAttack = (value) => {
  // WebAudio applies Math.max(null, .002) inside the orbit; native clamps at mapping.
  if (value && typeof value === 'object' && value.z) return bits(.002);
  return normNumber(value);
};
const failures = [];
results.forEach((test, index) => {
  const expected = test.seen.map((call) => ({
    orbit: call.orbit, onset: normOnset(call.onset),
    attack: normAttack(call.attack), depth: normDepth(call.depth),
  }));
  try {
    assert.deepEqual(actual[index].calls, expected);
  } catch {
    failures.push(`${test.name}: web ${JSON.stringify(expected)}, native ${JSON.stringify(actual[index].calls)}`);
  }
});
assert.deepEqual(failures, []);
console.log(`${results.length} web/native duck mapping cases passed`);
