/* Vowel-filter impulse responses through the deployed web filter, owned
   offline contexts only. Compares the five-formant parallel topology
   (per-formant gains, makeup x8) including the high-Q tails, which the old
   three-formant native approximation did not reproduce. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

const LETTERS = ['a', 'e', 'i', 'o', 'u', 'ae', 'aa', 'oe', 'ue', 'y', 'uh', 'un', 'en', 'an', 'on'];
const FRAMES = 44100, STRIDE = 7;

async function capture() {
  const letters = ['a', 'e', 'i', 'o', 'u', 'ae', 'aa', 'oe', 'ue', 'y', 'uh', 'un', 'en', 'an', 'on'];
  const frames = 44100, stride = 7;
  const results = [];
  for (const letter of letters) {
    const context = new OfflineAudioContext(1, frames, 44100);
    const impulse = context.createBuffer(1, frames, 44100);
    impulse.getChannelData(0)[0] = 1;
    const source = context.createBufferSource();
    source.buffer = impulse;
    const vowel = context.createVowelFilter(letter);
    source.connect(vowel);
    vowel.connect(context.destination);
    source.start();
    const pcm = (await context.startRendering()).getChannelData(0);
    const points = [];
    for (let frame = 0; frame < frames; frame += stride) points.push(pcm[frame]);
    results.push({letter, points});
  }
  return results;
}

const browser = await BrowserClient.create();
let results;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function"', 20000);
  results = await browser.evaluate('(' + capture.toString() + ')()');
} finally { await browser.close(); }
const artifacts = process.argv[2] ?? '/tmp/tompouce-voice-vowel-reference';
await fs.mkdir(artifacts, {recursive: true});
await fs.writeFile(path.join(artifacts, 'web.json'), JSON.stringify(results) + '\n');

const native = spawnSync(process.env.TOMPOUCE_VOICE_BINARY ?? './build/tests/test_audio_performance',
  ['--vowel-reference'], {input: JSON.stringify(results.map(({letter}) => ({letter}))),
  encoding: 'utf8', maxBuffer: 4 * 1024 * 1024});
assert.equal(native.status, 0, native.stderr);
const actual = JSON.parse(native.stdout);
assert.equal(actual.length, results.length);
await fs.writeFile(path.join(artifacts, 'native.json'), JSON.stringify(actual) + '\n');
let worst = 0, failures = 0;
results.forEach((test, index) => {
  test.points.forEach((gain, i) => {
    const diff = Math.abs(actual[index][i] - gain);
    const tol = 1e-6 + 1e-5 * Math.abs(gain);
    if (diff > worst) worst = diff;
    if (diff > tol) {
      if (failures++ < 12) console.error({letter: test.letter, frame: i * STRIDE, web: gain, native: actual[index][i]});
    }
  });
});
assert.equal(failures, 0, 'vowel impulse-response differences');
console.log(`${results.length} web/native vowel impulse responses passed (worst ${worst})`);
