#!/usr/bin/env node
/* Repeatable unseeded live wtphaserand distribution check. Each run starts a
   fresh compare-audio process, so the native renderer reseeds from time/PID
   and the site worklet uses its own unseeded Math.random. The check compares
   rendered RMS/peak distributions rather than sample-exact PCM. */
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs/promises';
import path from 'node:path';

const runs = Math.max(2, Number(process.env.TOMPOUCE_QA_RUNS ?? 16));
const out = path.resolve(process.argv[2] ?? '/tmp/tompouce-wtphaserand-qa');
const name = 'diag-wt-phaserand-live';
const frames = 44100; // the case's one-second comparison window
await fs.mkdir(out, { recursive: true });

function nativeStats(bytes) {
  const pcm = new Float32Array(bytes.buffer, bytes.byteOffset, bytes.length / 4);
  const count = Math.min(pcm.length, frames * 2);
  let sum = 0, peak = 0;
  for (let i = 0; i < count; i++) {
    const value = pcm[i];
    sum += value * value;
    if (Math.abs(value) > peak) peak = Math.abs(value);
  }
  return { rms: Math.sqrt(sum / count), peak };
}

function browserStats(bytes) {
  let offset = 12, data = -1, length = 0;
  while (offset + 8 <= bytes.length) {
    const id = bytes.toString('ascii', offset, offset + 4);
    const size = bytes.readUInt32LE(offset + 4);
    if (id === 'data') { data = offset + 8; length = size; break; }
    offset += 8 + size + (size & 1);
  }
  if (data < 0) throw new Error('browser WAV has no data chunk');
  const count = Math.min(length / 2, frames * 2);
  let sum = 0, peak = 0;
  for (let i = 0; i < count; i++) {
    const value = bytes.readInt16LE(data + i * 2) / 32768;
    sum += value * value;
    if (Math.abs(value) > peak) peak = Math.abs(value);
  }
  return { rms: Math.sqrt(sum / count), peak };
}

function digest(bytes) {
  return createHash('sha256').update(bytes).digest('hex').slice(0, 16);
}

const native = [], browser = [];
for (let run = 1; run <= runs; run++) {
  const result = spawnSync(process.execPath, ['scripts/compare-audio.mjs', out], {
    env: { ...process.env, TOMPOUCE_COMPARE_FILTER: name },
    encoding: 'utf8', timeout: 300000,
  });
  if (result.status !== 0) {
    process.stderr.write(result.stdout || '');
    process.stderr.write(result.stderr || '');
    throw new Error(`compare-audio failed on run ${run}`);
  }
  const nativeBytes = await fs.readFile(path.join(out, `${name}-native.f32`));
  const browserBytes = await fs.readFile(path.join(out, `${name}-browser.wav`));
  native.push({ ...nativeStats(nativeBytes), digest: digest(nativeBytes) });
  browser.push({ ...browserStats(browserBytes), digest: digest(browserBytes) });
  process.stderr.write(`run ${run}/${runs}: native rms ${native.at(-1).rms.toFixed(6)} ` +
    `peak ${native.at(-1).peak.toFixed(6)}; site rms ${browser.at(-1).rms.toFixed(6)} ` +
    `peak ${browser.at(-1).peak.toFixed(6)}\n`);
}

function summary(list) {
  const rms = list.map(item => item.rms);
  const peak = list.map(item => item.peak);
  const mean = values => values.reduce((a, b) => a + b, 0) / values.length;
  const sd = values => {
    const average = mean(values);
    return Math.sqrt(values.reduce((a, b) => a + (b - average) ** 2, 0) / values.length);
  };
  return {
    runs: list.length,
    rms: { mean: mean(rms), sd: sd(rms), min: Math.min(...rms), max: Math.max(...rms) },
    peak: { mean: mean(peak), sd: sd(peak), min: Math.min(...peak), max: Math.max(...peak) },
    distinct: new Set(list.map(item => item.digest)).size,
  };
}

const nativeSummary = summary(native);
const browserSummary = summary(browser);
const relative = (a, b) => Math.abs(a - b) / Math.max(a, b, 1e-12);
const rmsDelta = relative(nativeSummary.rms.mean, browserSummary.rms.mean);
const peakDelta = relative(nativeSummary.peak.mean, browserSummary.peak.mean);
const overlap = (a, b) => Math.max(a.min, b.min) <= Math.min(a.max, b.max);
const checks = {
  nativeVaries: nativeSummary.distinct > 1,
  siteVaries: browserSummary.distinct > 1,
  rmsMeans: rmsDelta <= 0.05,
  peakMeans: peakDelta <= 0.10,
  rmsRangesOverlap: overlap(nativeSummary.rms, browserSummary.rms),
  peakRangesOverlap: overlap(nativeSummary.peak, browserSummary.peak),
};
const report = {
  case: name, generated: new Date().toISOString(), runs,
  native: nativeSummary, browser: browserSummary,
  bound: { relativeRmsMean: rmsDelta, relativePeakMean: peakDelta, checks },
  runsDetail: { native, browser },
};
await fs.writeFile(path.join(out, 'report.json'), JSON.stringify(report, null, 2));
console.log(JSON.stringify(report, null, 2));
if (Object.values(checks).some(ok => !ok)) process.exitCode = 1;
