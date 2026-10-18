// Compare the native host adapter with the web REPL's query-time ref contract.
// Uses the pinned upstream core directly; never regenerates event baselines.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { Cyclist } from '../tests/reference/web-cyclist.mjs';
import { controlTrace, normalize } from '../tests/reference/control-trace.mjs';
import * as patterns from './node_modules/@strudel/core/pattern.mjs';
import * as controls from './node_modules/@strudel/core/controls.mjs';
import Fraction from './node_modules/fraction.js/dist/fraction.mjs';
import {queryFraction} from './query-time.mjs';
const core = { ...patterns, ...controls };
const notes = (...values) => core.note(core.fastcat(...values.map(core.pure)));

const clockValues=[.6000000000000001,.6250000000000001,.7500000000000002,
  .7750000000000002,0,1,-.6000000000000001,1e-12,1e12+.25];
for (let i=0;i<200;i++) clockValues.push(Math.sin(i*37.17)*10000);
for (let d=2;d<=32;d++) for (const offset of [-Number.EPSILON,0,Number.EPSILON])
  clockValues.push(1/d+offset,1-1/d+offset);
for (const value of clockValues) {
  const expected=Fraction(value),actual=queryFraction(Fraction,value);
  assert.equal(actual.n,expected.n,`clock numerator ${value}`);
  assert.equal(actual.d,expected.d,`clock denominator ${value}`);
  assert.equal(actual.s,expected.s,`clock sign ${value}`);
}
process.stdout.write(`Clock fractions: ${clockValues.length} exact pinned conversions passed\n`);

let values = [];
globalThis.__hostLog = () => {};
globalThis.__hostSliderBind = (value, min, max, step) => {
  assert.equal(step, (max-min)/1000, 'web widget default slider step');
  values.push(value); return values.length - 1;
};
globalThis.__hostSliderRead = index => values[index];
await import('./dist/runtime.js');

const cases = [
  ['gain', 'note("60 64 67 72").gain(slider(.5))', x => notes(60,64,67,72).gain(x)],
  ['pan', 'note("60 64").pan(slider(.5))', x => notes(60,64).pan(x)],
  ['filter', 'note("60 64").lpf(slider(.5).range(100,4000))', x => notes(60,64).lpf(x.range(100,4000))],
  ['arithmetic', 'note(60).gain(slider(.5).mul(.5).add(.1))', x => core.note(60).gain(x.mul(.5).add(.1))],
  ['mask', 'note("60 64 67 72").mask(slider(.5))', x => notes(60,64,67,72).mask(x)],
  ['fast', 'note("60 64").fast(slider(.5))', x => notes(60,64).fast(x)],
  ['slow', 'note("60 64").slow(slider(.5))', x => notes(60,64).slow(x)],
  ['effects', 'note(60).room(slider(.5)).delay(slider(.5))', x => core.note(60).room(x).delay(x)],
  ['held pattern', 'note(60).slow(4).gain(slider(.5))', x => core.note(60).slow(4).gain(x)],
];
let queries = 0;
for (const [name, code, build] of cases) {
  values = [];
  await __compile(code, 120);
  const upstream = build(core.ref(() => values[0]));
  for (const value of [0, .25, .5, 1, .75]) {
    values.fill(value);
    for (const [begin, end] of [[0, 1], [5, 6], [5.125, 5.625]]) {
      const expected = upstream.queryArc(begin, end, { _cps: .5 })
        .filter(h => h.hasOnset() && h.whole && +h.whole.begin >= begin && +h.whole.begin < end)
        .map(h => ({ begin: +h.whole.begin, end: +h.whole.end, value: h.value }));
      const actual = JSON.parse(__query(begin, end, .5))
        .map(({ begin, end, value }) => ({ begin, end, value }));
      const order = (a,b) => a.begin-b.begin || a.end-b.end || JSON.stringify(a.value).localeCompare(JSON.stringify(b.value));
      assert.deepEqual(actual.sort(order), expected.sort(order), `${name}: value=${value}, arc=${begin}..${end}`);
      queries++;
    }
  }
}
process.stdout.write(`Web REPL live controls: ${queries} upstream ref comparisons passed\n`);

for (const code of ['const control=slider; note(60).gain(control(.7))',
                    "note(60).gain(globalThis['slider'](.7))"]) {
  values=[]; await __compile(code,120);
  assert.equal(values.length,0,'aliased slider must not register a hidden live control');
  assert.equal(JSON.parse(__query(0,1,.5))[0].value.gain,.7);
}
values=[]; await __compile('note(60).cpm(slider(60))',120);
assert.equal(JSON.parse(__query(0,1,.5)).length,2);
assert.equal(JSON.parse(__query(0,1,1)).length,1);
values[0]=30;
assert.equal(JSON.parse(__query(0,1,1))[0].end,2);
assert.equal(JSON.parse(__query(0,1,.25)).length,2);
values=[]; await __compile('note(60).cpm(60)',120);
assert.equal(JSON.parse(__query(0,1,1)).length,2,'numeric cpm retains evaluation-time scaling');
values=[]; const globalTempo=JSON.parse(await __compile('setCpm(slider(30)); note(60)',120));
assert.equal(globalTempo.bpm,120); assert.equal(values.length,1);
process.stdout.write('Web control aliases: static slider fallback, query-time cpm and pattern-valued setcpm passed\n');

