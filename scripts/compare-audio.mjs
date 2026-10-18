// Diagnostic only. A separate Chromium process renders the actual web player.
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { build } from '../js/node_modules/esbuild/lib/main.js';
import { StringDecoder } from 'node:string_decoder';

const out = path.resolve(process.argv[2] ?? '/tmp/tompouce-comparison');
await fs.mkdir(out, { recursive: true });
const profile = await fs.mkdtemp(path.join(os.tmpdir(), 'tompouce-browser-'));
const browser = spawn(process.env.CHROMIUM ?? 'chromium', [
  '--headless', '--no-first-run', '--disable-gpu', '--remote-debugging-pipe',
  `--user-data-dir=${profile}`, 'about:blank',
], { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'] });
let id = 0, buffer = '', session;
let startupError;
const decoder = new StringDecoder('utf8');
const pending = new Map();
browser.stdio[4].on('data', data => {
  buffer += decoder.write(data);
  for (;;) {
    const end = buffer.indexOf('\0');
    if (end < 0) break;
    const message = JSON.parse(buffer.slice(0, end)); buffer = buffer.slice(end + 1);
    if (pending.has(message.id)) {
      const { resolve, reject, timer } = pending.get(message.id);
      pending.delete(message.id); clearTimeout(timer);
      message.error ? reject(new Error(JSON.stringify(message.error))) : resolve(message.result);
    }
  }
});
browser.on('exit', () => {
  for (const { reject, timer } of pending.values()) { clearTimeout(timer); reject(new Error('Chromium exited')); }
  pending.clear();
});
browser.on('error', error => {
  startupError = error;
  for (const { reject, timer } of pending.values()) { clearTimeout(timer); reject(error); }
  pending.clear();
});
function call(method, params = {}, sessionId = session) {
  return new Promise((resolve, reject) => {
    if (startupError) { reject(startupError); return; }
    const request = ++id;
    const timer = setTimeout(() => { pending.delete(request); reject(new Error(`Timed out: ${method}`)); }, 120000);
    pending.set(request, { resolve, reject, timer });
    browser.stdio[3].write(JSON.stringify({ id: request, method, params, ...(sessionId ? {sessionId} : {}) }) + '\0');
  });
}
async function evaluate(expression) {
  const result = await call('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
  if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
  return result.result.value;
}
try {
  const version = await call('Browser.getVersion');
  const target = await call('Target.createTarget', { url: 'https://strudel.cc/' });
  session = (await call('Target.attachToTarget', { targetId: target.targetId, flatten: true })).sessionId;
  let ready = false;
  for (let attempt = 0; attempt < 120; attempt++) {
    ready = await evaluate(`typeof renderPatternAudio === 'function' && typeof getSound === 'function'`);
    if (ready) break;
    await new Promise(resolve => setTimeout(resolve, 500));
  }
  if (!ready) throw new Error('strudel.cc audio API did not initialize');
  const transpiler = await build({ stdin: {
    contents: `export {transpiler} from '@strudel/transpiler'`, resolveDir: path.resolve('js'),
  }, bundle: true, format: 'iife', globalName: '__comparisonTranspiler', write: false, platform: 'browser' });
  await evaluate(transpiler.outputFiles[0].text);
  const corpus = JSON.parse(await fs.readFile('tests/fixtures/strudel-examples.json', 'utf8'));
  const arpoon = corpus.examples.find(example => example.title === 'Arpoon');
  const cases = [
    { name: 'piano-c4-dry', code: `note(60).s('piano').clip(1).gain(.5)`, sound: 'piano', cycles: 1, cps: .5, window: .6, correlation: .99 },
    { name: 'arpoon-without-room', code: arpoon.code.trimEnd() + '.room(0)', sound: 'piano', cycles: 4, cps: .5, window: .6, correlation: .99 },
    { name: 'piano-custom-ir', code: `note(60).s('piano').clip(.25).ir("RolandTR909_sd:0").room(1).roomsize(.2).dry(0)`, sound: 'piano', cycles: 1, cps: .5, window: .6, correlation: .99 },
    // Different reverb algorithms and randomized browser impulses: record
    // evidence, without inventing a universal acceptance threshold.
    { name: 'arpoon-opening', code: arpoon.code, sound: 'piano', cycles: 4, cps: .5, window: .6, correlation: null },
  ];
  const report = { browser: version, url: 'https://strudel.cc/', capturedAt: new Date().toISOString(),
    nativeBundleSha256: hash(await fs.readFile('js/dist/runtime.js')),
    nativeRendererSha256: hash(await fs.readFile('tests/render_audio')),
    nativePackages: JSON.parse(await fs.readFile('js/package.json', 'utf8')).dependencies,
    scheduling: 'browser eager offline path; native sample-frame triggers', cases: [] };
  report.browserScripts = await evaluate(`Promise.all(Array.from(document.scripts).filter(s=>s.src).map(async s=>{
    const bytes=await (await fetch(s.src)).arrayBuffer();
    return {url:s.src,sha256:Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))).map(b=>b.toString(16).padStart(2,'0')).join('')};
  }))`);
  for (const spec of cases) {
    const page = await call('Target.createTarget', { url: 'https://strudel.cc/' }, null);
    session = (await call('Target.attachToTarget', { targetId: page.targetId, flatten: true }, null)).sessionId;
    let initialized = false;
    for (let attempt = 0; attempt < 120; attempt++) {
      initialized = await evaluate(`typeof renderPatternAudio === 'function' && typeof getSound === 'function'`);
      if (initialized) break;
      await new Promise(resolve => setTimeout(resolve, 500));
    }
    if (!initialized) throw new Error('Capture page did not initialize');
    await evaluate(transpiler.outputFiles[0].text);
    const request = path.join(out, `${spec.name}.json`), native = path.join(out, `${spec.name}-native.f32`);
    await fs.writeFile(request, JSON.stringify(spec, null, 2));
    const rendered = spawnSync('./tests/render_audio', [request, native], { encoding: 'utf8', timeout: 120000 });
    if (rendered.status !== 0) throw new Error(rendered.stderr || String(rendered.error));
    const captured = await evaluate(`(async()=>{
      const spec=${JSON.stringify(spec)};
      const {pattern}=await evaluate(spec.code,__comparisonTranspiler.transpiler);
      const selected=pattern.filterValues(v=>v.s===spec.sound);
      const haps=selected.queryArc(0,spec.cycles,{_cps:spec.cps}).filter(h=>h.hasOnset());
      const original=URL.createObjectURL, click=HTMLAnchorElement.prototype.click;
      let blob;
      URL.createObjectURL=function(value){blob=value;return original.call(URL,value)};
      HTMLAnchorElement.prototype.click=function(){};
      // Use the renderer's supported eager path. Its suspend/resume path
      // races completion for short offline clips in current Chromium.
      const suspend=OfflineAudioContext.prototype.suspend;
      OfflineAudioContext.prototype.suspend=undefined;
      try {
        await renderPatternAudio(selected,spec.cps,0,spec.cycles,44100,128,false,'comparison');
        if(!blob)throw new Error('No WAV captured');
        const bytes=new Uint8Array(await blob.arrayBuffer());
        let binary='';for(let i=0;i<bytes.length;i+=8192)binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
        return {wav:btoa(binary),events:haps.map(h=>({begin:Number(h.whole.begin),end:Number(h.whole.end),value:h.value}))};
      } finally {URL.createObjectURL=original;HTMLAnchorElement.prototype.click=click;OfflineAudioContext.prototype.suspend=suspend}
    })()`);
    const wav = Buffer.from(captured.wav, 'base64');
    await fs.writeFile(path.join(out, `${spec.name}-browser.wav`), wav);
    await fs.writeFile(path.join(out, `${spec.name}-browser-events.json`), JSON.stringify(captured.events, null, 2));
    const nativeBytes = await fs.readFile(native);
    const metrics = compare(nativeBytes, wav, spec.window);
    const durationMatches = metrics.nativeFrames === Math.round(spec.cycles / spec.cps * 44100) && metrics.nativeFrames === metrics.browserFrames;
    const passed = spec.correlation === null ? null : durationMatches && metrics.correlation >= spec.correlation && metrics.rmsRatio >= .85 && metrics.rmsRatio <= 1.15;
    report.cases.push({ ...spec, codeSha256: hash(spec.code), nativePcmSha256: hash(nativeBytes), browserWavSha256: hash(wav), metrics, passed });
    console.log(spec.name, JSON.stringify(metrics), passed === null ? 'DIAGNOSTIC' : passed ? 'PASS' : 'FAIL');
    await call('Target.closeTarget', { targetId: page.targetId }, null);
  }
  await fs.writeFile(path.join(out, 'report.json'), JSON.stringify(report, null, 2));
  if (report.cases.some(result => result.passed === false)) process.exitCode = 1;
} finally {
  browser.kill();
  await new Promise(resolve => !browser.pid || browser.exitCode !== null ? resolve() : browser.once('exit', resolve));
  await fs.rm(profile, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
}

function hash(bytes) { return createHash('sha256').update(bytes).digest('hex'); }
function compare(native, wav, seconds) {
  if (wav.toString('ascii', 0, 4) !== 'RIFF' || wav.toString('ascii', 8, 12) !== 'WAVE') throw new Error('Invalid WAV');
  let format, channels, rate, bits, data;
  for (let offset = 12; offset + 8 <= wav.length;) {
    const size = wav.readUInt32LE(offset + 4), start = offset + 8;
    if (start + size > wav.length) throw new Error('Truncated WAV');
    const kind = wav.toString('ascii', offset, offset + 4);
    if (kind === 'fmt ') { format = wav.readUInt16LE(start); channels = wav.readUInt16LE(start + 2); rate = wav.readUInt32LE(start + 4); bits = wav.readUInt16LE(start + 14); }
    if (kind === 'data') data = wav.subarray(start, start + size);
    offset = start + size + (size % 2);
  }
  if (channels !== 2 || rate !== 44100 || !data || !((format === 1 && bits === 16) || (format === 3 && bits === 32))) throw new Error(`Unsupported WAV ${format}/${bits}/${channels}/${rate}`);
  const samples = Math.round(seconds * rate) * channels;
  if (native.length < samples * 4 || data.length < samples * bits / 8) throw new Error('Capture too short');
  let xy = 0, xx = 0, yy = 0, error = 0;
  for (let i = 0; i < samples; i++) {
    const x = native.readFloatLE(i * 4), y = format === 1 ? data.readInt16LE(i * 2) / 32768 : data.readFloatLE(i * 4);
    if (!Number.isFinite(x) || !Number.isFinite(y)) throw new Error('Non-finite audio');
    xy += x*y; xx += x*x; yy += y*y; error += (x-y)**2;
  }
  if (!xx || !yy) throw new Error('Silent capture');
  return { correlation: xy / Math.sqrt(xx*yy), rmsRatio: Math.sqrt(xx/yy), rmsError: Math.sqrt(error/samples),
    windowSeconds: seconds, lagFrames: 0, nativeFrames: native.length / 8, browserFrames: data.length / (bits / 8) / 2 };
}
