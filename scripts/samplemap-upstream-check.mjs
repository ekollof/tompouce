#!/usr/bin/env node
// Independent web-parity check for manifest sample-map interpretation.
// Downloads the real upstream modules (superdough sampler/util/logger) and
// diffs their normalization + trigger selection against Tompouce's importer
// contract. Manual QA only (needs network); not a ctest gate.
//
// Covered agreements: single/array/pitch-map forms, top-level and per-entry
// _base prefixing, nearest-key selection with first-wins ties, n wrapping.
// Known divergences (documented in docs/wiki/Sounds-and-Samples.md):
//  - upstream aborts the whole map on one malformed value; native skips it
//  - upstream registers _-prefixed keys as (broken) sounds; native drops them
//  - upstream rejects uppercase extensions; native accepts them
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const BASE = 'https://codeberg.org/uzu/strudel/raw/branch/main/packages/superdough';
const dir = mkdtempSync(join(tmpdir(), 'upstream-samplemap-'));
const get = async (name) => {
  const response = await fetch(`${BASE}/${name}`);
  if (!response.ok) throw new Error(`fetch ${name}: ${response.status}`);
  writeFileSync(join(dir, name), await response.text());
};
await Promise.all(['sampler.mjs', 'util.mjs', 'logger.mjs'].map(get));
writeFileSync(join(dir, 'index.mjs'), `
export const soundMap = new Map();
export const registerSound = () => {};
export const registerWaveTable = () => {};
`);
writeFileSync(join(dir, 'audioContext.mjs'), `
export const getAudioContext = () => { throw new Error('no audio in node'); };
`);
writeFileSync(join(dir, 'helpers.mjs'), `
export const getADSRValues = () => [];
export const getParamADSR = () => {};
export const getPitchEnvelope = () => {};
export const getVibratoOscillator = () => {};
export const onceEnded = () => {};
export const releaseAudioNode = () => {};
`);

const sampler = await import(join(dir, 'sampler.mjs'));
const util = await import(join(dir, 'util.mjs'));
let fails = 0;
const check = (name, actual, expected) => {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) {
    fails++;
    console.log(`FAIL ${name}\n  actual   ${a}\n  expected ${e}`);
  } else {
    console.log(`ok ${name}`);
  }
};

// Normalization: forms + base prefixing (well-formed subset; _-keys and
// malformed values exercise documented divergences, asserted separately).
const normalized = {};
sampler.processSampleMap(
  {
    _base: 'audio/',
    bd: 'bd.wav',
    Shared: ['sdx.wav', 'sdy.wav'],
    piano: { c2: 'low.wav', C4: ['high.wav', 'nest/deep.wav'] },
    perbase: { _base: 'other/', d2: 'snare.wav' },
  },
  (key, value) => {
    normalized[key] = value;
  },
);
check('single string wraps to array', normalized.bd, ['audio/bd.wav']);
check('flat array keeps order', normalized.Shared, ['audio/sdx.wav', 'audio/sdy.wav']);
check('pitch map regions', normalized.piano, {
  c2: ['audio/low.wav'],
  C4: ['audio/high.wav', 'audio/nest/deep.wav'],
});
check('per-entry base overrides', normalized.perbase.d2, ['other/snare.wav']);

// Selection: nearest key (first wins ties), n wraps within the region.
const bank = { c0: ['sub.wav'], c3: ['lo-a.wav', 'lo-b.wav'], c4: ['hi.wav'] };
const select = (midi, n) => {
  const { url, transpose, index } = util.getCommonSampleInfo({ s: 't', n, note: midi }, bank);
  return [url, transpose, index];
};
check('region low', select(48, 0), ['lo-a.wav', 0, 0]);
check('region high', select(60, 0), ['hi.wav', 0, 0]);
check('tie keeps first region', select(54, 0), ['lo-a.wav', 6, 0]);
check('closer region wins', select(59, 0), ['hi.wav', -1, 0]);
check('raw drum pitch hits lowest key', select(-1, 0), ['sub.wav', -13, 0]);
check('index wraps within region', select(48, 2), ['lo-a.wav', 0, 0]);
check('index addresses region', select(48, 1), ['lo-b.wav', 0, 1]);

// Documented divergences: malformed values abort the map upstream...
let threw = '';
try {
  sampler.processSampleMap({ ok: 'a.wav', bad: 42 }, () => {});
} catch (error) {
  threw = error.message;
}
check('upstream aborts map on malformed value', threw.startsWith('wrong sample map format'), true);
// ...and _-prefixed keys register as sounds upstream.
const underscore = {};
sampler.processSampleMap({ _base: 'audio/', bd: 'bd.wav' }, (k, v) => {
  underscore[k] = v;
});
check('upstream registers _base as a sound', Object.keys(underscore).includes('_base'), true);

if (fails) {
  console.error(`${fails} upstream-parity checks failed`);
  process.exit(1);
}
console.log('upstream sample-map parity holds (modulo documented divergences)');
