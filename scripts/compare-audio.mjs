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
const monoFixture=Buffer.alloc(44+44100*2);
fixture.copy(monoFixture,0,0,44);
monoFixture.writeUInt32LE(monoFixture.length-8,4);monoFixture.writeUInt16LE(1,22);
monoFixture.writeUInt32LE(88200,28);monoFixture.writeUInt16LE(2,32);monoFixture.writeUInt32LE(monoFixture.length-44,40);
for(let i=0;i<44100;i++)monoFixture.writeInt16LE(fixture.readInt16LE(44+i*4),44+i*2);
await fs.writeFile(path.join(fixtureRoot,'Dirt-Samples/mono.wav'),monoFixture);
await fs.writeFile(path.join(fixtureRoot,'Dirt-Samples/strudel.json'),JSON.stringify({stretch_fixture:['stretch.wav'],mono_fixture:['mono.wav']}));
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
async function audioReady() {
  try {
    return await evaluate(`typeof renderPatternAudio === 'function' && typeof getSound === 'function'`);
  } catch (error) {
    /* A newly attached target can lose its about:blank context during
       navigation. Retry only this readiness probe, never an audio render. */
    if (/Cannot find default execution context|Execution context was destroyed/.test(error.message)) return false;
    throw error;
  }
}
try {
  const version = await call('Browser.getVersion');
  const target = await call('Target.createTarget', { url: 'https://strudel.cc/' });
  session = (await call('Target.attachToTarget', { targetId: target.targetId, flatten: true })).sessionId;
  let ready = false;
  for (let attempt = 0; attempt < 120; attempt++) {
    ready = await audioReady();
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
    ...[
      ['natural',''],['fast',`.speed(8).irspeed(8)`],['reverse',`.speed(-2).irspeed(-2)`],
      ['short-buffer',`.speed(8).irspeed(8).clip(1)`],
      ['pitch',`.penv(7).patt(.02).pdec(.2).psus(0).panchor(0)`],
      ['vibrato',`.vib(5).vibmod(.5)`],
      ['slice',`.begin(.5).end(.75)`],
      ['loop',`.loop(1).loopBegin(.1).loopEnd(.2).clip(.05)`],
      ['fxr',`.speed(8).irspeed(8).fxr(1.5)`],
    ].map(([name,controls])=>({name:`shared-room-lifetime-${name}`,sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',
      code:`s('stretch_fixture').gain(.03)${controls}.ir('mono_fixture').room(1).dry(0)`,cycles:1,cps:.5,window:2,correlation:.99})),
    {name:'shared-room-single',sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',code:`s('stretch_fixture').gain(.3).clip(.05).ir('mono_fixture').room(1).dry(0)`,cycles:1,cps:.5,window:2,correlation:.99},
    ...[
      ['send-toggle',`.room("1 0 1")`],
      ['postgain-zero',`.room(1).postgain("1 0 1")`],
      ['orbit-switch',`.room(1).orbit("1 2 1")`],
      ['delay-send',`.room(.5).delay(.5).delaytime(.075).delayfeedback(.5)`],
    ].map(([name,controls])=>({name:`shared-room-${name}`,sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',
      code:`s('stretch_fixture').fast(3).gain(.3).clip(.05).ir('mono_fixture')${controls}.dry(0)`,cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['send-toggle',`note("c4*3").delay("1 0 1").delaytime(.075).delayfeedback(.7)`],
      ['feedback-zero',`note("c4*3").delay(1).delaytime(.075).delayfeedback(".7 0 .4")`],
      ['time-negative',`note("c4*2").delay(1).delaytime(".075 -1").delayfeedback(.7)`],
      ['postgain-zero',`note("c4*3").delay(1).delaytime(".05 .075 .1").delayfeedback(.7).postgain("1 0 1")`],
      ['gain-zero',`note("c4*3").delay(1).delaytime(".05 .075 .1").delayfeedback(.7).gain(".5 0 .5")`],
      ['orbit-switch',`note("c4*3").delay(1).delaytime(".05 .075 .1").delayfeedback(.7).orbit("1 2 1")`],
      ['stack-order',`stack(note("c4*2").delaytime(.05),note("c5").late(.25).delaytime(.1)).delay(1).delayfeedback(.7)`],
      ['fractional',`note("c4*3").delay(1).delaytime(".05 .075 .1").delayfeedback(".5 .7 .4").late(.137)`],
    ].map(([name,pattern])=>({name:`shared-delay-${name}`,sound:'sine',code:`${pattern}.s('sine')${name==='gain-zero'?'':'.gain(.5)'}.clip(.05).dry(0)`,cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['baseline',''],
      ['release',`.penv(7).patt(.02).pdec(.1).psus(.3).panchor(0).prel(.2).release(.3)`],
      ['linear',`.penv(7).patt(.02).pdec(.2).psus(0).panchor(0)`],
      ['exponential',`.penv(-7).pcurve(1).patt(.03).pdec(.2).psus(.2).panchor(1)`],
      ['late',`.penv(7).patt(.02).pdec(.2).psus(0).panchor(0).late(.137)`],
      ['vibrato',`.vib(5).vibmod(.5)`],
    ].map(([name,controls])=>({name:`worklet-pitch-supersaw-${name}`,sound:'supersaw',seedSupersaw:true,
      code:`note(69).s('supersaw').gain(.5).clip(.25)${controls}`,cycles:1,cps:.5,window:name==='release'?.9:.6,correlation:.99})),
    ...[
      ['linear',`.penv(7).patt(.02).pdec(.2).psus(0).panchor(0)`],
      ['exponential',`.penv(-7).pcurve(1).patt(.03).pdec(.2).psus(.2).panchor(1)`],
      ['late',`.penv(7).patt(.02).pdec(.2).psus(0).panchor(0).late(.137)`],
      ['vibrato',`.vib(5).vibmod(.5)`],
      ['release',`.penv(7).patt(.02).pdec(.1).psus(.3).panchor(0).prel(.2).release(.3)`],
    ].map(([name,controls])=>({name:`worklet-pitch-pulse-${name}`,sound:'pulse',
      code:`note(69).s('pulse').gain(.2).clip(.25)${controls}`,cycles:1,cps:.5,window:name==='release'?.9:.6,correlation:.99})),
    {name:'worklet-pitch-graph',sound:'sine',code:`note(69).s('sine').gain(.5).K(sine(sFreq).mul(.1).out()).penv(12).patt(.1).vib(5).vibmod(.5)`,cycles:1,cps:.5,window:.6,correlation:.99},
    ...[
      ['linear',`.penv(7).patt(.02).pdec(.2).psus(0).panchor(0)`],
      ['exponential',`.penv(7).pcurve(1).patt(.02).pdec(.2).psus(0).panchor(0)`],
      ['late',`.penv(-7).patt(.1).pdec(.2).psus(.5).late(.137)`],
      ['vibrato',`.vib(5).vibmod(.5)`],
      ['release',`.penv(12).patt(.01).pdec(.05).psus(.3).panchor(0).prel(.2).release(.3)`],
      ['exponential-late',`.penv(-7).pcurve(1).patt(.03).pdec(.2).psus(.2).panchor(1).late(.137)`],
    ].map(([name,controls])=>({name:`sample-pitch-${name}`,sound:'mono_fixture',fixture:true,mono:true,
      code:`s('mono_fixture').gain(.5).clip(.25)${controls}`,cycles:1,cps:.5,window:.6,correlation:.99})),
    ...[1,-.75].map(speed=>({name:`sample-pitch-stereo-${speed}`,sound:'stretch_fixture',fixture:true,
      code:`s('stretch_fixture').gain(.5).speed(${speed}).penv(7).patt(.02).pdec(.2).psus(0).panchor(0).clip(.25)`,
      cycles:1,cps:.5,window:.6,correlation:.99})),
    ...[0,.5].map(fxr=>({name:`kabel-fxr-natural-long-delay-${fxr}`,sound:'stretch_fixture',fixture:true,
      code:`s('stretch_fixture').gain(.5).speed(8).worklet('audioin().delay(.8).out()').fxr(${fxr})`,cycles:1,cps:.5,window:1.2,correlation:.99})),
    ...[0,.005,.01].map(release=>({name:`kabel-src-short-release-${release}`,sound:'sine',
      code:`note(69).s('sine').gain(.4).clip(.05).release(${release}).K(sine(sFreq).mul(.1).out()).fxr(.005)`,cycles:1,cps:.5,window:.2,correlation:.99})),
    {name:'kabel-fxr-short-release',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.05).release(0).worklet('audioin().add(.1).out()').fxr(.005)`,cycles:1,cps:.5,window:.2,correlation:.99},
    ...[
      ['gain-low',`.gain(.2).worklet('audioin().fold(8).out()')`],
      ['gain-high',`.gain(.8).worklet('audioin().fold(8).out()')`],
      ['offset',`.gain(.2).worklet('audioin().add(.1).out()')`],
      ['shape',`.gain(.5).worklet('audioin().mul(3).out()').shape(.5)`],
      ['filter',`.gain(.5).worklet('audioin().fold(3).out()').lpf(800).lpq(1)`],
      ['tail-filter',`.gain(.5).clip(.05).worklet('audioin().delay(.1).fold(3).out()').lpf(800).lpq(1).fxr(.4)`],
      ['tail-offset',`.gain(.2).clip(.05).worklet('audioin().add(.1).out()').fxr(.4)`],
    ].map(([name,controls])=>({name:`kabel-order-${name}`,sound:'sine',code:`note(69).s('sine')${controls}`,cycles:1,cps:.5,window:1,correlation:.99})),
    {name:'kabel-order-stereo-shape',sound:'stretch_fixture',fixture:true,code:`s('stretch_fixture').gain(.5).clip(.05).worklet('audioin().delay(.1).out(0).mul(-.5).out(1)').shape(.5).fxr(.5)`,cycles:1,cps:.5,window:1,correlation:.99},
    ...[0,.005,.2,.5].map(release=>({name:`kabel-fxr-${release}`,sound:'sine',
      code:`note(69).s('sine').gain(.5).clip(.05).release(.01).worklet('audioin().delay(.1).out()').fxr(${release})`,
      cycles:1,cps:.5,window:.8,correlation:.99})),
    {name:'kabel-fxr-feedback',sound:'sine',code:`note(69).s('sine').gain(.5).clip(.05).release(.01).worklet('audioin().add(x=>x.delay(.05).mul(.5)).out()').fxr(.5)`,cycles:1,cps:.5,window:.8,correlation:.99},
    ...[false,true].map(natural=>({name:`kabel-fxr-stereo-${natural?'natural':'clipped'}`,sound:'stretch_fixture',fixture:true,
      code:`s('stretch_fixture').gain(.5).speed(4)${natural?'':'.clip(.05)'}.worklet('audioin().delay(.1).out()').fxr(.5)`,cycles:1,cps:.5,window:.8,correlation:.99})),
    {name:'kabel-fxr-explicit-channels',sound:'stretch_fixture',fixture:true,code:`s('stretch_fixture').gain(.5).clip(.05).worklet('audioin().delay(.1).out(0).mul(-.5).out(1)').fxr(.5)`,cycles:1,cps:.5,window:.8,correlation:.99},
    ...[0,.5].map(release=>({name:`kabel-src-fxr-${release}`,sound:'sine',
      code:`note(69).s('sine').clip(.05).release(.01).gain(.4).K(sine(sFreq).mul(.12).add(x=>x.delay(.1).mul(.45)).out()).fxr(${release})`,
      cycles:1,cps:.5,window:.8,correlation:.99})),
    {name:'kabel-src-adsr-fxr',sound:'sine',code:`note(69).s('sine').clip(.1).release(.01).gain(.4).K(sine(sFreq).mul(sGate.adsr(.001,.02,.8,.15)).mul(.12).out()).fxr(.4)`,cycles:1,cps:.5,window:.8,correlation:.99},
    {name:'kabel-src-shape',sound:'sine',code:`note(69).s('sine').gain(.4).K(sine(sFreq).mul(.2).out()).shape(.5)`,cycles:1,cps:.5,window:.5,correlation:.99},
    {name:'kabel-src-filter',sound:'sine',code:`note(69).s('sine').gain(.4).K(sine(sFreq).mul(.2).out()).lpf(800).lpq(1)`,cycles:1,cps:.5,window:.5,correlation:.99},
    {name:'kabel-src-pan',sound:'sine',code:`note(69).s('sine').gain(.4).K(sine(sFreq).mul(.2).out()).pan(.2)`,cycles:1,cps:.5,window:.5,correlation:.99},
    {name:'kabel-src-late',sound:'sine',code:`note(69).s('sine').gain(.4).late(.137).K(sine(sFreq).mul(.2).out())`,cycles:1,cps:.5,window:.5,correlation:.99},
    {name:'kabel-fx-late',sound:'sine',code:`note(69).s('sine').late(.137).clip(.05).release(.01).gain(.5).worklet('audioin().delay(.05).out()').fxr(.3)`,cycles:1,cps:.5,window:.8,correlation:.99},
    ...['sawtooth','square','triangle'].flatMap(sound=>[1,8,32,64].map(n=>({
      name:`source-${sound}-n${n}`,code:`note(69).s('${sound}').n(${n}).gain(.5)`,sound,
      cycles:1,cps:.5,window:2,correlation:.99}))),
    ...[
      {name:'source-user-partials',code:`note(69).s('user').partials([1,.5,0,.2]).gain(.5)`,sound:'user'},
      {name:'source-triangle-phases',code:`note(69).s('triangle').partials([1,0,.5,0,.2]).phases([.25,0,.125,0,.5]).gain(.5)`,sound:'triangle'},
      {name:'source-sine-partials-ignored',code:`note(69).s('sine').partials([0,0,1]).gain(.5)`,sound:'sine'},
      {name:'source-pulse-fractional',code:`note(69).s('pulse').gain(.5).pwrate(3).late(.137)`,sound:'pulse'},
      {name:'source-pulse-quantum-boundary',code:`note(69).s('pulse').gain(.5).late(64/44100)`,sound:'pulse'},
      ...[0,.5,1].map(pan=>({name:`source-pulse-pan${pan}`,code:`note(69).s('pulse').gain(.5).pan(${pan})`,sound:'pulse'})),
      {name:'source-n-high-pitch',code:`note(100).s('sawtooth').n(128).gain(.5)`,sound:'sawtooth'},
      {name:'source-n-maximum',code:`note(48).s('square').n(2047).gain(.5)`,sound:'square'},
      {name:'source-noise-fractional',code:`note(69).s('sine').noise(1).gain(.5).late(.137)`,sound:'sine',seedNoise:true},
      {name:'source-noise-partials',code:`note(69).s('square').n(8).noise(.5).gain(.5)`,sound:'square',seedNoise:true},
      {name:'source-pink-loop',code:`note(69).s('pink').gain(.5)`,sound:'pink',seedNoise:true,cps:.25,window:4},
      ...[{}, {pw:.2}, {pwrate:3}, {pwsweep:.4}, {pw:.7,pwrate:2,pwsweep:.3}].map((controls,i)=>({
        name:`source-pulse-${i}`,sound:'pulse',code:`note(69).s('pulse').gain(.5)`+
          Object.entries(controls).map(([key,value])=>`.${key}(${value})`).join('')})),
      ...[.25,.5,1].map(wet=>({name:`source-noise-mix-${wet}`,sound:'sawtooth',
        code:`note(69).s('sawtooth').noise(${wet}).gain(.5)`,seedNoise:true})),
      ...['white','pink','brown'].map(sound=>({name:`source-${sound}`,sound,
        code:`note(69).s('${sound}').gain(.5)`,seedNoise:true})),
    ].map(spec=>({cycles:1,cps:.5,window:2,correlation:.99,...spec})),
    { name: 'sine-440', code: `note(69).s('sine').gain(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-fractional-delay', code: `note(69).s('sine').gain(.5).clip(.025).dry(0).delay(1).delaytime(128.5/44100).delayfeedback(.5)`, sound: 'sine', cycles: 1, cps: .5, window: .3, correlation: .99 },
    { name: 'sine-delay-auto-feedback', code: `note("c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime(.05).delayfeedback("0.5 0.75")`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-delay-auto-time', code: `note("c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime("0.05 0.075").delayfeedback(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-delay-auto-zero', code: `note("c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime("0.05 0").delayfeedback(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-delay-auto-return', code: `note("c4 c4 c4").s('sine').gain(.5).clip(.05).dry(0).delay(1).delaytime("0.05 0.075 0.05").delayfeedback("0.5 0.75 0.5")`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    ...['sine','triangle','sawtooth','square'].map(sound=>({name:`static-detune-${sound}`,sound,
      code:`note(69).s('${sound}').gain(.1).clip(.25).detune(700)`,cycles:1,cps:.5,window:2,correlation:.99})),
    {name:'static-detune-sample',sound:'mono_fixture',fixture:true,mono:true,
      code:`note(60).s('mono_fixture').gain(.1).detune(700)`,cycles:1,cps:.5,window:2,correlation:.99},
    ...[
      ['basic','.fm(1).fmh(2)'],
      ['detune','.fm(1).fmh(2).detune(700)'],
      ['envelope','.fm(1).fmh(2).penv(7).patt(.02).pdec(.2).psus(0).panchor(0)'],
      ['vibrato','.fm(.5).fmh(1.5).vib(5).vibmod(.5)'],
      ['fractional','.fm(.5).fmh(2).late(.137)'],
    ].map(([name,controls])=>({name:`fm-frequency-${name}`,sound:'sine',
      code:`note(69).s('sine').gain(.1).clip(.25)${controls}`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['chain','.fm(.5).fmh(2).fm2(.25).fmh2(3)'],
      ['parallel','.fm(.5).fmh(2).fm20(.25).fmh2(3)'],
      ['explicit','.fm30(.25).fmh3(2)'],
      ['modulator-only','.fm2(.5).fmh2(3)'],
      ['cross','.fm(.5).fm13(.25).fm30(.3).fmh3(2)'],
      ['feedback','.fm(.5).fm11(.25)'],
      ['three-stage','.fm(.5).fm2(.25).fm3(.1).fmh2(2).fmh3(3)'],
      ['eighth','.fm80(.25).fmh8(1.5)'],
      ['negative','.fm30(-.25).fmh3(2)'],
      ['negative-index','.fm(-.5).fmh(2)'],
      ['negative-harmonicity','.fm(.5).fmh(-2)'],
      ['cycle','.fm(.5).fm2(.25).fm12(.1)'],
      ['numbered-envelope','.fm(.5).fm2(.25).fmattack2(.02).fmdecay2(.1).fmsustain2(.3)'],
    ].map(([name,controls])=>({name:`fm-matrix-${name}`,sound:'sine',
      code:`note(69).s('sine').gain(.1).clip(.25)${controls}`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['attack','.fmattack(.1)'],
      ['decay','.fmdecay(.2)'],
      ['sustain','.fmsustain(.3)'],
      ['release','.fmrelease(.2).release(.3)'],
      ['linear','.fmattack(.02).fmdecay(.1).fmsustain(.3).fmrelease(.2).fmenv("linear").release(.3)'],
      ['exponential','.fmattack(.02).fmdecay(.1).fmsustain(.3).fmrelease(.2).fmenv("exp").release(.3)'],
      ['short-attack','.fmattack(.8).fmrelease(.2).release(.3)'],
      ['short-decay','.fmattack(.1).fmdecay(.8).fmsustain(0).fmrelease(.2).release(.3)'],
      ['zero-segments','.fmattack(0).fmdecay(0).fmsustain(0).fmrelease(0)'],
    ].map(([name,controls])=>({name:`fm-adsr-${name}`,sound:'sine',
      code:`note(69).s('sine').gain(.1).clip(.25).fm(.5).fmh(2)${controls}`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    { name: 'sine-adsr', code: `note(69).s('sine').gain(.5).attack(.02).decay(.1).sustain(.4).release(.2).clip(.25)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-lowpass', code: `note(69).s('sine').gain(.5).lpf(800).lpq(1)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-pan-center', code: `note(69).s('sine').gain(.5).pan(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-c4-dry', code: `note(60).s('piano').clip(1).gain(.5)`, sound: 'piano', cycles: 1, cps: .5, window: .6, correlation: .99 },
    { name: 'arpoon-without-room', code: arpoon.code.trimEnd() + '.room(0)', sound: 'piano', cycles: 4, cps: .5, window: .6, correlation: .99 },
    { name: 'piano-custom-ir', code: `note(60).s('piano').clip(.25).ir("RolandTR909_sd:0").room(1).roomsize(.2).dry(0)`, sound: 'piano', cycles: 1, cps: .5, window: .6, correlation: .99 },
    { name: 'piano-generated-room-seeded', code: `note(60).s('piano').clip(.25).room(.5).roomsize(1)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99, randomSeed: 0xc0ffee },
    // A real onset at 0.25s avoids the upstream stretcher's negative-onset
    // rejection at zero (its graph schedules 40ms early). No captures shift.
    { name: 'sine-gaincurve', code: `setGainCurve(x=>x*x); note(69).s('sine').gain(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'prebake-glide', code: `note("60 67 64 72").s('sine').gain(.5).glide(.2)`, sound:'sine', cycles:1,cps:.5,window:2,correlation:.99,prebake:true,freshRender:true },
    ...[
      ['defaults',`.penv(12)`],
      ['decay-only',`.pdec(.1)`],
      ['zero-attack',`.penv(-7).patt(0).pdec(.2).psus(0).panchor(0)`],
      ['anchor',`.penv(7).patt(.02).pdec(.1).psus(.3)`],
      ['short',`.penv(12).patt(.2).clip(.025)`],
      ['late',`.note(67).penv(-7).patt(0).pdec(.2).psus(0).panchor(0).late(.25)`],
      ['fractional',`.penv(7).patt(.01).pdec(.2).psus(0).panchor(0).late(.137)`],
      ['vibrato-late',`.vib(5).vibmod(.5).late(.137)`],
      ['vibrato-mixed',`.penv(7).patt(.01).pdec(.2).psus(0).panchor(0).vib(5).vibmod(.5).late(.137)`],
    ].map(([name,controls])=>({name:`pitch-envelope-${name}`,code:`note(69).s('sine').gain(.5)${controls}`,
      sound:'sine',cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['default',`.penv(12)`],
      ['decay',`.penv(24).patt(0).pdec(.3).psus(0).panchor(0)`],
      ['negative',`.penv(-12).patt(.03).pdec(.2).psus(.2).panchor(1)`],
      ['crossing',`.penv(12).patt(.1).pdec(.2).psus(.5).panchor(.5)`],
      ['exact-zero',`.penv(12).patt(.125).pdec(.25).psus(.5).panchor(.5).prel(.2).clip(.25).release(.4)`],
      ['sustain',`.penv(12).patt(.025).pdec(.15).psus(.6).panchor(.3)`],
      ['short-attack',`.penv(12).patt(.5).clip(.025)`],
      ['short-decay',`.penv(12).patt(.01).pdec(.5).psus(.1).clip(.05)`],
      ['release',`.penv(12).patt(.02).pdec(.1).psus(.4).panchor(0).prel(.2).clip(.1).release(.4)`],
      ['fractional',`.penv(7).patt(.01).pdec(.2).psus(0).panchor(0).late(.137)`],
    ].map(([name,controls])=>({name:`pitch-exponential-${name}`,code:`note(69).s('sine').gain(.5).pcurve(1)${controls}`,
      sound:'sine',cycles:1,cps:.5,window:2,correlation:.99})),
    { name: 'prebake-accent', code: `note("60 64").s('sawtooth').lpf(1200).lpenv(2).velocity(.5).accent(2).gain(.4)`, sound:'sawtooth',cycles:1,cps:.5,window:2,correlation:.99,prebake:true },
    { name: 'prebake-delay', code: `note(69).s('sine').gain(.5).clip(.1).dly(.5).delaytime(.075)`,sound:'sine',cycles:1,cps:.5,window:2,correlation:.99,prebake:true },
    { name: 'sine-gaincurve-velocity', code: `setGainCurve(x=>x*x); note(69).s('sine').gain(.5).velocity(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-gaincurve-delay', code: `setGainCurve(x=>x*x); note(69).s('sine').gain(.5).clip(.1).delay(.5).delaytime(.075).delayfeedback(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-custom-default', code: `setDefault('gain',.5); resetDefaultValues(); note(69).s('sine')`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
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
    ...[
      ['up',".stretch(2)"],['down',".stretch(-2)"],
      ['reverse',".speed(-.75).begin(.9).stretch(-2)"],
      ['loop',".stretch(-2).loop(1).loopBegin(.1).loopEnd(.2)"],
      ['envelope',".stretch(2).attack(.03).decay(.08).sustain(.4).release(.12)"],
    ].map(([name,effects])=>({name:`pcm-mono-${name}`,code:`s('mono_fixture')${effects}.clip(.25).late(.137)`,sound:'mono_fixture',fixture:true,mono:true,cycles:1,cps:.5,window:2,correlation:.99})),
    { name: 'pcm-stretch-down-fractional', code: `s('stretch_fixture').speed(.75).stretch(-2).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional', code: `s('stretch_fixture').clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-speed', code: `s('stretch_fixture').speed(.75).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-fast', code: `s('stretch_fixture').speed(2).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-reverse', code: `s('stretch_fixture').speed(-2).begin(.25).clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-speedswitch', code: `s('stretch_fixture').stretch(-2).speed("0.75 1").clip(.25).late(.137)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-delayauto', code: `s('stretch_fixture').gain(.5).clip(.25).late(.137).dry(0).delay(1).delaytime("0.05 0.075").delayfeedback(.5)`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'pcm-onset-fractional-roomswitch', code: `s('stretch_fixture').fast(2).clip(.25).late(.137).room(.5).roomsize("0.5 1")`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: 2, correlation: null, randomSeed: 0xc0ffee }, /* Diagnostic: onsets align exactly. Tails cannot gate here: the eager renderer bounds each hap's capture to its clipped duration (hard zeros past 0.26 s per hap above), so no convolver tail is captured on either side of a switch. Production randomness is unknowable anyway (fresh Math.random plus one chain-ID draw per trigger), and same-orbit repeats reuse the native kernel while the web regenerates. Observed correlation swings 0.982-0.991 across runs with bit-identical captures within each run, so no threshold here would be sound. Native continues one LCG stream across generations (advancing past cache hits) so rooms vary per call deterministically instead of reseeding to identical kernels. */
    { name: 'piano-stretch-reverse', code: `note(60).s('piano').speed(-.5).stretch(1).begin(.98).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-loop', code: `note(60).s('piano').speed(.5).stretch(1).loop(1).loopBegin(.005).loopEnd(.01).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'piano-stretch-envelope-filter', code: `note(60).s('piano').speed(.5).stretch(1).attack(.03).decay(.08).sustain(.4).release(.12).lpf(1600).lpq(.7).clip(.25).late(.125)`, sound: 'piano', cycles: 1, cps: .5, window: 2, correlation: .99 },
    ...['z_sine','bytebeat'].flatMap(sound=>[
      ['envelope','.penv(12).patt(.05).pdec(.2).psus(.25).prel(.1)'],
      ['vibrato','.vib(5).vibmod(.5)'],
      ['detune','.detune(700)'],
    ].map(([name,controls])=>({name:`generated-pitch-${sound}-${name}`,sound,
      code:`note(69).s('${sound}').zrand(0).bbexpr('t*(t>>15^t>>66)').bbst(65536).gain(.1).attack(.01).decay(.04).sustain(.5).release(.1).clip(.25)${controls}`,
      cycles:1,cps:.5,window:2,correlation:.99}))),
    { name: 'zzfx-sine', code: `note(69).s('z_sine').zrand(0).gain(.5).attack(.01).decay(.04).sustain(.5).release(.1).clip(.25)`, sound: 'z_sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'bytebeat', code: `note(60).s('bytebeat').bbexpr('t*(t>>15^t>>66)').bbst(65536).gain(.5).clip(.25).late(.125)`, sound: 'bytebeat', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'saw-440', code: `note(69).s('sawtooth').gain(.5)`, sound: 'sawtooth', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'square-440', code: `note(69).s('square').gain(.5)`, sound: 'square', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'triangle-440', code: `note(69).s('triangle').gain(.5)`, sound: 'triangle', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'gm-epiano1', code: `note(60).s('gm_epiano1').gain(.5).clip(.5)`, sound: 'gm_epiano1', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'gm-acoustic-bass', code: `note(36).s('gm_acoustic_bass').gain(.5).clip(.5)`, sound: 'gm_acoustic_bass', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'gm-violin', code: `note(69).s('gm_violin').gain(.5).clip(.5)`, sound: 'gm_violin', cycles: 1, cps: .5, window: 2, correlation: .99 },
    // Different reverb algorithms and randomized browser impulses: record
    // evidence, without inventing a universal acceptance threshold.
    { name: 'arpoon-opening', code: arpoon.code, sound: 'piano', cycles: 4, cps: .5, window: .6, correlation: null },
  ];
  const report = { browser: version, url: 'https://strudel.cc/', capturedAt: new Date().toISOString(), complete: false,
    nativeBundleSha256: hash(await fs.readFile('js/dist/runtime.js')),
    nativeRendererSha256: hash(await fs.readFile(renderer)),
    nativePackages: JSON.parse(await fs.readFile('js/package.json', 'utf8')).dependencies,
    scheduling: 'browser eager offline path; native sample-frame triggers', cases: [] };
  report.browserScripts = await evaluate(`Promise.all(Array.from(document.scripts).filter(s=>s.src).map(async s=>{
    const bytes=await (await fetch(s.src)).arrayBuffer();
    return {url:s.src,sha256:Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))).map(b=>b.toString(16).padStart(2,'0')).join('')};
  }))`);
  const selectedCases=cases.filter(spec=>!process.env.TOMPOUCE_COMPARE_FILTER || spec.name.includes(process.env.TOMPOUCE_COMPARE_FILTER));
  let starter;
  const prebakeRoot=path.join(out,'prebake-config');
  if(selectedCases.some(spec=>spec.prebake)) {
    const url='https://raw.githubusercontent.com/switchangel/strudel-scripts/706dd24814ac86193ceec6d0b263f432cba6e5ff/prebake.strudel';
    const response=await fetch(url); if(!response.ok)throw Error('Pinned prebake fixture download failed');
    starter=await response.text(); if(Buffer.byteLength(starter)>256*1024)throw Error('Oversize prebake fixture');
    await fs.mkdir(path.join(prebakeRoot,'tompouce'),{recursive:true});
    await fs.writeFile(path.join(prebakeRoot,'tompouce/prebake.strudel'),'// tompouce-prebake: switchangel\n'+starter,{mode:0o600});
    report.prebake={url,sha256:hash(starter)};
  }
  for (const spec of selectedCases) {
    const page = await call('Target.createTarget', { url: 'https://strudel.cc/' }, null);
    session = (await call('Target.attachToTarget', { targetId: page.targetId, flatten: true }, null)).sessionId;
    let initialized = false;
    for (let attempt = 0; attempt < 120; attempt++) {
      initialized = await audioReady();
      if (initialized) break;
      await new Promise(resolve => setTimeout(resolve, 500));
    }
    if (!initialized) throw new Error('Capture page did not initialize');
    await evaluate(transpiler.outputFiles[0].text);
    const request = path.join(out, `${spec.name}.json`), native = path.join(out, `${spec.name}-native.f32`);
    await fs.writeFile(request, JSON.stringify(spec, null, 2));
    const rendered = spawnSync(renderer, [request, native], { encoding: 'utf8', timeout: 120000,
      env:{...nativeEnvironment,...(spec.fixture?{TOMPOUCE_BANKS:fixtureRoot}:{}),...(spec.prebake?{XDG_CONFIG_HOME:prebakeRoot}:{})} });
    if (rendered.status !== 0) throw new Error(rendered.stderr || String(rendered.error));
    const captured = await evaluate(`(async()=>{
      const spec=${JSON.stringify(spec)};
      if(spec.prebake)await evaluate(${JSON.stringify(starter??'')},__comparisonTranspiler.transpiler,{addReturn:false});
      if(spec.fixture){const bytes=Uint8Array.from(atob(spec.mono?${JSON.stringify(monoFixture.toString('base64'))}:${JSON.stringify(fixture.toString('base64'))}),c=>c.charCodeAt(0));await samples({[spec.sound]:[URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}))]})}
      if(spec.fixtureIR){const bytes=Uint8Array.from(atob(${JSON.stringify(monoFixture.toString('base64'))}),c=>c.charCodeAt(0));await samples({[spec.fixtureIR]:[URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}))]})}
      if(spec.randomSeed!==undefined){let seed=spec.randomSeed>>>0;Math.random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296}}
      const {pattern}=await evaluate(spec.code,__comparisonTranspiler.transpiler);
      let selected=pattern.filterValues(v=>v.s===spec.sound);
      const haps=selected.queryArc(0,spec.cycles,{_cps:spec.cps}).filter(h=>h.hasOnset());
      // Metadata must not prime stateful helpers such as glide before PCM
      // rendering. The native renderer consumes its prepared haps once.
      if(spec.freshRender)selected=(await evaluate(spec.code,__comparisonTranspiler.transpiler)).pattern.filterValues(v=>v.s===spec.sound);
      const renderEvents=[];
      if(spec.freshRender) {
        const query=selected.queryArc.bind(selected);
        selected.queryArc=(...args)=>{
          const haps=query(...args);
          renderEvents.push(...haps.filter(h=>h.hasOnset()).map(h=>({begin:Number(h.whole.begin),end:Number(h.whole.end),value:h.value})));
          return haps;
        };
      }
      const original=URL.createObjectURL, click=HTMLAnchorElement.prototype.click;
      let blob;
      URL.createObjectURL=function(value){blob=value;return original.call(URL,value)};
      HTMLAnchorElement.prototype.click=function(){};
      // Use the renderer's supported eager path. Its suspend/resume path
      // races completion for short offline clips in current Chromium.
      const suspend=OfflineAudioContext.prototype.suspend;
      const createBuffer=BaseAudioContext.prototype.createBuffer;
      const random=Math.random;
      const addModule=AudioWorklet.prototype.addModule;
      const seededWorklets=[];
      if(spec.seedSupersaw)AudioWorklet.prototype.addModule=async function(url,...args){
        const text=await(await fetch(url)).text();
        if(!text.includes('supersaw-oscillator'))return addModule.call(this,url,...args);
        seededWorklets.push(Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(text)))).map(b=>b.toString(16).padStart(2,'0')).join(''));
        const seed='let __comparisonSeed=0xc0ffee; Math.random=()=>{__comparisonSeed=(Math.imul(__comparisonSeed,1664525)+1013904223)>>>0;return __comparisonSeed/4294967296};\\n';
        const owned=original.call(URL,new Blob([seed,text],{type:'text/javascript'}));
        try{return await addModule.call(this,owned,...args)}finally{URL.revokeObjectURL(owned)}
      };
      if(spec.seedNoise)BaseAudioContext.prototype.createBuffer=function(channels,length,rate){
        if(channels===1 && length===88200 && rate===44100){
          let seed=0xc0ffee;Math.random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296};
        }
        return createBuffer.call(this,channels,length,rate);
      };
      OfflineAudioContext.prototype.suspend=undefined;
      try {
        await renderPatternAudio(selected,spec.cps,0,spec.cycles,44100,128,false,'comparison');
        if(!blob)throw new Error('No WAV captured');
        const bytes=new Uint8Array(await blob.arrayBuffer());
        let binary='';for(let i=0;i<bytes.length;i+=8192)binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
        if(spec.seedSupersaw && !seededWorklets.length)throw Error('Seeded supersaw worklet was not loaded');
        return {wav:btoa(binary),events:haps.map(h=>({begin:Number(h.whole.begin),end:Number(h.whole.end),value:h.value})),renderEvents,seededWorklets};
      } finally {URL.createObjectURL=original;HTMLAnchorElement.prototype.click=click;OfflineAudioContext.prototype.suspend=suspend;BaseAudioContext.prototype.createBuffer=createBuffer;Math.random=random;AudioWorklet.prototype.addModule=addModule}
    })()`);
    const wav = Buffer.from(captured.wav, 'base64');
    await fs.writeFile(path.join(out, `${spec.name}-browser.wav`), wav);
    await fs.writeFile(path.join(out, `${spec.name}-browser-events.json`), JSON.stringify(captured.events, null, 2));
    if(captured.renderEvents.length)await fs.writeFile(path.join(out,`${spec.name}-browser-render-events.json`),JSON.stringify(captured.renderEvents,null,2));
    const nativeBytes = await fs.readFile(native);
    const metrics = compare(nativeBytes, wav, spec.window);
    const durationMatches = metrics.nativeFrames === Math.round(spec.cycles / spec.cps * 44100) && metrics.nativeFrames === metrics.browserFrames;
    const passed = spec.correlation === null ? null : durationMatches && metrics.correlation >= spec.correlation && metrics.rmsRatio >= .85 && metrics.rmsRatio <= 1.15;
    report.cases.push({ ...spec,...(spec.fixture?{sharedSourceSha256:hash(spec.mono?monoFixture:fixture)}:{}),...(spec.fixtureIR?{sharedIrSha256:hash(monoFixture)}:{}),...(spec.seedSupersaw?{seededWorkletSha256:captured.seededWorklets}:{}),codeSha256: hash(spec.code), nativePcmSha256: hash(nativeBytes), browserWavSha256: hash(wav), metrics, passed });
    await fs.writeFile(path.join(out, 'report.json'), JSON.stringify(report, null, 2));
    console.log(spec.name, JSON.stringify(metrics), passed === null ? 'DIAGNOSTIC' : passed ? 'PASS' : 'FAIL');
    await call('Target.closeTarget', { targetId: page.targetId }, null);
  }
  report.complete = true;
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
