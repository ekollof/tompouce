/* Dominant MIDI extras against the deployed web REPL: hap value shapes from
   live pattern queries plus deployed send-validation predicates, compared
   with native commit decisions. Headless Chromium grants no WebMIDI access,
   so browser sends cannot be captured; instead the gate checks the exact
   hap values the scheduler would send (queryArc on a disposable test REPL)
   and asserts the deployed bundle still carries the validation predicates
   and byte mappings the native rules were verified against. A deployed
   change fails the gate for investigation, never for threshold loosening.
   Mini-notation strings are spelled with explicit mini() because the test
   REPL evaluates without the page transpiler; the native runtime transpiles
   double-quoted strings to the same mini calls. Owned disposable browser
   profile; no audio device is opened. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

const cases = [
  {name: 'cc', code: 'note(60).ccn(74).ccv(0.5).midi()',
    events: [{chan: 0, note: 60, cc: 74, ccv: 64, cycle_start: 1}]},
  {name: 'control', code: 'note(60).control([74, 0.5]).midi()',
    events: [{chan: 0, note: 60, cc: 74, ccv: 64, cycle_start: 1}]},
  {name: 'cconly', code: 'ccn(74).ccv(0.5).midi()',
    events: [{chan: 0, note: -1, cc: 74, ccv: 64, cycle_start: 1}]},
  {name: 'prog', code: 'note(60).progNum(10).midi()',
    events: [{chan: 0, note: 60, prog: 10, cycle_start: 1}]},
  {name: 'bendtouch', code: 'note(60).midibend(0.25).miditouch(0.75).midi()',
    events: [{chan: 0, note: 60, bend: 2047, touch: 95, cycle_start: 1}]},
  {name: 'sysex', code: 'note(60).sysex([0x43, [0x79, 0x09]]).midi()',
    events: [{chan: 0, note: 60, sysex_id: [67], sysex: [121, 9], cycle_start: 1}]},
  {name: 'badprog', code: 'note(60).progNum(200).ccn(74).ccv(0.5).midi()',
    events: [{chan: 0, note: 60}]},
  {name: 'badccv', code: 'note(60).ccn(74).ccv(64).midibend(0.5).midi()',
    events: [{chan: 0, note: 60}]},
  {name: 'halfcc', code: 'note(60).ccn(74).midi()',
    events: [{chan: 0, note: 60, cycle_start: 1}]},
  {name: 'restcc', code: "s('~').ccn(74).ccv(0.5).midi()",
    events: [{chan: 0, note: -1, cc: 74, ccv: 64, cycle_start: 1}]},
  {name: 'drumcc', code: "s('bd').ccn(20).ccv(0.5).midi()",
    events: [{chan: 0, note: -1, cc: 20, ccv: 64, cycle_start: 1}]},
  {name: 'cmdstop', code: 'midicmd("stop").midi()',
    events: [{chan: 0, note: -1, cmd: 4, cycle_start: 1}]},
  {name: 'cmdbogus', code: 'midicmd("bogus").midi()',
    events: [{chan: 0, note: -1, cycle_start: 1}]},
  {name: 'cmdclock', code: 'midicmd("clock").midi()',
    events: [{chan: 0, note: -1, cmd: 1, cycle_start: 1}]},
  {name: 'port', code: 'note(60).midiport(2).ccn(1).ccv(1).midi()',
    events: [{chan: 0, note: 60, cc: 1, ccv: 127, cycle_start: 1}]},
  {name: 'portstr', code: 'note(60).midiport("out").midi()',
    events: [{chan: 0, note: 60, cycle_start: 1}]},
  {name: 'chan', code: 'note(60).midichan(3).ccn(1).ccv(0).midi()',
    events: [{chan: 2, note: 60, cc: 1, ccv: 0, cycle_start: 1}]},
  {name: 'seq', code: 'note("60 62").ccn("74 75").ccv("0.5 0.25").midi()',
    webcode: 'note(mini("60 62")).ccn(mini("74 75")).ccv(mini("0.5 0.25")).midi()',
    events: [{chan: 0, note: 60, cc: 74, ccv: 64, cycle_start: 1},
      {chan: 0, note: 62, cc: 75, ccv: 32}]},
  {name: 'progfloat', code: 'note(60).progNum(10.9).midi()',
    events: [{chan: 0, note: 60, prog: 10, cycle_start: 1}]},
  {name: 'bendends', code: 'note(60).midibend(-1).miditouch(1).midi()',
    events: [{chan: 0, note: 60, bend: -8192, touch: 127, cycle_start: 1}]},
];
// Pinned 1.2.6 coerces numeric strings in control args (ccn("74") arrives
// as 74; sysexid "0x77" as 119) while the deployed REPL preserves them, so
// send outcomes diverge there by runtime version. Documented drift, kept out
// of the exact gate; AGENTS.md pins the runtime references.

async function webValues(browser) {
  return browser.evaluate(`(async()=>{
    const results = [];
    for (const test of ${JSON.stringify(cases.map(c => ({name: c.name, code: c.webcode ?? c.code})))} ) {
      const instance = repl({getTime:()=>0, onSchedule:()=>{}, setInterval:()=>1, clearInterval:()=>{}});
      const entry = {name: test.name};
      try {
        await instance.evaluate(test.code, false);
        if (instance.state.evalError) entry.error = String(instance.state.evalError?.message ?? instance.state.evalError);
        else {
          const haps = instance.scheduler.pattern.queryArc(0, 1);
          entry.values = haps.slice(0, 4).map(h => ({begin: Number(h.whole.begin), end: Number(h.whole.end), value: h.value}));
        }
      } catch (error) { entry.error = String(error?.message ?? error); }
      try { instance.stop(); } catch {}
      results.push(entry);
    }
    return results;
  })()`);
}

// Deployed send-validation predicates the native rules were verified
// against (strudel.cc bundle, 2026-10-20). Any drift fails for review.
const predicates = [
  'expected ccv to be a number between 0 and 1',
  'expected progNum (program change) to be a number between 0 and 127',
  'expected midibend to be a number between -1 and 1',
  'expected miditouch to be a number between 0 and 1',
  'all sysexid bytes must be integers between 0 and 255',
  'expected sysex to be an array of numbers (0-255)',
  'all sysex bytes must be integers between 0 and 255',
  'fromFloatToMsbLsb',
  'fromFloatTo7Bit',
];

const browser = await BrowserClient.create();
let web, bundleInfo;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof repl==="function"', 25000);
  web = await webValues(browser);
  const bundle = await browser.evaluate(`(async()=>{
    const resources = performance.getEntriesByType('resource').map(e=>e.name).filter(u=>u.endsWith('.js'));
    for (const url of resources) {
      const text = await (await fetch(url)).text();
      if (text.includes('sendProgramChange') && text.includes('sendPitchBend')) return {url, text};
    }
    throw new Error('deployed MIDI module not found');
  })()`);
  const missing = predicates.filter(p => !bundle.text.includes(p));
  assert.deepEqual(missing, [], 'deployed MIDI validation changed; review native rules');
  bundleInfo = {url: bundle.url, sha256_16: createHash('sha256').update(bundle.text).digest('hex').slice(0, 16)};
} finally { await browser.close(); }

const artifacts = path.resolve(process.argv[2] ?? '/tmp/tompouce-midi-reference');
await fs.mkdir(artifacts, {recursive: true});
await fs.writeFile(path.join(artifacts, 'web.json'), JSON.stringify({bundle: bundleInfo, cases: web}, null, 2) + '\n');

const native = spawnSync(process.env.TOMPOUCE_MIDI_BINARY ?? './build/tests/test_midi',
  ['--midi-reference'], {input: JSON.stringify(cases.map(c => ({name: c.name, code: c.code}))),
  encoding: 'utf8', maxBuffer: 4 * 1024 * 1024});
assert.equal(native.status, 0, native.stderr);
const actual = JSON.parse(native.stdout);
await fs.writeFile(path.join(artifacts, 'native.json'), JSON.stringify(actual, null, 2) + '\n');

const stripNative = (value) => {
  const clean = {};
  for (const [key, entry] of Object.entries(value)) {
    if (key.startsWith('_')) continue;
    clean[key] = entry;
  }
  return clean;
};
const failures = [];
assert.equal(actual.length, cases.length);
assert.equal(web.length, cases.length);
for (const test of cases) {
  const want = web.find(w => w.name === test.name);
  const got = actual.find(a => a.name === test.name);
  assert.ok(want && got, `missing case ${test.name}`);
  const norm = (haps, strip) => (haps ?? []).map(h => {
    const value = strip ? stripNative(h.value) : h.value;
    return {begin: h.begin, end: h.end, value};
  });
  try {
    assert.deepEqual(norm(got.values, true), norm(want.values, false));
  } catch { failures.push(`${test.name}: hap values differ (web ${JSON.stringify(want.values)} native ${JSON.stringify(got.values)})`); }
  try {
    assert.deepEqual(got.events ?? [], test.events);
  } catch { failures.push(`${test.name}: commit differs (want ${JSON.stringify(test.events)} got ${JSON.stringify(got.events)})`); }
}
assert.deepEqual(failures, []);
console.log(`${cases.length} web/native MIDI cases passed (bundle ${bundleInfo.sha256_16})`);
