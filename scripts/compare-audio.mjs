// Diagnostic only. A separate Chromium process renders the actual web player.
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { build } from '../js/node_modules/esbuild/lib/main.js';
import { StringDecoder } from 'node:string_decoder';

const renderer = process.env.TOMPOUCE_RENDER_AUDIO ?? './build/tests/render_audio';
const out = path.resolve(process.argv[2] ?? '/tmp/tompouce-comparison');
await fs.mkdir(out, { recursive: true });
// Exact shared PCM isolates the vocoder from independently decoded MP3s.
// The independently decoded MP3 matrix separately guards decoder precision.
const fixture=Buffer.alloc(44+44100*4);
fixture.write('RIFF');fixture.writeUInt32LE(fixture.length-8,4);fixture.write('WAVEfmt ',8);
fixture.writeUInt32LE(16,16);fixture.writeUInt16LE(1,20);fixture.writeUInt16LE(2,22);
fixture.writeUInt32LE(44100,24);fixture.writeUInt32LE(176400,28);fixture.writeUInt16LE(4,32);fixture.writeUInt16LE(16,34);
fixture.write('data',36);fixture.writeUInt32LE(fixture.length-44,40);
for(let i=0;i<44100;i++) {
  const t=i/44100,fade=Math.min(1,t/.003)*Math.exp(-t*2);
  const x=Math.round(6000*fade*(Math.sin(2*Math.PI*440*t)+.4*Math.sin(2*Math.PI*1271*t)+.2*Math.sin(2*Math.PI*7123*t)));
  fixture.writeInt16LE(x,44+i*4);fixture.writeInt16LE(-2*x,46+i*4);
}
const fixtureRoot=path.join(out,'fixture-banks');
await fs.mkdir(path.join(fixtureRoot,'Dirt-Samples'),{recursive:true});
await fs.writeFile(path.join(fixtureRoot,'Dirt-Samples/stretch.wav'),fixture);
await fs.writeFile(path.join(fixtureRoot,'Dirt-Samples/strudel.json'),JSON.stringify({stretch_fixture:['stretch.wav']}));
const nativeEnvironment={...process.env};
if (!nativeEnvironment.TOMPOUCE_BANKS && !nativeEnvironment.STRUDELTERM_BANKS) {
  for (const candidate of ['assets/banks',path.join(os.homedir(),'.local/lib/tompouce/assets/banks')]) {
    try { await fs.access(path.join(candidate,'dough-samples/piano.json')); nativeEnvironment.TOMPOUCE_BANKS=path.resolve(candidate); break; }
    catch(error) {if(error.code!=='ENOENT')throw error;}
  }
}
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
    { name: 'sine-440', code: `note(69).s('sine').gain(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-fractional-delay', code: `note(69).s('sine').gain(.5).clip(.025).dry(0).delay(1).delaytime(128.5/44100).delayfeedback(.5)`, sound: 'sine', cycles: 1, cps: .5, window: .3, correlation: .99 },
    { name: 'sine-delay-auto-feedback', code: `note("c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime(.05).delayfeedback("0.5 0.75")`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-delay-auto-time', code: `note("c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime("0.05 0.075").delayfeedback(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-delay-auto-zero', code: `note("c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime("0.05 0").delayfeedback(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-delay-auto-return', code: `note("c4 c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime("0.05 0.075 0.05").delayfeedback("0.5 0.75 0.5")`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-adsr', code: `note(69).s('sine').gain(.5).attack(.02).decay(.1).sustain(.4).release(.2).clip(.25)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-lowpass', code: `note(69).s('sine').gain(.5).lpf(800).lpq(1)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-pan-center', code: `note(69).s('sine').gain(.5).pan(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-c4-dry', code: `note(60).s('piano').clip(1).gain(.5)`, sound: 'piano', cycles: 1, cps: .5, window: .6, correlation: .99 },
    { name: 'arpoon-without-room', code: arpoon.code.trimEnd() + '.room(0)', sound: 'piano', cycles: 4, cps: .5, window: .6, correlation: .99 },
    { name: 'piano-custom-ir', code: `note(60).s('piano').clip(.25).ir("RolandTR909_sd:0").room(1).roomsize(.2).dry(0)`, sound: 'piano', cycles: 1, cps: .5, window: .6, correlation: .99 },
    { name: 'piano-generated-room-seeded', code: `note(60).s('piano').clip(.25).room(.5).roomsize(1)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99, randomSeed: 0xc0ffee },
    // A real onset at 0.25s avoids the upstream stretcher's negative-onset
    // rejection at zero (its graph schedules 40ms early). No captures shift.
    { name: 'piano-stretch', code: `note(60).s('piano').speed(.5).stretch(1).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-fractional', code: `note(60).s('piano').speed(.75).stretch(1/3).clip(.25).late(.137)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-up', code: `note(60).s('piano').stretch(1).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down', code: `note(60).s('piano').stretch(-2).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down-shallow', code: `note(60).s('piano').stretch(-1).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down-deep', code: `note(60).s('piano').stretch(-3).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down-fractional', code: `note(60).s('piano').speed(.75).stretch(-2).clip(.25).late(.137)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down-speed', code: `note(60).s('piano').speed(.75).stretch(-2).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down-onset', code: `note(60).s('piano').stretch(-2).clip(.25).late(.137)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down-fs3', code: `note(54).s('piano').stretch(-2).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-down-ds4', code: `note(63).s('piano').stretch(-2).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-stretch-down', code: `s('stretch_fixture').stretch(-2).clip(.25).late(.125)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-stretch-down-fractional', code: `s('stretch_fixture').speed(.75).stretch(-2).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional', code: `s('stretch_fixture').clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-speed', code: `s('stretch_fixture').speed(.75).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-fast', code: `s('stretch_fixture').speed(2).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-reverse', code: `s('stretch_fixture').speed(-2).begin(.25).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-speedswitch', code: `s('stretch_fixture').stretch(-2).speed("0.75 1").clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-delayauto', code: `s('stretch_fixture').gain(.5).clip(.25).late(.137).dry(0).delay(1).delaytime("0.05 0.075").delayfeedback(.5)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-roomswitch', code: `s('stretch_fixture').fast(2).clip(.25).late(.137).room(.5).roomsize("0.5 1")`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: null, randomSeed: 0xc0ffee }, /* Diagnostic: onsets align exactly; regenerated rooms use fresh web randomness per call while native reseeds per call, so wet tails differ. */
    { name: 'piano-stretch-reverse', code: `note(60).s('piano').speed(-.5).stretch(1).begin(.98).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-loop', code: `note(60).s('piano').speed(.5).stretch(1).loop(1).loopBegin(.005).loopEnd(.01).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-envelope-filter', code: `note(60).s('piano').speed(.5).stretch(1).attack(.03).decay(.08).sustain(.4).release(.12).lpf(1600).lpq(.7).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'zzfx-sine', code: `note(69).s('z_sine').zrand(0).gain(.5).attack(.01).decay(.04).sustain(.5).release(.1).clip(.25)`, sound: 'z_sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'bytebeat', code: `note(60).s('bytebeat').bbexpr('t*(t>>15^t>>66)').bbst(65536).gain(.5).clip(.25).late(.125)`, sound: 'bytebeat', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'saw-440', code: `note(69).s('sawtooth').gain(.5)`, sound: 'sawtooth', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'square-440', code: `note(69).s('square').gain(.5)`, sound: 'square', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'triangle-440', code: `note(69).s('triangle').gain(.5)`, sound: 'triangle', cycles: 1, cps: .5, window: 2, correlation: .99 },
    // Different reverb algorithms and randomized browser impulses: record
    // evidence, without inventing a universal acceptance threshold.
    { name: 'arpoon-opening', code: arpoon.code, sound: 'piano', cycles: 4, cps: .5, window: .6, correlation: null },
  ];
  const report = { browser: version, url: 'https://strudel.cc/', capturedAt: new Date().toISOString(),
    nativeBundleSha256: hash(await fs.readFile('js/dist/runtime.js')),
    nativeRendererSha256: hash(await fs.readFile(renderer)),
    nativePackages: JSON.parse(await fs.readFile('js/package.json', 'utf8')).dependencies,
    scheduling: 'browser eager offline path; native sample-frame triggers', cases: [] };
  report.browserScripts = await evaluate(`Promise.all(Array.from(document.scripts).filter(s=>s.src).map(async s=>{
    const bytes=await (await fetch(s.src)).arrayBuffer();
    return {url:s.src,sha256:Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))).map(b=>b.toString(16).padStart(2,'0')).join('')};
  }))`);
  for (const spec of cases.filter(spec=>!process.env.TOMPOUCE_COMPARE_FILTER || spec.name.includes(process.env.TOMPOUCE_COMPARE_FILTER))) {
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
    const rendered = spawnSync(renderer, [request, native], { encoding: 'utf8', timeout: 120000,
      env:spec.fixture?{...nativeEnvironment,TOMPOUCE_BANKS:fixtureRoot}:nativeEnvironment });
    if (rendered.status !== 0) throw new Error(rendered.stderr || String(rendered.error));
    const captured = await evaluate(`(async()=>{
      const spec=${JSON.stringify(spec)};
      if(spec.fixture){const bytes=Uint8Array.from(atob(${JSON.stringify(fixture.toString('base64'))}),c=>c.charCodeAt(0));await samples({stretch_fixture:[URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}))]})}
      if(spec.randomSeed!==undefined){let seed=spec.randomSeed>>>0;Math.random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296}}
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
    report.cases.push({ ...spec,...(spec.fixture?{sharedSourceSha256:hash(fixture)}:{}),codeSha256: hash(spec.code), nativePcmSha256: hash(nativeBytes), browserWavSha256: hash(wav), metrics, passed });
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
  let xy = 0, xx = 0, yy = 0, error = 0, nativePeak=0, browserPeak=0;
  for(let i=0;i<native.length/4;i++) {
    const x=native.readFloatLE(i*4); if(!Number.isFinite(x))throw new Error('Non-finite native capture');
    nativePeak=Math.max(nativePeak,Math.abs(x));
  }
  for(let i=0;i<data.length/(bits/8);i++) {
    const y=format===1?data.readInt16LE(i*2)/32768:data.readFloatLE(i*4);
    if(!Number.isFinite(y))throw new Error('Non-finite browser capture'); browserPeak=Math.max(browserPeak,Math.abs(y));
  }
  for (let i = 0; i < samples; i++) {
    const x = native.readFloatLE(i * 4), y = format === 1 ? data.readInt16LE(i * 2) / 32768 : data.readFloatLE(i * 4);
    if (!Number.isFinite(x) || !Number.isFinite(y)) throw new Error('Non-finite audio');
    xy += x*y; xx += x*x; yy += y*y; error += (x-y)**2;
  }
  if (!xx || !yy) throw new Error('Silent capture');
  return { correlation: xy / Math.sqrt(xx*yy), rmsRatio: Math.sqrt(xx/yy), rmsError: Math.sqrt(error/samples),
    windowSeconds: seconds, lagFrames: 0, nativePeak,browserPeak,
    nativeFrames: native.length / 8, browserFrames: data.length / (bits / 8) / 2 };
}