if (process.env.TOMPOUCE_LIVE_CONTROLS_BINARY) {
  let now=0, timer, gain=.5;
  const expected=[];
  const scheduler=new Cyclist({getTime:()=>now,setInterval:fn=>(timer=fn,1),clearInterval:()=>{},
    onSchedule:(hap,time,cps)=>expected.push({time,duration:Number(hap.duration)/cps,
      freq:440*2**((hap.value.note-69)/12),gain:hap.value.gain})});
  await scheduler.setPattern(notes(60,64,67,72).fast(10).gain(core.ref(()=>gain)));
  await scheduler.start();
  gain=.75; now=.1; timer();
  scheduler.setCps(1); now=.2; timer();
  await scheduler.setPattern(notes(72,76,79,84).fast(10).gain(core.ref(()=>gain)));
  now=.3; timer(); scheduler.stop();
  const result=spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY,['--clock-reference'],{encoding:'utf8',timeout:15000});
  assert.equal(result.status,0,result.stderr);
  const actual=JSON.parse(result.stdout);
  assert.equal(actual.length,expected.length);
  for (let i=0;i<actual.length;i++) for (const field of ['time','duration','freq','gain'])
    assert.ok(Math.abs(actual[i][field]-expected[i][field])<1e-7,`clock event ${i} ${field}: native=${actual[i][field]}, web=${expected[i][field]}`);
  process.stdout.write(`Web REPL clock: ${actual.length} native events match upstream slider, tempo and pattern changes\n`);

  let cps=.5; now=0; expected.length=0;
  await scheduler.setPattern(notes(60,64,67,72).fast(10).gain(.5).cps(core.ref(()=>cps)));
  scheduler.setCps(.5); await scheduler.start();
  cps=1; now=.1; timer(); now=.2; timer();
  cps=.25; for (const time of [.3,.4,.5]) {now=time;timer();}
  cps=0; for (const time of [.6,.7,.8]) {now=time;timer();}
  assert.equal(scheduler.cps,0);
  cps=.5; scheduler.setCps(.5); for (const time of [.9,1]) {now=time;timer();}
  scheduler.stop();
  const cpsResult=spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY,['--cps-reference'],{encoding:'utf8',timeout:15000});
  assert.equal(cpsResult.status,0,cpsResult.stderr);
  const cpsActual=JSON.parse(cpsResult.stdout);
  const byTime=(a,b)=>a.time-b.time || a.freq-b.freq;
  // The live browser rejects the non-finite AudioParam times/durations
  // produced at zero tempo. Compare audible schedules and separately check
  // that the control pauses/resumes the cycle clock.
  const finiteExpected=expected.filter(e=>Number.isFinite(e.time) && Number.isFinite(e.duration));
  expected.length=0; expected.push(...finiteExpected);
  expected.sort(byTime); cpsActual.sort(byTime);
  if (cpsActual.length!==expected.length)
    process.stderr.write(JSON.stringify({native:cpsActual,web:expected},null,2)+'\n');
  assert.equal(cpsActual.length,expected.length);
  for (let i=0;i<cpsActual.length;i++) for (const field of ['time','duration','freq','gain'])
    assert.ok(Math.abs(cpsActual[i][field]-expected[i][field])<1e-7,`cps event ${i} ${field}: native=${cpsActual[i][field]}, web=${expected[i][field]}`);
  process.stdout.write(`Web REPL event tempo: ${cpsActual.length} native events match upstream cps changes\n`);
  const performance=await controlTrace(Cyclist,core);
  assert.deepEqual(performance.errors,[]);
  const performanceResult=spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY,['--performance-reference'],{encoding:'utf8',timeout:15000});
  assert.equal(performanceResult.status,0,performanceResult.stderr);
  const performanceActual=normalize(JSON.parse(performanceResult.stdout));
  const performanceExpected=normalize({events:performance.events.filter(e=>Number.isFinite(e.time) && Number.isFinite(e.duration))
    .map(({time,duration,note})=>({time,duration,note})),states:performance.states});
  const eventOrder=(a,b)=>a.time-b.time || a.note-b.note;
  performanceActual.events.sort(eventOrder); performanceExpected.events.sort(eventOrder);
  assert.deepEqual(performanceActual,performanceExpected,'native mixed tempo/transport scenario differs from web REPL');
  process.stdout.write(`Web REPL performance: ${performanceActual.events.length} native events and ${performanceActual.states.length} clock states match\n`);
  const queryExpected=[]; let queryErrors=0;
  const failureScheduler=new Cyclist({getTime:()=>0,setInterval:()=>1,clearInterval:()=>{},onError:()=>{queryErrors++;},
    onSchedule:(hap,time,cps)=>queryExpected.push({time,duration:Number(hap.duration)/cps,freq:440*2**((hap.value.note-69)/12),gain:hap.value.gain})});
  const healthy=notes(60,64,67,72).fast(10).gain(.5);
  await failureScheduler.setPattern({queryArc:(begin,...args)=>{if(begin<.025)throw Error('query failure');return healthy.queryArc(begin,...args);}});
  await failureScheduler.start(); failureScheduler.stop();
  assert.equal(queryErrors,1); assert.equal(queryExpected.length,3);
  const queryResult=spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY,['--query-failure-reference'],{encoding:'utf8',timeout:15000});
  assert.equal(queryResult.status,0,queryResult.stderr);
  assert.deepEqual(normalize(JSON.parse(queryResult.stdout)),normalize(queryExpected),'query failure stalled subsequent windows');
  process.stdout.write('Web REPL query failure: later scheduling windows continue without restarting\n');
}
