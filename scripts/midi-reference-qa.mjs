/* Dominant MIDI extras against the deployed web REPL: hap value shapes from
   live pattern queries and real WebMidi sends to an owned fake transport,
   compared with native commit and consumer-send decisions. Only the raw
   transport and clock mapping are replaced; deployed scheduling, validation
   and byte conversion run unchanged. Note-on/off timestamp differences gate
   each duration; physical device delivery latency stays outside this gate. A deployed
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
  {name:'channels-note',code:'note(60).midi({midichannel:[1,3]})'},
  {name:'channels-cc',code:'note(60).ccn(74).ccv(.5).midi({midichannel:[1,3]})'},
  {name:'channels-controller',code:'ccn(74).ccv(.5).midi({midichannel:[3,1],isController:true})'},
  {name:'channels-extras',code:'note(60).progNum(10).ccn(74).ccv(.5).nrpnn([1,2]).nrpv(64).midibend(.25).miditouch(.75).midi({midichannel:[3,1]})'},
  {name:'channels-all-extras',code:"note(60).progNum(10).ccn(74).ccv(.5).nrpnn([1,2]).nrpv(64).midibend(.25).miditouch(.75).midi({midichannel:'all'})"},
  {name:'channels-global',code:"note(60).sysex([67,[1,2]]).midicmd('stop').midi({midichannel:[3,1]})"},
  {name:'channels-duplicates',code:'note(60).midi({midichannel:[3,1,3]})'},
  {name:'channels-empty',code:'note(60).ccn(74).ccv(.5).midi({midichannel:[]})'},
  {name:'channels-invalid',code:'note(60).ccn(74).ccv(.5).midi({midichannel:[0,2,17]})'},
  {name:'channels-fractional',code:'note(60).midi({midichannel:[1.5,2]})'},
  {name:'channels-strings',code:"note(60).midi({midichannel:['2',3]})"},
  {name:'channels-all',code:"note(60).midi({midichannel:'all'})"},
  {name:'channels-override',code:'note(60).midichan(4).midi({midichannel:[1,3]})'},
  {name:'channels-options-order',code:'note(60).midi({midichannel:[1,3]},{midichannel:[4,2]})'},
  ...[
    ['channels-array-all',"note(60).midi({midichannel:[3,'all',1]})"],
    ['channels-none',"note(60).midi({midichannel:'none'})"],
    ['channels-null','note(60).midi({midichannel:null})'],
    ['channels-zero','note(60).midi({midichannel:0})'],
    ['channels-undefined','note(60).midi({midichannel:undefined})'],
    ['channels-prefix-strings',"note(60).midi({midichannel:['2x','0xA',' 3 ']})"],
    ['channels-nonfinite','note(60).midi({midichannel:[NaN,Infinity,-1,16,17]})'],
    ['channels-midi-pattern','note(60).midichan([1,3]).midi()'],
    ['channels-post-program',"note(60).midicmd(['progNum',11]).midi({midichannel:[3,1]})"],
    ['channels-mapped',"defaultmidimap({lpf:74,pan:10}); note(60).lpf(.5).pan(.25).midi({midichannel:[3,1]})"],
    ['channels-max-mapped',"defaultmidimap(Object.fromEntries(Array.from({length:32},(_,i)=>['qa'+i,i]))); note(60).set(Object.fromEntries(Array.from({length:32},(_,i)=>['qa'+i,.5]))).midi({midichannel:'all'})"],
    ['channels-short','note(60).clip(.0001).midi({midichannel:[3,1]})'],
  ].map(([name,code])=>({name,code,...(code.includes(';')?{webcode:`(async()=>{${code.slice(0,code.lastIndexOf(';'))};return ${code.slice(code.lastIndexOf(';')+1)}})()`}:{})})),
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
  {name: 'zero-velocity', code: 'note(60).velocity(0).midi()',
    events: [{chan: 0, note: 60, cycle_start: 1}]},
  {name: 'invalid-velocity', code: 'note(60).gain(2).ccn(74).ccv(0.5).midi()', events: []},
  {name: 'local-gain-stages', code: 'note(60).amp(2).busgain(2).overgain(2).midi()',
    events: [{chan: 0, note: 60, cycle_start: 1}]},
  {name: 'send-order', code: 'note(60).progNum(10).sysex([67,[1,2]]).ccn(74).ccv(0.5).midibend(0.25).miditouch(0.75).midi()',
    events: [{chan: 0, note: 60, prog: 10, sysex_id: [67], sysex: [1,2], cc: 74, ccv: 64,
      bend: 2047, touch: 95, cycle_start: 1}]},
  {name:'controller',code:'note(60).ccn(74).ccv(.5).midi({isController:true})',
    events:[{chan:0,note:-1,cc:74,ccv:64,cycle_start:1}]},
  {name:'options',code:'note(60).midi({midichannel:2,velocity:.2,gain:.5},{midichannel:3,velocity:.8})',
    events:[{chan:2,note:60,cycle_start:1}]},
  {name:'options-overridden',code:'note(60).velocity(.5).gain(.5).midichan(4).midi({midichannel:2,velocity:.2,gain:.2})',
    events:[{chan:3,note:60,cycle_start:1}]},
  {name:'named-cc',code:"note(60).ccn('brightness').ccv(.5).midi()",
    events:[{chan:0,note:60,cc:74,ccv:64,cycle_start:1}]},
  {name:'unknown-cc',code:"note(60).ccn('noSuchController').ccv(.5).midibend(.5).midi()",
    events:[{chan:0,note:60}]},
  {name:'nrpn',code:'note(60).nrpnn([1,2]).nrpv(64).midibend(.5).midi()',
    events:[{chan:0,note:60,bend:4095,cycle_start:1}]},
  {name:'nrpn-bad',code:'note(60).nrpnn([128,2]).nrpv(64).midibend(.5).midi()',
    events:[{chan:0,note:60}]},
  {name:'duplicate-program',code:"note(60).progNum(10).midicmd(['progNum',11]).midi()",
    events:[{chan:0,note:60,prog:10,cycle_start:1}]},
  {name:'duplicate-sysex',code:"note(60).sysex([67,[1,2]]).midicmd(['sysex',68,[3,4]]).midi()",
    events:[{chan:0,note:60,sysex_id:[67],sysex:[1,2],cycle_start:1}]},
  ...[
    ['short-gate','note(60).compress(0,.001).midi()'],
    ['fractional-gate','note(60).fast(3).midi()'],
    ['offset-gate','note(60).compress(0,.01).midi({noteOffsetMs:100})'],
    ['zero-offset','note(60).midi({noteOffsetMs:0})'],
    ['clip-gate','note(60).clip(.25).midi()'],
    ['duration-gate','note(60).duration(.01).midi()'],
    ['command-program',"note(60).midicmd(['progNum',11]).midi()"],
    ['nrpn-two-data','note(60).nrpnn([1,2]).nrpv([64,32]).midi()'],
    ['map-default',"defaultmidimap({lpf:{ccn:74,min:100,max:1100,exp:2}}); note(60).lpf(600).midi()"],
    ['map-named',"await midimaps({qa:{lpf:74,pan:10}}); note(60).lpf(.5).pan(.25).midimap('qa').midi()"],
    ['map-options',"await midimaps({qa:{lpf:74}}); note(60).lpf(.5).midi({midimap:'qa',isController:true})"],
    ['map-clamp',"await midimaps({qa:{lpf:{ccn:'brightness',min:100,max:1100}}}); note(60).lpf(10000).midimap('qa').midi()"],
    ['map-invalid',"await midimaps({qa:{lpf:{ccn:74,min:1,max:1}}}); note(60).lpf(.5).midimap('qa').midi()"],
    ['null-gain','note(60).midi({gain:null})'],
    ['controller-invalid-velocity','note(60).gain(2).ccn(74).ccv(.5).midi({isController:1})'],
    ['map-before-invalid-note',"defaultmidimap({lpf:74}); note(60).lpf(.5).gain(2).midi()"],
    ['controls-after-midi','note(60).midi({velocity:.2}).gain(.5).velocity(.8)'],
    ['map-after-midi',"defaultmidimap({lpf:74}); note(60).midi().lpf(.5)"],
    ['map-selection-after-midi',"await midimaps({qa:{lpf:74}}); note(60).midi().midimap('qa').lpf(.5)"],
  ].map(([name,code])=>{
    const split=code.lastIndexOf(';');
    return {name,code,...(split>=0?{webcode:`(async()=>{${code.slice(0,split)};return ${code.slice(split+1)}})()`}:{})};
  }),
];
// Pinned 1.2.6 coerces numeric strings in control args (ccn("74") arrives
// as 74; sysexid "0x77" as 119) while the deployed REPL preserves them, so
// send outcomes diverge there by runtime version. Documented drift, kept out
// of the exact gate; AGENTS.md pins the runtime references.

async function webValues(browser) {
  return browser.evaluate(`(async()=>{
    const sent=[],timed=[];
    const ports=Array.from({length:3},(_,i)=>({id:'qa'+i,name:i?'out'+i:'out',
      manufacturer:'Tompouce QA',type:'output',state:'connected',connection:'open',
      open(){return Promise.resolve(this)},close(){return Promise.resolve(this)},
      send(data,time){sent.push(Array.from(data));timed.push({bytes:Array.from(data),time})}}));
    Object.defineProperty(navigator,'requestMIDIAccess',{configurable:true,value:async()=>({
      inputs:new Map(),outputs:new Map(ports.map(p=>[p.id,p])),sysexEnabled:true})});
    setAudioContext(new OfflineAudioContext(2,44100,44100));
    const results = [];
    for (const test of ${JSON.stringify(cases.map(c => ({name: c.name, code: c.webcode ?? c.code})))} ) {
      const instance = repl({getTime:()=>0, onSchedule:()=>{}, setInterval:()=>1, clearInterval:()=>{}});
      const entry = {name: test.name};
      try {
        defaultmidimap({});
        await instance.evaluate(test.code, false);
        if (!WebMidi.enabled) await new Promise(resolve=>WebMidi.addOneTimeListener('enabled',resolve));
        getClockBridge().getPerformanceTime=at=>1000+at*1000;
        if (instance.state.evalError) entry.error = String(instance.state.evalError?.message ?? instance.state.evalError);
        else {
          const haps = instance.scheduler.pattern.queryArc(0, 1);
          entry.values = haps.slice(0, 4).map(h => ({begin: Number(h.whole.begin), end: Number(h.whole.end), value: h.value}));
          sent.length=0;timed.length=0;
          entry.sendErrors=[];
          for (const hap of haps) {
            try {hap.context.onSchedule(hap,Number(hap.whole.begin)*2,.5);}
            catch(error){entry.sendErrors.push(String(error?.message??error));}
          }
          entry.sends=sent.filter(bytes=>(bytes[0]&0xF0)!==0x80);
          const ons=timed.filter(m=>(m.bytes[0]&0xf0)===0x90);
          const offs=timed.filter(m=>(m.bytes[0]&0xf0)===0x80);
          entry.gates=ons.map((m,i)=>(offs[i].time-m.time)/1000);
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
  const controllers=await browser.evaluate('Object.entries(WebMidi.MIDI_CONTROL_CHANGE_MESSAGES)');
  assert.equal(controllers.length,128,'deployed controller catalog changed; review native names');
  for(const [name] of controllers) {
    assert.match(name,/^[a-z0-9]+$/);
    // Actual send bytes are the oracle. WebMidi's exported constants reverse
    // the coarse/fine NRPN/RPN names relative to its sender's lookup table.
    cases.push({name:`catalog-${name}`,code:`ccn('${name}').ccv(.5).midi()`});
  }
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
    if(test.events)assert.deepEqual(got.events ?? [], test.events);
  } catch { failures.push(`${test.name}: commit differs (want ${JSON.stringify(test.events)} got ${JSON.stringify(got.events)})`); }
  try {assert.deepEqual(got.sends,want.sends);}
  catch {failures.push(`${test.name}: sends differ (web ${JSON.stringify(want.sends)} native ${JSON.stringify(got.sends)})`);}
  try {
    assert.equal(got.gates.length,want.gates.length);
    got.gates.forEach((gate,i)=>assert(Math.abs(gate-want.gates[i])<1e-9,`gate ${gate} != ${want.gates[i]}`));
  } catch(error){failures.push(`${test.name}: note gates differ: ${error.message}`);}
}
assert.deepEqual(failures, []);
console.log(`${cases.length} web/native MIDI cases passed (bundle ${bundleInfo.sha256_16})`);
