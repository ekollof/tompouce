/* Live strudel.cc verification against the test-only scheduler snapshot.
   Owned disposable browser; fake clocks and captured callbacks produce no
   real audio. Output checks use isolated OfflineAudioContexts. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { BrowserClient } from './browser-client.mjs';
import { Cyclist } from '../tests/reference/web-cyclist.mjs';
import * as patterns from '../js/node_modules/@strudel/core/pattern.mjs';
import * as controls from '../js/node_modules/@strudel/core/controls.mjs';
import { controlTrace, normalize } from '../tests/reference/control-trace.mjs';

const expected=normalize(await controlTrace(Cyclist,{...patterns,...controls}));
async function controlAliases() {
  const test=repl({getTime:()=>0,onSchedule:()=>{},setInterval:()=>1,clearInterval:()=>{}});
  globalThis.__aliasCpm=60; test.scheduler.setCps(.5);
  await test.evaluate('note(60).cpm(ref(()=>globalThis.__aliasCpm))',false);
  if (test.state.evalError) throw test.state.evalError;
  const pattern=test.scheduler.pattern, dynamic=[];
  for (const [cps,value] of [[.5,60],[1,60],[1,30],[.25,30]]) {
    test.scheduler.setCps(cps); globalThis.__aliasCpm=value;
    const haps=pattern.queryArc(0,1,{_cps:cps});
    dynamic.push([haps.length,Number(haps[0].whole.end)]);
  }
  test.scheduler.setCps(.5); await test.evaluate('note(60).cpm(60)',false);
  test.scheduler.setCps(1);
  const numeric=test.scheduler.pattern.queryArc(0,1,{_cps:1}).length;
  await test.evaluate('(()=>{setCpm(pure(30));return note(60)})()',false);
  if (test.state.evalError) throw test.state.evalError;
  const patternTempo=test.scheduler.cps;
  const warnings=[],warn=console.warn;
  console.warn=(message)=>warnings.push(String(message));
  try {await test.evaluate('(()=>{const control=slider;return note(60).gain(control(.7))})()',false);}
  finally {console.warn=warn;}
  if (test.state.evalError) throw test.state.evalError;
  const aliasGain=test.scheduler.pattern.queryArc(0,1)[0].value.gain;
  const aliasWidgets=test.state.sliders?.length??0;
  test.stop(); delete globalThis.__aliasCpm;
  return {dynamic,numeric,patternTempo,aliasGain,aliasWidgets,warnings};
}
const browser=await BrowserClient.create();
const artifacts=path.resolve(process.argv[2]??'/tmp/tompouce-web-controls-reference-qa');
try {
  await browser.page('https://strudel.cc/');
  await browser.until(`!!window.strudelMirror?.repl?.scheduler && typeof superdough==='function' && getSound('sine')`,20000);
  const actual=await browser.evaluate(`(${controlTrace.toString()})(strudelMirror.repl.scheduler.constructor,{note,fastcat,pure,ref,stack}).then(${normalize.toString()})`);
  assert.deepEqual(actual,expected,'test scheduler snapshot differs from the live web REPL');
  assert.deepEqual(actual.errors,[]);
  const zeroTempo=await browser.evaluate(`(async()=>{
    const context=new OfflineAudioContext(2,44100,44100);
    setAudioContext(context); setSuperdoughAudioController(null);
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    try {await superdough({note:60,s:'sine',gain:0,postgain:0},.1,Infinity,0,0);return {accepted:true};}
    catch(error){return {accepted:false,name:error.name,message:error.message};}
  })()`);
  assert.equal(zeroTempo.accepted,false,'web output now accepts an infinite note duration; revisit native zero-tempo behavior');
  assert.match(zeroTempo.message,/non-finite/);
  const aliases=await browser.evaluate('('+controlAliases.toString()+')()');
  assert.deepEqual(aliases.dynamic,[[2,.5],[1,1],[1,2],[2,.5]]);
  assert.equal(aliases.numeric,2); assert.equal(aliases.patternTempo,.5);
  assert.equal(aliases.aliasGain,.7); assert.equal(aliases.aliasWidgets,0);
  assert(aliases.warnings.some(message=>message.includes('slider will only work')));
  const lifecycle=await browser.evaluate(`(async()=>{
    let time=0,timer;
    const test=repl({getTime:()=>time,onSchedule:()=>{},setInterval:fn=>(timer=fn,1),clearInterval:()=>{}});
    await test.evaluate('note(60)',true);
    const pattern=test.scheduler.pattern;
    time=.1; timer();
    const before={cycle:Number(test.scheduler.lastEnd),cps:test.scheduler.cps,started:test.scheduler.started};
    await test.evaluate('(()=>{throw Error("performance eval failure")})()',true);
    const after={cycle:Number(test.scheduler.lastEnd),cps:test.scheduler.cps,started:test.scheduler.started};
    const retained=!!pattern && test.scheduler.pattern===pattern;
    const error=test.state.evalError?.message;
    test.stop();
    let activationPending;
    const mirror={repl:test,code:'note(72)',flash:()=>{},
      evaluate(...args){return activationPending=strudelMirror.evaluate.apply(this,args)}};
    await strudelMirror.toggle.call(mirror);
    await activationPending; // toggle launches evaluation without awaiting it.
    const activation={started:test.scheduler.started,note:Number(test.scheduler.pattern.queryArc(0,1)[0].value.note)};
    test.stop(); mirror.code='(()=>{throw Error("stopped eval failure")})()';
    const stoppedPattern=test.scheduler.pattern;
    await strudelMirror.toggle.call(mirror);
    await activationPending;
    activation.failedStarted=test.scheduler.started;
    activation.failedRetained=test.scheduler.pattern===stoppedPattern;
    activation.failedError=test.state.evalError?.message;
    const context=getAudioContext();
    if (!(context instanceof OfflineAudioContext)) throw Error('refuse real audio output');
    resetGlobalEffects(); setSuperdoughAudioController(null);
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const calls=[];
    const stop=AudioScheduledSourceNode.prototype.stop,disconnect=AudioNode.prototype.disconnect;
    AudioScheduledSourceNode.prototype.stop=function(...args){calls.push({kind:'stop',args});return stop.apply(this,args)};
    AudioNode.prototype.disconnect=function(...args){calls.push({kind:'disconnect'});return disconnect.apply(this,args)};
    await strudelMirror.repl.scheduler.setPattern(silence);
    strudelMirror.repl.scheduler.setStarted(true);
    await superdough({s:'sine',freq:440,gain:.5,postgain:1,attack:.001,decay:0,sustain:1,release:.02},.1,2,.5,0);
    const suspended=context.suspend(.2);
    const rendering=context.startRendering();
    await suspended;
    const scheduled=calls.slice();
    strudelMirror.repl.stop();
    const stopped=calls.slice(),stopTime=context.currentTime;
    await context.resume();
    const pcm=(await rendering).getChannelData(0);
    const energy=(begin,end)=>{let sum=0;for(let i=Math.round(begin*44100);i<Math.round(end*44100);i++)sum+=pcm[i]*pcm[i];return sum;};
    return {retained,error,before,after,activation,scheduled,stopped,stopTime,
      beforeEnergy:energy(.13,.18),afterEnergy:energy(.25,.3)};
  })()`);
  assert.equal(lifecycle.retained,true);
  assert.equal(lifecycle.error,'performance eval failure');
  assert.deepEqual(lifecycle.before,lifecycle.after,'failed evaluation changed the running web clock');
  assert.deepEqual(lifecycle.activation,{started:true,note:72,failedStarted:false,
    failedRetained:true,failedError:'stopped eval failure'},'web Play/evaluation activation changed');
  assert(lifecycle.scheduled.some(call=>call.kind==='stop' && call.args[0]>2));
  assert.deepEqual(lifecycle.scheduled,lifecycle.stopped,'web Stop changed source lifetime; revisit native draining');
  assert(lifecycle.stopTime>=.2 && lifecycle.stopTime<.21);
  assert(lifecycle.beforeEnergy>1e-4 && lifecycle.afterEnergy>1e-4,'web Stop muted the held source');
  assert(Math.abs(lifecycle.afterEnergy/lifecycle.beforeEnergy-1)<.001,'web Stop changed held source amplitude');
  const queryFailure=await browser.evaluate(`(async()=>{
    const events=[],errors=[];
    const scheduler=new strudelMirror.repl.scheduler.constructor({getTime:()=>0,setInterval:()=>1,clearInterval:()=>{},
      onError:error=>errors.push(error.message),onSchedule:(hap,time)=>events.push({time,note:hap.value.note})});
    const healthy=note(fastcat(60,64,67,72)).fast(10);
    await scheduler.setPattern({queryArc:(begin,...args)=>{if(begin<.025)throw Error('query failure');return healthy.queryArc(begin,...args)}});
    await scheduler.start(); const cycle=Number(scheduler.lastEnd); scheduler.stop();
    return {events,errors,cycle};
  })()`);
  assert.deepEqual(queryFailure.errors,['query failure']);
  assert.equal(queryFailure.events.length,3);
  assert(Math.abs(queryFailure.cycle-.1)<1e-9);
  const source=await browser.evaluate(`String(strudelMirror.repl.scheduler.constructor)`);
  const report={url:'https://strudel.cc/',capturedAt:new Date().toISOString(),
    schedulerSha256:createHash('sha256').update(source).digest('hex'),
    snapshotSha256:createHash('sha256').update(await fs.readFile('tests/reference/web-cyclist.mjs')).digest('hex'),
    events:actual.events.length,states:actual.states.length,zeroTempo,lifecycle,queryFailure,aliases,trace:actual};
  await fs.mkdir(artifacts,{recursive:true});
  await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify(report,null,2)+'\n');
  process.stdout.write(`Live web REPL reference verified: ${report.events} events, ${report.states} clock states; ${artifacts}\n`);
} finally {await browser.close();}
