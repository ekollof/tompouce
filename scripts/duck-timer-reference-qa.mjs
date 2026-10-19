/* Timer construction failures through real Superdough, owned silent contexts. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

async function capture() {
  const results=[];
  for(const time of [.005,.01,.011]) for(const targets of [9,[9,9],[8],[8,9],[9,8],[]])
    for(const sound of ['sine','~','qa_missing_duck_timer']) {
      const context=new OfflineAudioContext(2,8820,44100);
      setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
      await initAudio({maxPolyphony:1,multiChannelOrbits:false});
      const controller=getSuperdoughAudioController();
      const target=controller.getOrbit(9,[1,2]); target.output.gain.value=.8;
      const entry=getSound('sine'),original=entry.onTrigger,held=[],stops=[];
      entry.onTrigger=async(...args)=>{
        const result=await original(...args); held.push(result);
        const stop=result.stop.bind(result); result.stop=(at)=>{stops.push(at);return stop(at);};
        return result;
      };
      await superdough({s:'sine',gain:.1,orbit:1},.1,.1,.5,0);
      const calls=[],created=[],effects=[],timers=[],cancellations=[];
      const duck=target.duck;
      target.duck=(...args)=>{calls.push(args);return duck.apply(target,args);};
      const getOrbit=controller.getOrbit.bind(controller),seen=new Set([1,9]);
      controller.getOrbit=(id,...args)=>{
        const orbit=getOrbit(id,...args);
        if(!seen.has(id)) {
          seen.add(id); created.push(id);
          for(const name of ['getDelay','getReverb']) {
            const method=orbit[name]; orbit[name]=(...values)=>{effects.push(name);return method.apply(orbit,values);};
          }
        }
        return orbit;
      };
      const cancel=target.output.gain.cancelScheduledValues.bind(target.output.gain);
      target.output.gain.cancelScheduledValues=(...args)=>{cancellations.push(args);return cancel(...args);};
      const stop=AudioScheduledSourceNode.prototype.stop;
      const pending=[];
      AudioScheduledSourceNode.prototype.stop=function(...args) {
        if(this instanceof ConstantSourceNode && this.onended) {timers.push(args[0]);pending.push(this);}
        return stop.apply(this,args);
      };
      let error;
      try {
        await superdough({s:sound,gain:.1,orbit:7,duckorbit:targets,duckdepth:.25,
          delay:1,delaytime:.123,delayfeedback:.5,room:1,roomsize:.05},time,.1,.5,0);
      } catch(caught) {error=caught.name;}
      finally {AudioScheduledSourceNode.prototype.stop=stop;entry.onTrigger=original;}
      for(const timer of pending) timer.onended=null;
      results.push({time,targets,sound,error:error??null,created,duckCalls:calls.length,timers,
        cancellations:cancellations.length,sources:held.length-1,stops:stops.slice(),effects});
      await context.startRendering();
    }
  return results;
}
const browser=await BrowserClient.create();
let results;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function"',20000);
  results=await browser.evaluate('('+capture.toString()+')()');
} finally {await browser.close();}
const artifacts=process.argv[2]??'/tmp/tompouce-duck-timer-reference';
await fs.mkdir(artifacts,{recursive:true});
await fs.writeFile(path.join(artifacts,'web.json'),JSON.stringify(results,null,2)+'\n');
const native=spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY??'./build/tests/test_live_sliders',
  ['--duck-timer-reference'],{input:JSON.stringify(results),encoding:'utf8',maxBuffer:2*1024*1024});
assert.equal(native.status,0,native.stderr);
const actual=JSON.parse(native.stdout); assert.equal(actual.length,results.length);
await fs.writeFile(path.join(artifacts,'native.json'),JSON.stringify(actual,null,2)+'\n');
for(const [index,test] of results.entries()) {
  const targets=Array.isArray(test.targets)?test.targets:[test.targets];
  const matched=targets.filter(target=>target===9).length;
  const fails=test.time<.01 && matched>0;
  assert.deepEqual(test.created,[7]);
  assert.equal(test.error,fails?'RangeError':test.sound==='qa_missing_duck_timer'?'Error':null);
  assert.equal(test.duckCalls,fails?1:matched);
  assert.deepEqual(test.timers,Array(fails?1:matched).fill(test.time-.01));
  assert.equal(test.cancellations,0,'creating a timer must not cancel gain automation');
  assert.equal(test.sources,!fails && test.sound==='sine'?1:0);
  assert.deepEqual(actual[index],{sources:test.sources,controls:fails?0:matched,
    errors:fails?1:0,timer:fails,orbit:fails?9:-1,created:(1<<1)|(1<<7)|(1<<9),
    delay:test.effects.includes('getDelay'),room:test.effects.includes('getReverb'),gain:.8});
  if(fails) {
    assert.deepEqual(test.stops,[],'a rejected duck timer must not evict sources');
    assert.deepEqual(test.effects,[],'a rejected duck timer must not prepare shared effects');
  }
}
console.log(`${results.length} web/native duck timer construction/source/routing cases passed`);
