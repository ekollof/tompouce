/* Deployed voice-pool policy; disposable browser, offline audio only. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {BrowserClient} from './browser-client.mjs';

async function capture() {
  const context=new OfflineAudioContext(2,44100,44100);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const sound=getSound('sine'),original=sound.onTrigger,held=[],calls=[];
  const random=Math.random; let nextId=0;
  // Superdough uses rounded random IDs as Map keys. Avoid accidental key
  // collisions obscuring pool policy; this fixture does not compare timbre.
  Math.random=()=>((++nextId)%1000000)/1000000;
  sound.onTrigger=async (...args)=>{
    const result=await original(...args),id=args[1].qaId;
    // Keep strong references: the deployed pool stores WeakRefs. Capture only
    // eviction automation, after the source's own envelope has been scheduled.
    held.push(result);
    const ramp=result.node.gain.linearRampToValueAtTime.bind(result.node.gain);
    result.node.gain.linearRampToValueAtTime=(value,time)=>{
      calls.push({id,method:'ramp',value,time});return ramp(value,time);
    };
    const stop=result.stop.bind(result);
    result.stop=time=>{calls.push({id,method:'stop',time});return stop(time);};
    return result;
  };
  try {
    // Reverse target-time order and varied gains distinguish insertion FIFO
    // from oldest onset, physical-slot order and quietest-voice selection.
    for(let id=0;id<130;id++)
      await superdough({s:'sine',qaId:id,note:60,gain:id===0?.5:.001,
        attack:.001,decay:0,sustain:1,release:.01},.2-id*.0001,.6,.5,0);
    const afterNotes=calls.length;
    await superdough({s:'~'},.3,.1,.5,0);
    const afterRest=calls.length;
    await superdough({s:'sine',qaId:130,note:60,gain:0},.4,.1,.5,0);
    const afterSilent=calls.length;
    let missingError;
    try {await superdough({s:'tompouce_qa_nonexistent_sound'},.5,.1,.5,0);}
    catch(error) {missingError=String(error);}
    await superdough({s:'sine',qaId:131,note:60,gain:0},.6,.1,.5,0);
    await superdough({s:'sine',qaId:132,note:60,gain:0,stretch:1},.7,.1,.5,0);
    await context.startRendering();
    return {calls,afterNotes,afterRest,afterSilent,created:held.length,missingError};
  } finally {sound.onTrigger=original;Math.random=random;}
}

const artifacts=process.argv[2]??'/tmp/tompouce-voice-pool-reference';
const browser=await BrowserClient.create();
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function"',20000);
  const result=await browser.evaluate('('+capture.toString()+')()');
  const expected=[{id:0,time:.2-128*.0001+.25},
    {id:1,time:.2-129*.0001+.25},{id:2,time:.55},{id:3,time:.75},
    {id:4,time:.7-.04+.25}];
  assert.equal(result.afterNotes,4,'128-source pool must evict one source per new note');
  assert.equal(result.afterRest,6,'rest must evict before source selection');
  assert.equal(result.afterSilent,6,'rest leaves a free pool slot for the silent source');
  assert.equal(result.created,133,'zero gain still creates a registered source');
  assert.match(result.missingError,/not found/,'missing sound must fail after eviction');
  assert.equal(result.calls.length,expected.length*2);
  expected.forEach(({id,time},index)=>{
    assert.deepEqual(result.calls[index*2],{id,method:'ramp',value:0,time});
    assert.deepEqual(result.calls[index*2+1],{id,method:'stop',time});
  });
  await fs.mkdir(artifacts,{recursive:true});
  await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify({
    url:'https://strudel.cc/',capturedAt:new Date().toISOString(),...result,
  },null,2)+'\n');
  console.log('Web pool: insertion FIFO, 128 sources, target + 250 ms ramp/stop, rest/zero-gain/missing-source behavior verified');
} finally {await browser.close();}
