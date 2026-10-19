/* Actual source-node gain automation, with silent offline contexts only. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {BrowserClient} from './browser-client.mjs';

async function capture(sound) {
  const context=new OfflineAudioContext(2,44100,44100);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:1,multiChannelOrbits:false});
  const entry=getSound(sound),original=entry.onTrigger,held=[],automation=[];
  const methods=['setValueAtTime','linearRampToValueAtTime','cancelScheduledValues'];
  const originals=Object.fromEntries(methods.map(method=>[method,AudioParam.prototype[method]]));
  let parameter;
  for (const method of methods) AudioParam.prototype[method]=function(...args) {
    automation.push({parameter:this,method,args});return originals[method].apply(this,args);
  };
  entry.onTrigger=async(...args)=>{
    const result=await original(...args); held.push(result);
    if (held.length===1) {
      parameter=result.node.gain;
      // Replace only the waveform: retain the real registered source's gain
      // automation and its place in Superdough's eviction Map.
      result.nodes.source.forEach(node=>node.disconnect());
      const dc=context.createConstantSource(); dc.offset.value=1;
      dc.connect(result.node); dc.start(.1); dc.stop(.9);
      const stop=result.stop.bind(result);
      result.stop=time=>{stop(time);dc.stop(time);};
    }
    return result;
  };
  try {
    await superdough({s:sound,note:60,gain:.1,postgain:1,
      attack:.02,decay:.03,sustain:.6,release:.1,clip:1},.1,.7,.5,0);
    const suspended=context.suspend(8960/44100),rendered=context.startRendering();
    await suspended;
    const preparedAt=context.currentTime;
    await superdough({s:sound,note:60,gain:0},.3,.4,.5,0);
    await context.resume();
    const pcm=(await rendered).getChannelData(0);
    const points=[.12,.15,.2,.21,.25,.3,.4,.5,.549,.55,.56].map(time=>{
      const frame=Math.round(time*44100);return {time:frame/44100,gain:pcm[frame]/.1};
    });
    return {sound,preparedAt,points,automation:automation.filter(e=>e.parameter===parameter)
      .map(({method,args})=>({method,args}))};
  } finally {
    entry.onTrigger=original;
    for (const method of methods) AudioParam.prototype[method]=originals[method];
  }
}

const results=[];
for (const sound of ['sine','piano']) {
  const browser=await BrowserClient.create();
  try {
    await browser.page('https://strudel.cc/');
    await browser.until('typeof superdough==="function"',20000);
    const result=await browser.evaluate('('+capture.toString()+')('+JSON.stringify(sound)+')');
    assert(result.points.slice(-2).every(point=>Math.abs(point.gain)<1e-6),'evicted source must stop at target + .25');
    for (const point of result.points) {
      let expected;
      if (point.time>=.55) expected=0;
      else if (point.time>=result.preparedAt)
        expected=sound==='sine'?.6*(.55-point.time)/(.55-.15):(.55-point.time)/(.55-result.preparedAt);
      else if (sound==='piano') expected=1;
      else if (point.time<=.12) expected=(point.time-.1)/.02;
      else if (point.time<.15) expected=1-.4*(point.time-.12)/.03;
      else expected=.6;
      assert(Math.abs(point.gain-expected)<2e-6,`${sound} gain at ${point.time}`);
    }
    results.push(result);
  } finally {await browser.close();}
}
const artifacts=process.argv[2]??'/tmp/tompouce-voice-eviction-reference';
await fs.mkdir(artifacts,{recursive:true});
await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify(results,null,2)+'\n');
console.log(JSON.stringify(results));
