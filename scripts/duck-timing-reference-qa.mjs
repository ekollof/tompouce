/* Real web duck timers and gain PCM in owned offline contexts only. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

async function capture(cases) {
  const results=[];
  for (const test of cases) {
    const context=new OfflineAudioContext(2,44100,44100);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const controller=getSuperdoughAudioController(),orbit=controller.getOrbit(1,[1,2]);
    const dc=context.createConstantSource(); dc.offset.value=.1;
    dc.connect(orbit.output); dc.start(); orbit.output.gain.value=.8;
    const ramps=[],gain=orbit.output.gain;
    const ramp=gain.exponentialRampToValueAtTime.bind(gain);
    gain.exponentialRampToValueAtTime=(...args)=>{ramps.push(args);return ramp(...args);};
    const invoke=(sourceTime,controls)=>{
      const stop=AudioScheduledSourceNode.prototype.stop;
      let timer;
      AudioScheduledSourceNode.prototype.stop=function(...args) {
        if(this instanceof ConstantSourceNode && this.onended) timer=this;
        return stop.apply(this,args);
      };
      try {controller.duck(1,sourceTime,controls.onset,controls.attack,controls.depth);}
      finally {AudioScheduledSourceNode.prototype.stop=stop;}
      if(!timer?.onended) throw Error('missing duck timer');
      ramps.length=0;
      let rejected=0;
      try {timer.onended.call(timer,new Event('ended'));} catch {rejected=ramps.length;}
      timer.onended=null;
      return rejected;
    };
    const before=test.prior?context.suspend(test.prior==='down'?.28:.09):null;
    const suspended=context.suspend(test.callbackAt),rendered=context.startRendering();
    let prior;
    if(before) {
      await before;
      prior={callbackAt:context.currentTime,sourceTime:test.prior==='down'?.3:.1,
        controls:{onset:0,attack:.6,depth:.75}};
      invoke(prior.sourceTime,prior.controls); await context.resume();
    }
    await suspended;
    const callbackAt=context.currentTime;
    const rejected=invoke(test.sourceTime,test.controls);
    await context.resume(); const pcm=(await rendered).getChannelData(0);
    const frames=[1,128,441,2205,4410,8820,17640].map(offset=>Math.round(callbackAt*44100)+offset)
      .filter(frame=>frame<44100);
    results.push({...test,callbackAt,prior,rejected,
      points:frames.map(frame=>({time:frame/44100,gain:pcm[frame]/.1}))});
  }
  return results;
}
const cases=[];
for(const onset of [-.4,-.3,-.02,-.01,0,.02,'1','','e0','e-1',' ','\u00a0',true])
  cases.push({name:`onset ${JSON.stringify(onset)}`,sourceTime:.3,callbackAt:.29,
    controls:{onset,attack:.1,depth:.25}});
for(const callbackAt of [.3,.4,.6]) for(const onset of [-.02,0,.02,'1'])
  cases.push({name:`late ${callbackAt}/${onset}`,sourceTime:.3,callbackAt,
    controls:{onset,attack:.1,depth:.25}});
for(const prior of [true,'down']) for(const controls of [{onset:0,attack:.2,depth:.25},{onset:0,attack:.2,depth:0},
  {onset:0,attack:'bad',depth:.25},
  {onset:0,attack:.1,depth:-1},{onset:-.02,attack:.1,depth:.25},
  {onset:' ',attack:.1,depth:.25},{onset:'e0',attack:.1,depth:.25}])
  cases.push({name:`interrupted ${prior} ${JSON.stringify(controls)}`,sourceTime:.3,callbackAt:.29,prior,controls});
for(const text of ['\u180e.25','\u200b.25','\u202f.25\u3000','0B10','0O10','0X10',
  '+0b10','-0o10','inf','infinity','+Infinity','-Infinity','1e309','1e-324',
  '2.4703282292062327e-324','2.4703282292062328e-324','0x1p-2','1_000',
  '0x10000000000000001','0b'+'1'.repeat(100)]) for(const field of ['attack','depth'])
  cases.push({name:`${field} ${JSON.stringify(text)}`,sourceTime:.3,callbackAt:.29,
    controls:{onset:0,attack:.1,depth:.25,[field]:text}});
for(const onset of ['e-324','e309','e'+'0'.repeat(100),'1'+' '.repeat(100),'0'.repeat(126),
  [[null]],[[false]],[[[]]],[[[1]]]])
  cases.push({name:`onset ${JSON.stringify(onset)}`,sourceTime:.3,callbackAt:.29,
    controls:{onset,attack:.1,depth:.25}});
// Null/boolean selections: the mapping gate normalizes these single-step
// coercions, so prove them end to end at ramp/PCM level here.
for(const controls of [{onset:null,attack:.1,depth:.25},{onset:0,attack:null,depth:.25},
  {onset:0,attack:.1,depth:null},{onset:0,attack:.1,depth:true},
  {onset:0,attack:false,depth:.25},{onset:true,attack:.1,depth:.25},
  {onset:false,attack:true,depth:null}])
  cases.push({name:`coerce ${JSON.stringify(controls)}`,sourceTime:.3,callbackAt:.29,controls});
for(const [index,onset] of ['0'.repeat(128),'0'.repeat(10000),'1'+' '.repeat(10000),
  'e'+'0'.repeat(10000),'e-'+'0'.repeat(10000)+'1','0'.repeat(10000)+'1',
  '0'.repeat(1000)+'e-1','\u00a0'.repeat(1000),'0'.repeat(1000)+'bad'].entries())
  for(const callbackAt of [.29,.4]) for(const attack of [.1,1])
    cases.push({name:`long onset ${index}, ${callbackAt}, ${attack}`,sourceTime:.3,callbackAt,
      controls:{onset,attack,depth:.25}});
for(const [field,text] of [['onset','1\0tail'],['onset','0'.repeat(128)+'\0'],
  ['onset',[['1\0']]],['attack','0.1\0tail'],['depth','0.25\0tail']]) {
  const controls={onset:0,attack:.1,depth:.25,[field]:text};
  const values=Object.fromEntries(Object.entries(controls).map(([key,value])=>['duck'+key,value]));
  values.duckorbit=1;
  values.s='~';
  const characters=Array.from(JSON.stringify(values),text=>text.charCodeAt(0));
  cases.push({name:`NUL ${field} ${typeof text}`,sourceTime:.3,callbackAt:.29,controls,
    code:`s('~').fmap(()=>JSON.parse(String.fromCharCode(${characters.join(',')})))`});
}
const browser=await BrowserClient.create();
let results;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function"',20000);
  results=await browser.evaluate('('+capture.toString()+')('+JSON.stringify(cases)+')');
} finally {await browser.close();}
const artifacts=process.argv[2]??'/tmp/tompouce-duck-timing-reference';
await fs.mkdir(artifacts,{recursive:true});
await fs.writeFile(path.join(artifacts,'web.json'),JSON.stringify(results,null,2)+'\n');
// Keep each test invocation below the native harness's bounded input size.
const actual=[];
for(const result of results) {
  const native=spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY??'./build/tests/test_live_sliders',
    ['--duck-reference'],{input:JSON.stringify([result]),encoding:'utf8',maxBuffer:2*1024*1024});
  assert.equal(native.status,0,native.stderr);
  actual.push(...JSON.parse(native.stdout));
}
assert.equal(actual.length,results.length);
await fs.writeFile(path.join(artifacts,'native.json'),JSON.stringify(actual,null,2)+'\n');
const failures=[];
results.forEach((test,index)=>{
  assert.equal(actual[index].gains.length,test.points.length);
  assert(actual[index].gains.every(gain=>typeof gain==='number' && Number.isFinite(gain)));
  if(actual[index].rejected!==test.rejected) failures.push(`${test.name}: rejected ramp`);
  test.points.forEach((point,i)=>{
    if(Math.abs(point.gain-actual[index].gains[i])>2e-6)
      failures.push(`${test.name} @ ${point.time}: web ${point.gain}, native ${actual[index].gains[i]}`);
  });
});
assert.deepEqual(failures,[]);
console.log(`${results.length} web/native duck timing, coercion and interrupted-ramp cases passed`);
