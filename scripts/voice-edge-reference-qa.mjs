/* Real deployed source automation, private browser and offline contexts. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

async function capture() {
  const results=[];
  const envelopes=[
    {attack:.02,decay:.03,sustain:.6,release:.1},
    {attack:.4,decay:.3,sustain:.2,release:.4},
    {attack:.02,decay:0,sustain:.6,release:.1},
    {attack:0,decay:0,sustain:1,release:.1},
    {attack:.02,decay:.03,sustain:.6,release:.1,cutoff:100,resonance:10},
  ];
  const sampleEnvelopes=[envelopes[3],{...envelopes[3],cutoff:100,resonance:10}];
  for(const sound of ['sine','piano']) for(const value of sound==='sine'?envelopes:sampleEnvelopes)
    for(const duration of value.cutoff?[.7]:[.015,.05,.2,.7]) for(const incoming of [.25,.3])
      for(const cut of sound==='piano'?[false,true]:[false]) {
        const context=new OfflineAudioContext(2,44100,44100);
        setAudioContext(context);setSuperdoughAudioController(null);resetGlobalEffects();
        await initAudio({maxPolyphony:1,multiChannelOrbits:false});
        const entry=getSound(sound),original=entry.onTrigger,held=[],stops=[],automation=[];
        entry.onTrigger=async(...args)=>{
          const result=await original(...args);held.push(result);
          if(held.length===1) {
            for(const name of ['setValueAtTime','linearRampToValueAtTime','cancelScheduledValues']) {
              const method=result.node.gain[name].bind(result.node.gain);
              result.node.gain[name]=(...values)=>{automation.push({name,values});return method(...values);};
            }
            result.nodes.source.forEach(node=>node.disconnect());
            const dc=context.createConstantSource();dc.offset.value=1;
            dc.connect(result.node);dc.start(.1);dc.stop(.1+duration+value.release);
            const stop=result.stop.bind(result);
            result.stop=at=>{stops.push(at);stop(at);dc.stop(at);};
          }
          return result;
        };
        try {
          await superdough({s:sound,note:60,gain:.1,postgain:1,...value,clip:1,...(cut?{cut:1}:{})},.1,duration,.5,0);
          const suspended=context.suspend(8960/44100),rendered=context.startRendering();
          await suspended;
          await superdough({s:sound,note:60,gain:0,...(cut?{cut:1}:{})},incoming,.4,.5,0);
          await context.resume();
          const pcm=(await rendered).getChannelData(0),points=[];
          // Samples' ADSR is upstream of result.node. Apply that independent
          // envelope after measuring the real cut gain so both paths compare
          // the same constant source, without piano decoder/timbre differences.
          for(let frame=4500;frame<30000;frame+=137) {
            const time=frame/44100,age=time-.1;
            // Filter fixtures gate the post-stop ring-out; the unfiltered
            // fixtures above gate envelope/cut automation before source stop.
            if(value.cutoff && time<incoming+.25) continue;
            const envelope=sound==='piano'?(age<=duration?1:Math.max(0,1-(age-duration)/value.release)):1;
            points.push({frame,gain:pcm[frame]/.1*envelope});
          }
          results.push({sound,value,duration,incoming,cut,stops,automation,points});
        } finally {entry.onTrigger=original;}
      }
  return results;
}
const browser=await BrowserClient.create();let results;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function" && typeof getSound==="function" && getSound("sine")?.onTrigger && getSound("piano")?.onTrigger',20000);
  results=await browser.evaluate('('+capture.toString()+')()');
} finally {await browser.close();}
const artifacts=process.argv[2]??'/tmp/tompouce-voice-edge-reference';
await fs.mkdir(artifacts,{recursive:true});
await fs.writeFile(`${artifacts}/web.json`,JSON.stringify(results,null,2)+'\n');
const native=spawnSync(process.env.TOMPOUCE_VOICE_BINARY??'./build/tests/test_audio_performance',
  ['--voice-reference'],{input:JSON.stringify(results.map(test=>({...test,points:test.points.map(({frame})=>({frame}))}))),encoding:'utf8',maxBuffer:4*1024*1024});
assert.equal(native.status,0,native.stderr);
const actual=JSON.parse(native.stdout);assert.equal(actual.length,results.length);
await fs.writeFile(`${artifacts}/native.json`,JSON.stringify(actual,null,2)+'\n');
let failures=0;
for(const [index,test] of results.entries()) for(const [i,point] of test.points.entries()) {
  if(Math.abs(actual[index][i]-point.gain)>3e-5) {
    if(failures++<12) console.error({index,sound:test.sound,value:test.value,duration:test.duration,
      incoming:test.incoming,cut:test.cut,time:point.frame/44100,web:point.gain,native:actual[index][i]});
  }
}
assert.equal(failures,0,'source retirement envelope/cut PCM differences');
console.log(`${results.length} web/native retirement envelope/cut cases passed`);
