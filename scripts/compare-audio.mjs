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
// The starter's stxt picks wt_digital_echoes for its documented text. The
// vendored public-domain wavetable lets both players use identical bytes.
const wavetableFixture=await fs.readFile('tests/fixtures/uzu-wavetables/wt_echoes.wav');
await fs.mkdir(path.join(fixtureRoot,'uzu-wavetables'),{recursive:true});
await fs.writeFile(path.join(fixtureRoot,'uzu-wavetables/wt_echoes.wav'),wavetableFixture);
await fs.writeFile(path.join(fixtureRoot,'uzu-wavetables/strudel.json'),
  JSON.stringify({wt_digital_echoes:['wt_echoes.wav']}));
const nativeEnvironment={...process.env};
if (!nativeEnvironment.TOMPOUCE_BANKS && !nativeEnvironment.STRUDELTERM_BANKS) {
  for (const candidate of ['assets/banks',path.join(os.homedir(),'.local/lib/tompouce/assets/banks')]) {
    try { await fs.access(path.join(candidate,'dough-samples/piano.json')); nativeEnvironment.TOMPOUCE_BANKS=path.resolve(candidate); break; }
    catch(error) {if(error.code!=='ENOENT')throw error;}
  }
}
const profile = await fs.mkdtemp(path.join(os.tmpdir(), 'tompouce-browser-'));
const bakeryHome = await fs.mkdtemp(path.join(os.tmpdir(), 'tompouce-bakery-native-'));
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
  const bakery = JSON.parse(await fs.readFile('tests/fixtures/strudel-bakery.json', 'utf8'));
  for (const example of bakery.examples)
    if (hash(example.code)!==example.sha256) throw Error(`Bakery source hash mismatch: ${example.id}`);
  const bassFugue = await fs.readFile('bassfugue.json','utf8').then(JSON.parse).catch(()=>null);
  const cases = [
    {name:'voicing-reset-default',code:"setDefaultVoicings('lefthand');resetVoicings();chord('C').voicing()",
      sound:'',cycles:1,cps:.5,window:1,correlation:null,eventsOnly:true},
    ...bakery.examples.map(example=>({name:`bakery-${example.id}`,title:example.title,
      source:example.entries,code:example.code,sound:'',cycles:4,cps:.5,window:1,
      correlation:null,eventsOnly:true})),
    ...(bassFugue?.code?[{name:'bass-fugue-full-mix',
      code:bassFugue.code.replace(/^\s*setcps\([^)]*\);?\s*$/gm,''),sound:'',
      cycles:1,cps:1,window:1,correlation:.99}]:[]),
    ...[
      ['natural',''],['fast',`.speed(8).irspeed(8)`],['reverse',`.speed(-2).irspeed(-2)`],
      ['short-buffer',`.speed(8).irspeed(8).clip(1)`],
      ['pitch',`.penv(7).patt(.02).pdec(.2).psus(0).panchor(0)`],
      ['rate-half',`.speed(.5).irspeed(.5)`],
      ['rate-threequarter',`.speed(.75).irspeed(.75)`],
      ['vibrato',`.vib(5).vibmod(.5)`],
      ['slice',`.begin(.5).end(.75)`],
      ['loop',`.loop(1).loopBegin(.1).loopEnd(.2).clip(.05)`],
      ['fxr',`.speed(8).irspeed(8).fxr(1.5)`],
    ].map(([name,controls])=>({name:`shared-room-lifetime-${name}`,sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',
      code:`s('stretch_fixture').gain(.03)${controls}.ir('mono_fixture').room(1).dry(0)`,cycles:1,cps:.5,window:2,correlation:.99})),
    {name:'shared-room-lifetime-rate-half-source-unity',sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',float32:true,code:`s('stretch_fixture').gain(.03).irspeed(.5).ir('mono_fixture').room(1).dry(0)`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'shared-room-lifetime-rate-half-source-unity-active',sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',code:`s('stretch_fixture').gain(.03).irspeed(.5).ir('mono_fixture').room(1).dry(0)`,cycles:1,cps:.5,window:1.8,correlation:.99},
    {name:'shared-room-single',sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',code:`s('stretch_fixture').gain(.3).clip(.05).ir('mono_fixture').room(1).dry(0)`,cycles:1,cps:.5,window:2,correlation:.99},
    ...[
      ['send-toggle',`.room("1 0 1")`],
      ['postgain-zero',`.room(1).postgain("1 0 1")`],
      ['orbit-switch',`.room(1).orbit("1 2 1")`],
      ['delay-send',`.room(.5).delay(.5).delaytime(.075).delayfeedback(.5)`],
    ].map(([name,controls])=>({name:`shared-room-${name}`,sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',
      code:`s('stretch_fixture').fast(3).gain(.3).clip(.05).ir('mono_fixture')${controls}.dry(0)`,cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['source-pan',`.pan(.7)`],
      ['source-stretch',`.speed(.5)`],
      ['kernel-roomsize',`.roomsize(".3 .8 .3")`],
      ['kernel-size',`.size(".2 .6 .2")`],
    ].map(([name,controls])=>({name:`shared-room-${name}`,sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',
      code:`s('stretch_fixture').fast(3).gain(.3).clip(.05)${controls}.ir('mono_fixture').room(1).dry(0)`,cycles:1,cps:.5,window:2,correlation:.99})),
    {name:'shared-room-graph-source',sound:'sine',fixture:true,fixtureIR:'mono_fixture',
      code:`note(69).s('sine').gain(.4).clip(.05).K(sine(sFreq).mul(.2).out()).ir('mono_fixture').room(1).dry(0)`,
      cycles:1,cps:.5,window:2,correlation:.99},
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
    {name:'shared-delay-graph-source',sound:'sine',
      code:`note(69).s('sine').gain(.4).clip(.05).K(sine(sFreq).mul(.2).out()).delay(.5).delaytime(.075).delayfeedback(.5).dry(0)`,
      cycles:1,cps:.5,window:2,correlation:.99},
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
    {name:'fx-nested-lpf',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(x=>x.lpf(800))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-nested-room',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(x=>x.room(.3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-nested-delay',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(x=>x.delay(.2))`,cycles:1,cps:.5,window:.6,correlation:.99},
    /* Generic .lfo() supports the relative triangle gain path. Cutoff and
       other targets warn and keep their static value; filter-specific
       lp/hp/bp LFOs, vibrato, pulse-width LFO and tremolo are separate. */
    {name:'lfo-generic-cutoff',sound:'sawtooth',code:`note(60).s('sawtooth').lpf(500).lfo({rate:.5,depth:2})`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'lfo-generic-cutoff-steady',sound:'sawtooth',code:`note(60).s('sawtooth').lpf(500).lfo({rate:.5,depth:2})`,cycles:1,cps:.5,window:.55,windowStart:.05,correlation:.99},
    {name:'lfo-generic-gain',sound:'sine',code:`note(69).s('sine').gain(.5).lfo({control:'gain',rate:.5,depth:1})`,cycles:1,cps:.5,window:2,correlation:.99},
                {name:'env-inert-gain',sound:'sine',code:`note(69).s('sine').gain(.5).env({control:'gain',attack:.1,decay:.2,sustain:.5,release:.4})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-nan-gain',sound:'sine',code:`note(69).s('sine').gain(.5).lfo({control:'gain',curve:1.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-nan-gain-high',sound:'sine',code:`note(69).s('sine').gain(.8).lfo({control:'gain',curve:1.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-source-default',sound:'saw',code:`s("saw").lfo().lpf(800)`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-source-only',sound:'saw',code:`s("saw").lfo()`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-source-sine',sound:'sine',code:`s("sine").lfo()`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'tremolo-static',sound:'sine',code:`note(69).s('sine').gain(.5).tremolo(4)`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-tremolorate',sound:'sine',code:`note(69).s('sine').gain(.5).tremolo(4).lfo({control:'tremolo',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-tremolodepth',sound:'sine',code:`note(69).s('sine').gain(.5).tremolo(4).lfo({control:'tremolodepth',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-tremoloskew',sound:'sine',code:`note(69).s('sine').gain(.5).tremolo(4).lfo({control:'tremoloskew',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    /* No tremoloshape PCM case: a generic shape LFO feeds a float index into
       the deployed worklet's waveShapeNames table and silences the voice,
       which aborts the harness ('Silent capture'); native steps the
       integer wave instead (recorded superset, see javascript.md). */
    {name:'lfo-tremolophase-inert',sound:'sine',code:`note(69).s('sine').gain(.5).tremolo(4).lfo({control:'tremolophase',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'vib-static',sound:'sine',code:`note(69).s('sine').gain(.5).vib(5).vibmod(.5)`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-vibmod',sound:'sine',code:`note(69).s('sine').gain(.5).vib(5).vibmod(.5).lfo({control:'vibmod',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'detune-static',sound:'supersaw',seedSupersaw:true,code:`note(69).s('supersaw').gain(.3).detune(.3)`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-detune',sound:'supersaw',seedSupersaw:true,code:`note(69).s('supersaw').gain(.3).detune(.3).lfo({control:'detune',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'vowel-static',sound:'sawtooth',code:`note(69).s('sawtooth').gain(.5).vowel('a')`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-vowel',sound:'sawtooth',code:`note(69).s('sawtooth').gain(.5).vowel('a').lfo({control:'vowel',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'pw-static',sound:'pulse',code:`note(69).s('pulse').gain(.5).pw(.3)`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-pw',sound:'pulse',code:`note(69).s('pulse').gain(.5).pw(.3).lfo({control:'pw',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-pwrate',sound:'pulse',code:`note(69).s('pulse').gain(.5).pw(.3).pwrate(1).pwsweep(.3).lfo({control:'pwrate',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-pwsweep',sound:'pulse',code:`note(69).s('pulse').gain(.5).pw(.3).pwrate(1).pwsweep(.3).lfo({control:'pwsweep',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-coarse',sound:'sine',code:`note(69).s('sine').gain(.5).coarse(4).lfo({control:'coarse',rate:.5,depth:1})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-crush',sound:'sine',code:`note(69).s('sine').gain(.5).crush(4).lfo({control:'crush',rate:.5,depth:1})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-shape',sound:'sine',code:`note(69).s('sine').gain(.5).shape(.5).lfo({control:'shape',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-distort',sound:'sine',code:`note(69).s('sine').gain(.5).distort(2).lfo({control:'distort',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-shapevol',sound:'sine',code:`note(69).s('sine').gain(.5).shape(.5).lfo({control:'shapevol',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-distortvol',sound:'sine',code:`note(69).s('sine').gain(.5).distort(2).lfo({control:'distortvol',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-distorttype',sound:'sine',code:`note(69).s('sine').gain(.5).distort(2).lfo({control:'distorttype',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-tremolosync',sound:'sine',code:`note(69).s('sine').gain(.5).tremolosync(4).lfo({control:'tremolosync',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'phaser-static',sound:'sawtooth',code:`note(69).s('sawtooth').gain(.5).phaserrate(1).phaserdepth(.5)`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-phasercenter',sound:'sawtooth',code:`note(69).s('sawtooth').gain(.5).phaserrate(1).phaserdepth(.5).lfo({control:'phasercenter',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-phaserrate',sound:'sawtooth',code:`note(69).s('sawtooth').gain(.5).phaserrate(1).phaserdepth(.5).lfo({control:'phaserrate',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-phasersweep',sound:'sawtooth',code:`note(69).s('sawtooth').gain(.5).phaserrate(1).phaserdepth(.5).lfo({control:'phasersweep',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-phasedepth',sound:'sawtooth',code:`note(69).s('sawtooth').gain(.5).phaserrate(1).phaserdepth(.5).lfo({control:'phaserdepth',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'compressor-static-tmp',sound:'sine',code:`note(69).s('sine').gain(.5).compressor(-6)`,cycles:1,cps:.5,window:2,correlation:null},
    {name:'compressor-tmp',sound:'sine',code:`note(69).s('sine').gain(.5).compressor(-6).lfo({control:'compressor',rate:.5,depth:6})`,cycles:1,cps:.5,window:2,correlation:null},
    {name:'lfo-postgain',sound:'sine',code:`note(69).s('sine').gain(.5).postgain(.8).lfo({control:'postgain',rate:.5,depth:.5})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-resonance',sound:'sawtooth',code:`note(69).s('sawtooth').lpf(800).resonance(2).lfo({control:'resonance',rate:.5,depth:1})`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'lfo-pan',sound:'sine',code:`note(69).s('sine').gain(.5).pan(.5).lfo({control:'pan',rate:.5,depth:1})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-sample-gain',sound:'mono_fixture',fixture:true,mono:true,code:`s('mono_fixture').gain(.5).clip(.05).lfo({control:'gain',rate:.5,depth:1})`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'lfo-sample-detune',sound:'mono_fixture',fixture:true,mono:true,code:`s('mono_fixture').clip(1).lfo()`,cycles:1,cps:.5,window:2,correlation:.99},
                    {name:'fx-pattern-gain',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(gain(.5))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-lpf',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(lpf(800))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-pan',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(pan(.5))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-coarse',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(coarse(4))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-lpf',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).lpf(1200).FX(lpf(800))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-multi-lpf',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(lpf(800), lpf(1200))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-pan',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).pan(.25).FX(pan(.75))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-coarse',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).coarse(2).FX(coarse(4))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-partials',sound:'saw',code:`note(69).s('saw').clip(.1).FX(partials([1,.5,.25]))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-loop',sound:'stretch_fixture',fixture:true,code:`s('stretch_fixture').gain(.3).clip(.2).FX(loop(1).loopBegin(.1).loopEnd(.2))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-shape',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(shape(.3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-shape',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).shape(.6).FX(shape(.3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-delay',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).delay(.8).delaytime(.1).delayfeedback(.5).dry(0).FX(delay(.3))`,cycles:1,cps:.5,window:1.2,correlation:.99},
    {name:'fx-overlap-source-room',sound:'stretch_fixture',fixture:true,fixtureIR:'mono_fixture',code:`s('stretch_fixture').gain(.03).clip(.05).ir('mono_fixture').room(.8).dry(0).FX(room(.3))`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'fx-overlap-source-room-pan',sound:'mono_fixture',fixture:true,fixtureIR:'mono_fixture',code:`s('mono_fixture').gain(.03).clip(.05).pan(.75).ir('mono_fixture').room(.8).dry(0).FX(room(.3))`,cycles:1,cps:.5,window:2,correlation:.99},
    {name:'fx-source-distort',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).distort(2)`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distort',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-distort',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).distort(2).FX(distort(1))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-source-chorus',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).chorus(.7)`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-chorus',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(chorus(.3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-chorus',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).chorus(.7).FX(chorus(.3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-adsr',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.2).FX(attack(.1).decay(.2).sustain(.5).release(.3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-overlap-source-adsr',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.2).attack(.01).decay(.05).sustain(.2).release(.1).FX(attack(.1).decay(.2).sustain(.5).release(.3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-speed',sound:'stretch_fixture',fixture:true,code:`s('stretch_fixture').gain(.3).clip(.2).FX(speed(.5))`,cycles:1,cps:.5,window:1,correlation:.99},
    {name:'fx-overlap-source-speed',sound:'stretch_fixture',fixture:true,code:`s('stretch_fixture').gain(.3).clip(.2).speed(1).FX(speed(.5))`,cycles:1,cps:.5,window:1,correlation:.99},
    {name:'fx-pattern-note',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(note(72))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-n',sound:'saw',code:`note(69).s('saw').gain(.4).clip(.1).FX(n(3))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-s',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(s('saw'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-bank',sound:'stretch_fixture',fixture:true,code:`s('stretch_fixture').gain(.3).clip(.1).FX(bank('RolandTR909'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-ftype-string',sound:'saw',code:`note(69).s('saw').gain(.4).clip(.1).FX(ftype('24db').lpf(800))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-string',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('fold'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-soft',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('soft'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-scurve',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('scurve'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-hard',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('hard'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-sinefold',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('sinefold'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-cubic',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('cubic'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-diode',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('diode'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-asym',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('asym'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-pattern-distorttype-chebyshev',sound:'sine',code:`note(69).s('sine').gain(.4).clip(.1).FX(distort(1).distorttype('chebyshev'))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-graph-source-lpf',sound:'sine',code:`note(69).s('sine').gain(.3).clip(.1).K(sine(sFreq).mul(.2).out()).FX(x=>x.lpf(200))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-graph-out-channels',sound:'sine',code:`note(69).s('sine').gain(.3).clip(.1).K(sine(sFreq).mul(.2).out(0).mul(.5).out(1)).FX(x=>x.gain(.5))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-worklet-effect-gain',sound:'sine',code:`note(69).s('sine').gain(.3).clip(.1).worklet('audioin().delay(.05).out()').FX(x=>x.gain(.5))`,cycles:1,cps:.5,window:.6,correlation:.99},
    {name:'fx-worklet-effect-tail',sound:'sine',code:`note(69).s('sine').gain(.3).clip(.1).worklet('audioin().delay(.1).out()').fxr(.4).FX(x=>x.gain(.5))`,cycles:1,cps:.5,window:.8,correlation:.99},


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
      ['triangle','.fmwave("triangle")'],
      ['square','.fmwave("square")'],
      ['sawtooth','.fmwave("sawtooth")'],
      ['operator','.fm(.3).fm2(.4).fmwave2("sawtooth").fmh2(3)'],
      ['envelope','.fmwave("square").fmattack(.02).fmdecay(.1).fmsustain(.3).fmrelease(.15).release(.3)'],
    ].map(([name,controls])=>({name:`fm-wave-${name}`,sound:'sine',
      code:`note(69).s('sine').gain(.1).clip(.25).fm(.4).fmh(2)${controls}`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    ...['white','pink','brown'].map(wave=>({name:`fm-noise-onset-${wave}-direct`,sound:'sine',seedNoise:true,
      code:`note(69).s('sine').gain(.1).clip(.25).fm(.15).fmwave('${wave}').fmattack(.04).fmdecay(.08).fmsustain(.25).late(.137)`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    ...['white','pink','brown'].flatMap(wave=>[
      ['aligned',0],['half-quantum',32/44100],['fractional',.137],
    ].map(([name,onset])=>({name:`fm-noise-onset-${wave}-${name}`,sound:'sine',seedNoise:true,
      code:`note(69).s('sine').gain(.1).clip(.25).fm(.5).fmh(2).fm2(.3).fmh2(3).fmwave2('${wave}').fmattack2(.04).fmdecay2(.08).fmsustain2(.25).late(${onset})`,
      cycles:1,cps:.5,window:2,correlation:.99}))),
    ...['white','pink','brown'].map(wave=>({name:`fm-wave-${wave}`,sound:'sine',seedNoise:true,
      code:`note(69).s('sine').gain(.1).clip(.25).fm(.15).fmh(1).fmwave("${wave}")`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['adsr','.fmattack(.05).fmdecay(.1).fmsustain(.2).fmrelease(.15).release(.3).late(.137)'],
      ['linear','.fmattack(.02).fmdecay(.08).fmsustain(.4).fmenv("linear").late(64/44100)'],
      ['numbered','.fm2(.3).fmh2(3).fmattack2(.04).fmdecay2(.08).fmsustain2(.25).late(.137)'],
      ['chain','.fm2(.25).fmh2(3).late(.137)'],
      ['aligned','.fm2(.25).fmattack2(.02).fmdecay2(.1).fmsustain2(.3).late(64/44100)'],
      ['pitch','.fmattack(.03).fmdecay(.1).fmsustain(.2).penv(7).patt(.02).pdec(.1).psus(0).panchor(0).late(.137)'],
    ].map(([name,controls])=>({name:`fm-onset-${name}`,sound:'sine',
      code:`note(69).s('sine').gain(.1).clip(.25).fm(.5).fmh(2)${controls}`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['three','.fm(.4).fm2(.25).fm3(.2).fmh2(2).fmh3(3).fm13(.15)'],
      ['self','.fm(.4).fm2(.3).fmh2(2).fm22(.2)'],
      ['mutual','.fm(.35).fm2(.3).fmh2(1.5).fm12(.12).fm21(-.08)'],
      ['eighth','.fm(.3).fm8(.2).fmh8(1.5).fm81(.1)'],
    ].map(([name,controls])=>({name:`fm-cycle-${name}`,sound:'sine',
      code:`note(69).s('sine').gain(.1).clip(.25)${controls}`,
      cycles:1,cps:.5,window:2,correlation:.99})),
    ...[
      ['harmonicity',`note(69).s('sine').gain(.1).clip(.25).fm(.25).fmh(64)`],
      ['high-note',`note(108).s('sine').gain(.1).clip(.25).fm(.4).fmh(12)`],
      ['index',`note(69).s('sine').gain(.1).clip(.25).fm(24).fmh(3)`],
      ['negative',`note(69).s('sine').gain(.1).clip(.25).fm(.4).fmh(-48)`],
      ['low',`note(12).s('sine').gain(.1).clip(.25).fm(1.5).fmh(.5)`],
    ].map(([name,code])=>({name:`fm-range-${name}`,sound:'sine',code,
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
    { name: 'prebake-trancegate', code: `let qaSeed=0xc0ffee; Math.random=()=>((qaSeed=(Math.imul(qaSeed,1664525)+1013904223)>>>0)/4294967296); note("c3*8").s('sawtooth').gain(.5).trancegate(.4,3,1)`, sound:'sawtooth',cycles:1,cps:.5,window:2,correlation:.99,prebake:true,freshRender:true },
    { name: 'prebake-tgate', code: `note("c3*8").s('sawtooth').gain(.5).tgate(.5,2,1)`, sound:'sawtooth',cycles:1,cps:.5,window:2,correlation:.99,prebake:true },
    { name: 'prebake-acid', code: `note('c3').s('sine').gain(.5).acid()`, sound:'supersaw',seedSupersaw:true,cycles:1,cps:.5,window:2,correlation:.99,prebake:true },
    { name: 'prebake-stxt-wavetable', code: `note(60).gain(.5).stxt('tompouce')`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:2,correlation:.99,prebake:true,randomSeed:0xc0ffee,awaitRooms:true },
    { name: 'wt-sample-plain', code: `s('wt_digital_echoes').note(60).gain(.5)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-fixed', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.627).wtrate(2.137).wtdepth(.831)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-lfo-sine', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.3).wtrate(2).wtdepth(.4).wtshape('sine')`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-warp', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.35).warp(.6).warpmode('bendp')`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-unison', code: `s('wt_digital_echoes').note(60).gain(.5).unison(3).detune(.5).spread(.4).wtphaserand(0)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-late', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.5).clip(.25).late(.137)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:2,correlation:.99 },
    { name: 'wt-sample-sync', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.2).wtsync(.5).wtdepth(.4)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-wtrate', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.3).wtrate(2).wtdepth(.4).wtshape('sine').lfo({control:'wtrate',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-wtdepth', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.3).wtrate(2).wtdepth(.4).wtshape('sine').lfo({control:'wtdepth',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-wtskew', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.3).wtrate(2).wtdepth(.4).wtshape('sine').lfo({control:'wtskew',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-wtdc-inert', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.3).wtrate(2).wtdepth(.4).wtshape('sine').lfo({control:'wtdc',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wtdc-static', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.3).wtrate(2).wtdepth(.4).wtshape('sine').wtdc(.3)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-warprate', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.4).warprate(2).warpdepth(.3).warpshape('sine').warpmode('asym').lfo({control:'warprate',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-warpdepth', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.4).warprate(2).warpdepth(.3).warpshape('sine').warpmode('asym').lfo({control:'warpdepth',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-warpskew', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.4).warprate(2).warpdepth(.3).warpshape('sine').warpmode('asym').lfo({control:'warpskew',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'lfo-warpdc-inert', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.4).warprate(2).warpdepth(.3).warpshape('sine').warpmode('asym').lfo({control:'warpdc',rate:.5,depth:.5})`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'warpdc-static', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.4).warprate(2).warpdepth(.3).warpshape('sine').warpmode('asym').warpdc(.3)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'warpdc-static-neg', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.4).warprate(2).warpdepth(.3).warpshape('sine').warpmode('asym').warpdc(-0.25)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-wtenv', code: `s('wt_digital_echoes').note(60).gain(.5).wt(.1).wtenv(.6).wtattack(.05).wtdecay(.1).wtsustain(.3).wtrelease(.2).clip(.5)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:2,correlation:.99 },
    { name: 'wt-sample-warpenv', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.1).warpenv(.6).warpattack(.05).warpdecay(.1).warpsustain(.3).warprelease(.2).warpmode('asym').clip(.5)`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:2,correlation:.99 },
    { name: 'wt-sample-warplfo', code: `s('wt_digital_echoes').note(60).gain(.5).warp(.4).warprate(2).warpdepth(.3).warpshape('sine').warpmode('asym')`, sound:'wt_digital_echoes',wavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-phaserand', code: `s('wt_digital_echoes').note(60).gain(.5).unison(3).detune(.5).spread(.4).wtphaserand(.75)`, sound:'wt_digital_echoes',wavetable:true,seedWavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-phaserand-default', code: `s('wt_digital_echoes').note(60).gain(.5).unison(3).detune(.5).spread(.4)`, sound:'wt_digital_echoes',wavetable:true,seedWavetable:true,cycles:1,cps:.5,window:1,correlation:.99 },
    { name: 'wt-sample-phaserand-seq', code: `s('wt_digital_echoes').note("60 64").gain(.5).unison(3).detune(.5).spread(.4)`, sound:'wt_digital_echoes',wavetable:true,seedWavetable:true,cycles:1,cps:.5,window:2,correlation:.99 },
    { name: 'diag-wt-phaserand-live', code: `s('wt_digital_echoes').note(60).gain(.5).unison(3).detune(.5).spread(.4)`, sound:'wt_digital_echoes',wavetable:true,liveRandom:true,cycles:1,cps:.5,window:1,correlation:null },
    { name: 'prebake-humanize', code: `let qaSeed=0xc0ffee; Math.random=()=>((qaSeed=(Math.imul(qaSeed,1664525)+1013904223)>>>0)/4294967296); note("60*4").s('sine').gain(.5).humanize(.5)`, sound:'sine',cycles:1,cps:.5,window:2,correlation:.99,prebake:true,freshRender:true },
    { name: 'prebake-glitch', code: `let qaSeed=0xc0ffee; Math.random=()=>((qaSeed=(Math.imul(qaSeed,1664525)+1013904223)>>>0)/4294967296); note("60 64").s('sine').gain(.5).pan(.5).glitch(.25)`, sound:'sine',cycles:1,cps:.5,window:2,correlation:.99,prebake:true,freshRender:true },
    { name: 'prebake-fmtime', code: `note("60*4").s('sine').gain(.5).fmtime(2,4)`, sound:'sine',cycles:1,cps:.5,window:2,correlation:.99,prebake:true },
    { name: 'prebake-strum', code: `note("c3,e3,g3").s('sine').gain(.5).strum(.05)`, sound:'sine',cycles:1,cps:.5,window:2,correlation:.99,prebake:true },
    { name: 'prebake-volume-half', code: `note(69).s('sine').gain(.5)`, sound:'sine',cycles:1,cps:.5,window:2,correlation:.99,prebake:true,volume:.5 },
    { name: 'sine-gaincurve-velocity', code: `setGainCurve(x=>x*x); note(69).s('sine').gain(.5).velocity(.5)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
    { name: 'sine-velocity-above-one', code: `note(69).s('sine').gain(.4).velocity(2)`, sound: 'sine', cycles: 1, cps: .5, window: 2, correlation: .99 },
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
    { name: 'pcm-onset-fractional-roomswitch', code: `s('stretch_fixture').fast(2).clip(.25).late(.137).room(.5).roomsize("0.5 1")`, sound: 'stretch_fixture', fixture:true, float32:true, awaitRooms:true, cycles: 1, cps: .5, window: 2, correlation: .99, randomSeed: 0xc0ffee }, /* Full switch gates because awaitRooms waits for generated room buffers before rendering. */
    { name: 'pcm-onset-fractional-roomswitch-first', code: `s('stretch_fixture').fast(2).clip(.25).late(.137).room(.5).roomsize("0.5 1")`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: .35, correlation: .99, randomSeed: 0xc0ffee },
    { name: 'pcm-onset-fractional-roomswitch-second', code: `s('stretch_fixture').fast(2).clip(.25).late(.137).room(.5).roomsize("0.5 1")`, sound: 'stretch_fixture', fixture:true, cycles: 1, cps: .5, window: .35, windowStart: 1.137, correlation: .99, randomSeed: 0xc0ffee },
    { name: 'diag-ir-tail', code: `s('stretch_fixture').gain(1).clip(.0001).ir('mono_fixture').room(1).dry(0).irspeed(.5)`, sound: 'stretch_fixture', fixture:true, fixtureIR:'mono_fixture', cycles: 1, cps: .5, window: 2, correlation: null }, /* Diagnostic: a 0.2 ms source isolates the sub-unity IR tail. It matches native at 0.99918/1.032 over the full 2 s, so the IR resampler and edge are not the residual. */
    { name: 'shared-room-lifetime-rate-half-source-unity-gain', code: `s('stretch_fixture').gain(1).ir('mono_fixture').room(1).dry(0).irspeed(.5)`, sound: 'stretch_fixture', fixture:true, fixtureIR:'mono_fixture', cycles: 1, cps: .5, window: 2, correlation: .99 }, /* Same sub-unity IR path as -source-unity at a level above the 16-bit capture floor. */
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
    { name: 'arpoon-opening', code: arpoon.code, sound: 'piano', cycles: 4, cps: .5, window: .6, correlation: .99, randomSeed: 0xc0ffee },
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
    const request = path.join(out, `${spec.name}.json`), native = path.join(out, `${spec.name}-native${spec.eventsOnly?'-events.json':'.f32'}`);
    await fs.writeFile(request, JSON.stringify(spec, null, 2));
    const prebakeSource = spec.prebake ? (spec.volume===undefined ? starter :
      starter.replace(/'Volume',\s*\.8/, `'Volume', ${spec.volume}`)) : null;
    if (prebakeSource)
      await fs.writeFile(path.join(prebakeRoot,'tompouce/prebake.strudel'),
        '// tompouce-prebake: switchangel\n'+prebakeSource,{mode:0o600});
    const rendered = spawnSync(renderer, [request, native], { encoding: 'utf8', timeout: 120000,
      env:{...nativeEnvironment,...(spec.fixture||spec.wavetable?{TOMPOUCE_BANKS:fixtureRoot}:{}),...(spec.prebake?{XDG_CONFIG_HOME:prebakeRoot}:{XDG_CONFIG_HOME:bakeryHome}),
        ...(spec.eventsOnly?{HOME:bakeryHome,XDG_CONFIG_HOME:bakeryHome,XDG_CACHE_HOME:bakeryHome}:{})} });
    if (spec.eventsOnly) {
      const result={...spec,codeSha256:hash(spec.code),mode:'events',passed:false};
      if (rendered.status!==0) {
        result.stage='native'; result.error=(rendered.stderr||String(rendered.error)).slice(-16000);
      } else {
        try {
          const nativeEvents=JSON.parse(await fs.readFile(native,'utf8'));
          const browserEvents=await evaluate(`(async()=>{
            const context=new OfflineAudioContext(2,44100,44100);
            setAudioContext(context);
            const host=repl({getTime:()=>0,onSchedule:()=>{},setInterval:()=>1,clearInterval:()=>{},
              transpiler:__comparisonTranspiler.transpiler});
            await host.evaluate(${JSON.stringify(spec.code)},false);
            if(host.state.evalError)throw host.state.evalError;
            const pattern=host.scheduler.pattern;
            if(!pattern)throw Error('Composition did not return a pattern');
            const events=[];
            for(let begin=0;begin<${spec.cycles};begin++)
              events.push(...pattern.queryArc(begin,begin+1,{_cps:host.scheduler.cps})
                .filter(h=>h.hasOnset()).map(h=>({begin:Number(h.whole.begin),end:Number(h.whole.end),value:h.value})));
            host.scheduler.stop();
            return {events,cps:host.scheduler.cps};
          })()`);
          await fs.writeFile(path.join(out,`${spec.name}-browser-events.json`),JSON.stringify(browserEvents,null,2));
          result.cps=nativeEvents.cps;
          result.browserCps=browserEvents.cps;
          result.nativeEvents=nativeEvents.events.length; result.browserEvents=browserEvents.events.length;
          result.passed=Math.abs(nativeEvents.cps-browserEvents.cps)<1e-7 && compareEvents(nativeEvents.events,browserEvents.events);
          result.stage=result.passed?(browserEvents.events.length?'events-match':'empty-pattern'):'events-differ';
        } catch(error) { result.stage='browser'; result.error=String(error).slice(-16000); }
      }
      report.cases.push(result);
      await fs.writeFile(path.join(out,'report.json'),JSON.stringify(report,null,2));
      console.log(spec.name,result.stage,result.nativeEvents??'',result.browserEvents??'');
      await call('Target.closeTarget',{targetId:page.targetId},null);
      continue;
    }
    if (rendered.status !== 0) throw new Error(rendered.stderr || String(rendered.error));
    const captured = await evaluate(`(async()=>{
      const spec=${JSON.stringify(spec)};
      if(spec.prebake)await evaluate(${JSON.stringify(prebakeSource??'')},__comparisonTranspiler.transpiler,{addReturn:false});
      if(spec.fixture){const bytes=Uint8Array.from(atob(spec.mono?${JSON.stringify(monoFixture.toString('base64'))}:${JSON.stringify(fixture.toString('base64'))}),c=>c.charCodeAt(0));await samples({[spec.sound]:[URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}))]})}
      if(spec.fixtureIR){const bytes=Uint8Array.from(atob(${JSON.stringify(monoFixture.toString('base64'))}),c=>c.charCodeAt(0));await samples({[spec.fixtureIR]:[URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}))]})}
      if(spec.wavetable){const bytes=Uint8Array.from(atob(${JSON.stringify(wavetableFixture.toString('base64'))}),c=>c.charCodeAt(0));await samples({wt_digital_echoes:[URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}))]})}
      if(spec.randomSeed!==undefined){let seed=spec.randomSeed>>>0;Math.random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed/4294967296}}
      const {pattern}=await evaluate(spec.code,__comparisonTranspiler.transpiler);
      let selected=spec.sound?pattern.filterValues(v=>v.s===spec.sound):pattern;
      const haps=selected.queryArc(0,spec.cycles,{_cps:spec.cps}).filter(h=>h.hasOnset());
      // Metadata must not prime stateful helpers such as glide before PCM
      // rendering. The native renderer consumes its prepared haps once.
      if(spec.freshRender){const fresh=(await evaluate(spec.code,__comparisonTranspiler.transpiler)).pattern;selected=spec.sound?fresh.filterValues(v=>v.s===spec.sound):fresh;}
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
      const originalConvolver=BaseAudioContext.prototype.createConvolver;
      const roomContexts=new WeakMap();
      if(spec.awaitRooms)BaseAudioContext.prototype.createConvolver=function(){
        const convolver=originalConvolver.call(this);
        const list=roomContexts.get(this)||[];
        list.push(convolver);
        roomContexts.set(this,list);
        return convolver;
      };
      // Use the renderer's supported eager path. Its suspend/resume path
      // races completion for short offline clips in current Chromium.
      const suspend=OfflineAudioContext.prototype.suspend;
      const createBuffer=BaseAudioContext.prototype.createBuffer;
      const random=Math.random;
      const addModule=AudioWorklet.prototype.addModule;
      AudioWorklet.prototype.addModule=async function(url,...args){
        const text=await(await fetch(url)).text();
        if(text.includes('distort-processor'))window.__distortModule=text;
        return addModule.call(this,url,...args);
      };
      const seededWorklets=[];
      let seededWavetable=false;
      if(spec.seedSupersaw||spec.seedWavetable)AudioWorklet.prototype.addModule=async function(url,...args){
        const text=await(await fetch(url)).text();
        const supersaw=spec.seedSupersaw&&text.includes('supersaw-oscillator');
        const wavetable=spec.seedWavetable&&text.includes('wavetable-oscillator-processor');
        if(!supersaw&&!wavetable)return addModule.call(this,url,...args);
        if(wavetable)seededWavetable=true;
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
      const originalStart=OfflineAudioContext.prototype.startRendering;
      let renderedBuffer=null;
      const expectedFrames=Math.round(spec.cycles/spec.cps*44100);
      if(spec.float32||spec.awaitRooms)OfflineAudioContext.prototype.startRendering=function(){
        const ctx=this;
        return (async()=>{
          if(spec.awaitRooms && roomContexts.has(ctx)){
            const list=roomContexts.get(ctx);
            for(let i=0;i<2000 && list.some(c=>!c.buffer);i++)
              await new Promise(resolve=>setTimeout(resolve,1));
          }
          const pending=originalStart.call(ctx);
          if(spec.float32)pending.then(buffer=>{
            if(!renderedBuffer && buffer.length===expectedFrames &&
               buffer.numberOfChannels===2) renderedBuffer=buffer;
          }).catch(()=>{});
          return pending;
        })();
      };
      try {
        await renderPatternAudio(selected,spec.cps,0,spec.cycles,44100,128,false,'comparison');
        if(!blob)throw new Error('No WAV captured');
        const bytes=new Uint8Array(await blob.arrayBuffer());
        let binary='';for(let i=0;i<bytes.length;i+=8192)binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
        let wav32='';
        if(spec.float32){
          if(!renderedBuffer)throw new Error('No float buffer captured');
          const channels=renderedBuffer.numberOfChannels,frames=renderedBuffer.length;
          const pcm=new Uint8Array(44+frames*channels*4),view=new DataView(pcm.buffer);
          const put=(offset,text)=>{for(let i=0;i<text.length;i++)view.setUint8(offset+i,text.charCodeAt(i));};
          put(0,'RIFF');view.setUint32(4,36+frames*channels*4,true);put(8,'WAVE');
          put(12,'fmt ');view.setUint32(16,16,true);view.setUint16(20,3,true);
          view.setUint16(22,channels,true);view.setUint32(24,44100,true);
          view.setUint32(28,44100*channels*4,true);view.setUint16(32,channels*4,true);
          view.setUint16(34,32,true);put(36,'data');view.setUint32(40,frames*channels*4,true);
          const data=[];for(let c=0;c<channels;c++)data.push(renderedBuffer.getChannelData(c));
          let offset=44;
          for(let i=0;i<frames;i++)for(let c=0;c<channels;c++){view.setFloat32(offset,data[c][i],true);offset+=4;}
          let encoded='';for(let i=0;i<pcm.length;i+=8192)encoded+=String.fromCharCode(...pcm.subarray(i,i+8192));
          wav32=btoa(encoded);
        }
        if(spec.seedSupersaw && !seededWorklets.length)throw Error('Seeded supersaw worklet was not loaded');
        if(spec.seedWavetable && !seededWavetable)throw Error('Seeded wavetable worklet was not loaded');
        return {wav:btoa(binary),wav32,events:haps.map(h=>({begin:Number(h.whole.begin),end:Number(h.whole.end),value:h.value})),renderEvents,seededWorklets};
      } finally {URL.createObjectURL=original;HTMLAnchorElement.prototype.click=click;OfflineAudioContext.prototype.suspend=suspend;OfflineAudioContext.prototype.startRendering=originalStart;BaseAudioContext.prototype.createBuffer=createBuffer;BaseAudioContext.prototype.createConvolver=originalConvolver;Math.random=random;AudioWorklet.prototype.addModule=addModule}
    })()`);
    const wav = Buffer.from(captured.wav, 'base64');
    const wav32 = captured.wav32 ? Buffer.from(captured.wav32, 'base64') : null;
    const distortModule = await evaluate('window.__distortModule||""');
    if (distortModule) await fs.writeFile('/tmp/opencode/distort-worklet.js', distortModule);
    await fs.writeFile(path.join(out, `${spec.name}-browser.wav`), wav);
    if (wav32) await fs.writeFile(path.join(out, `${spec.name}-browser32.wav`), wav32);
    await fs.writeFile(path.join(out, `${spec.name}-browser-events.json`), JSON.stringify(captured.events, null, 2));
    if(captured.renderEvents.length)await fs.writeFile(path.join(out,`${spec.name}-browser-render-events.json`),JSON.stringify(captured.renderEvents,null,2));
    const nativeBytes = await fs.readFile(native);
    const browserBytes = spec.float32 && wav32 ? wav32 : wav;
    const metrics = compare(nativeBytes, browserBytes, spec.window, spec.windowStart ?? 0);
    const durationMatches = metrics.nativeFrames === Math.round(spec.cycles / spec.cps * 44100) && metrics.nativeFrames === metrics.browserFrames;
    const passed = spec.correlation === null ? null : durationMatches && metrics.correlation >= spec.correlation && metrics.rmsRatio >= .85 && metrics.rmsRatio <= 1.15;
    report.cases.push({ ...spec,...(spec.fixture?{sharedSourceSha256:hash(spec.mono?monoFixture:fixture)}:{}),...(spec.fixtureIR?{sharedIrSha256:hash(monoFixture)}:{}),...(spec.seedSupersaw?{seededWorkletSha256:captured.seededWorklets}:{}),codeSha256: hash(spec.code), nativePcmSha256: hash(nativeBytes), browserWavSha256: hash(browserBytes), metrics, passed });
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
  await fs.rm(bakeryHome, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
}

function hash(bytes) { return createHash('sha256').update(bytes).digest('hex'); }
function compareEvents(native,browser) {
  const normalize=value=>Array.isArray(value)?value.map(normalize):
    value && typeof value==='object'?Object.fromEntries(Object.keys(value).sort().map(k=>[k,normalize(value[k])])):value;
  const ordered=events=>events.map(({begin,end,value})=>normalize({begin,end,value}))
    .sort((a,b)=>a.begin-b.begin || a.end-b.end || JSON.stringify(a.value).localeCompare(JSON.stringify(b.value)));
  const equal=(a,b)=>{
    if(typeof a==='number' && typeof b==='number')return Number.isFinite(a) && Number.isFinite(b) && Math.abs(a-b)<=1e-7*Math.max(1,Math.abs(b));
    if(a===b)return true;
    if(!a || !b || typeof a!=='object' || typeof b!=='object')return false;
    const keys=Object.keys(a);return keys.length===Object.keys(b).length && keys.every(k=>Object.hasOwn(b,k) && equal(a[k],b[k]));
  };
  return equal(ordered(native),ordered(browser));
}
function compare(native, wav, seconds, start = 0) {
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
  const first = Math.round(start * rate);
  const samples = Math.round(seconds * rate) * channels;
  const nativeOffset = first * channels * 4;
  const browserOffset = first * channels * (bits / 8);
  if (native.length < nativeOffset + samples * 4 || data.length < browserOffset + samples * bits / 8) throw new Error('Capture too short');
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
    const x = native.readFloatLE(nativeOffset + i * 4), y = format === 1 ? data.readInt16LE(browserOffset + i * 2) / 32768 : data.readFloatLE(browserOffset + i * 4);
    if (!Number.isFinite(x) || !Number.isFinite(y)) throw new Error('Non-finite audio');
    xy += x*y; xx += x*x; yy += y*y; error += (x-y)**2;
  }
  if (!xx || !yy) throw new Error('Silent capture');
  return { correlation: xy / Math.sqrt(xx*yy), rmsRatio: Math.sqrt(xx/yy), rmsError: Math.sqrt(error/samples),
    windowSeconds: seconds, windowStart: start, lagFrames: 0, nativePeak,browserPeak,
    nativeFrames: native.length / 8, browserFrames: data.length / (bits / 8) / 2 };
}
