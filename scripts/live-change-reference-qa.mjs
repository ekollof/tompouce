/* Live engine-operation comparison: transport stop/start, source
   re-evaluation and external tempo changes with fractional sample onsets.
   Both engines run deterministically: the native reference driver advances
   virtual time in exact 128-frame blocks with sample-exact scripted ops,
   and the web live REPL runs on fake time with the same block rule while
   scheduling real pattern audio into a disposable offline context.
   Owned browser profile and fixture sample; no audio device is opened. */
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs/promises';
import path from 'node:path';
import {BrowserClient} from './browser-client.mjs';

const binary=process.env.TOMPOUCE_LIVE_ONSET_BINARY??'./build/tests/test_live_sliders';
const directory=path.resolve(process.argv[2]??'/tmp/tompouce-live-change-reference');
const SR=44100, BLOCK=128;
const scenarios={
  transport:{end:2.0,first:"s('livefixture').fast(4).clip(.25).late(.137)",
    ops:[{sample:Math.round(1.0*SR),kind:'stop'}]},
  source:{end:2.5,first:"s('livefixture').fast(4).clip(.25).late(.137)",
    ops:[{sample:Math.round(1.0*SR),kind:'eval'}]},
  tempo:{end:2.5,first:"s('livefixture').fast(4).clip(.25).late(.137).cps(fastcat(0.5,0.5,1,1))",
    ops:[]},
};
const SECOND="s('livefixture').fast(2).clip(.25).late(.311)";

function fixtureWav() {
  const frames=SR, wav=Buffer.alloc(44+frames*4);
  wav.write('RIFF'); wav.writeUInt32LE(wav.length-8,4); wav.write('WAVEfmt ',8);
  wav.writeUInt32LE(16,16); wav.writeUInt16LE(1,20); wav.writeUInt16LE(2,22);
  wav.writeUInt32LE(SR,24); wav.writeUInt32LE(SR*4,28); wav.writeUInt16LE(4,32); wav.writeUInt16LE(16,34);
  wav.write('data',36); wav.writeUInt32LE(wav.length-44,40);
  for (let i=0;i<frames;i++) {
    const s=Math.trunc(2000*Math.sin(2*Math.PI*440*i/SR));
    wav.writeInt16LE(s,44+i*4); wav.writeInt16LE(s,46+i*4);
  }
  return wav;
}

function correlate(native, browser) {
  assert.equal(native.length,browser.length,'capture length mismatch');
  let dot=0, xx=0, yy=0;
  for (let i=0;i<native.length;i++) { dot+=native[i]*browser[i]; xx+=native[i]*native[i]; yy+=browser[i]*browser[i]; }
  assert(xx>1e-9 && yy>1e-9,'silent capture');
  return {correlation:dot/Math.sqrt(xx*yy),rmsRatio:Math.sqrt(xx/native.length)/Math.sqrt(yy/browser.length)};
}

function parseWav(wav) {
  assert(wav.toString('ascii',0,4)==='RIFF','invalid WAV');
  let channels=0, rate=0, bits=0, format=0, data=null;
  for (let offset=12;offset+8<=wav.length;) {
    const size=wav.readUInt32LE(offset+4), start=offset+8, kind=wav.toString('ascii',offset,offset+4);
    if (kind==='fmt ') { format=wav.readUInt16LE(start); channels=wav.readUInt16LE(start+2); rate=wav.readUInt32LE(start+4); bits=wav.readUInt16LE(start+14); }
    if (kind==='data') data=wav.subarray(start,start+size);
    offset=start+size;
  }
  assert(channels===2 && rate===SR && data && format===1 && bits===16,'unsupported WAV');
  const out=new Float64Array(data.length/4);
  for (let i=0;i<out.length;i++) out[i]=data.readInt16LE(i*2)/32768;
  return out;
}

function parseF32(raw) {
  assert(raw.length%4===0,'truncated native PCM');
  const out=new Float64Array(raw.length/4);
  for (let i=0;i<out.length;i++) out[i]=Buffer.from(raw.subarray(i*4,i*4+4)).readFloatLE(0);
  return out;
}

async function main() {
  await fs.mkdir(directory,{recursive:true});
  const store=path.join(directory,'store','tompouce','sounds');
  await fs.mkdir(path.join(store,'assets'),{recursive:true});
  const asset='a'.repeat(64)+'.wav', wav=fixtureWav();
  await fs.writeFile(path.join(store,'assets',asset),wav);
  await fs.writeFile(path.join(store,'index.json'),
    JSON.stringify({version:1,sounds:{livefixture:[{label:'live.wav',file:asset}]}}));
  const browser=await BrowserClient.create();
  const results=[];
  try {
    await browser.page('https://strudel.cc/');
    await browser.until('typeof superdough==="function" && typeof repl==="function" && typeof webaudioOutput==="function"',30000);
    for (const [name,scenario] of Object.entries(scenarios)) {
      const pcmPath=path.join(directory,`${name}-native.f32`);
      const run=spawnSync(binary,['--live-onset-reference',name,pcmPath],{
        env:{...process.env,TOMPOUCE_LIVE_BANKS:path.join(directory,'store')},
        encoding:'utf8',timeout:120000});
      assert.equal(run.status,0,run.stderr||String(run.error));
      const nativeSummary=JSON.parse(run.stdout.trim());
      const native=parseF32(await fs.readFile(pcmPath));
      const captured=await browser.evaluate(`(async()=>{
        const spec=${JSON.stringify({first:scenario.first,second:SECOND,end:scenario.end,ops:scenario.ops,wav:wav.toString('base64')})};
        const context=new OfflineAudioContext(2,Math.round(spec.end*${SR}),${SR});
        setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
        await initAudio({maxPolyphony:128,multiChannelOrbits:false});
        const bytes=Uint8Array.from(atob(spec.wav),c=>c.charCodeAt(0));
        await samples({livefixture:[URL.createObjectURL(new Blob([bytes],{type:'audio/wav'}))]});
        await getAudioContext().decodeAudioData(bytes.buffer.slice(0));
        let time=0,tick=null;
        const haps=[];
        const host=repl({getTime:()=>time,setInterval:fn=>(tick=fn,1),clearInterval:()=>{tick=null;},
          onSchedule:async(hap,at,cps)=>{haps.push({time:at,fake:time});await webaudioOutput(hap,at,cps);}});
        host.scheduler.setCps(.5);
        /* Evaluate without autostart, then start explicitly: autostart
           scheduling races manual ticks and double-schedules the first
           window. Mid-run swaps stay non-autostart for the same reason. */
        await host.evaluate(spec.first,false);
        if (host.state.evalError) throw host.state.evalError;
        host.start();
        await new Promise(resolve=>setTimeout(resolve,500));
        const ops=spec.ops.map(op=>({...op,done:false}));
        const total=Math.round(spec.end*${SR});
        for (let done=0;done<total;done+=${BLOCK}) {
          for (const op of ops) if (!op.done && done>=op.sample) {
            op.done=true;
            if (op.kind==='stop') host.stop();
            else if (op.kind==='eval') await host.evaluate(spec.second,false);
            if (host.state.evalError) throw host.state.evalError;
          }
          /* A cleared interval stays dead, exactly as in production: after
             Stop no further scheduling passes run. */
          if (tick!==null) { time=done/${SR}; tick(); }
          await new Promise(resolve=>setTimeout(resolve,0));
        }
        await new Promise(resolve=>setTimeout(resolve,500));
        const rendered=await context.startRendering();
        const encode=(data)=>{let binary=''; const u8=new Uint8Array(data.buffer);
          for (let i=0;i<u8.length;i+=8192) binary+=String.fromCharCode(...u8.subarray(i,i+8192));
          return btoa(binary);};
        return {left:encode(rendered.getChannelData(0)),right:encode(rendered.getChannelData(1)),
          haps,sampleRate:rendered.sampleRate,cps:host.scheduler.cps};
      })()`);
      /* Browser channels arrive as base64 Float32 PCM; interleave stereo. */
      const decode=(text)=>{const bytes=Uint8Array.from(atob(text),c=>c.charCodeAt(0));
        const view=new DataView(bytes.buffer), out=new Float64Array(bytes.length/4);
        for (let i=0;i<out.length;i++) out[i]=view.getFloat32(i*4,true);
        return out;};
      const left=decode(captured.left), right=decode(captured.right);
      assert.equal(left.length*2,native.length,'capture length mismatch');
      const ints=new Float64Array(native.length);
      for (let i=0;i<left.length;i++) { ints[i*2]=left[i]; ints[i*2+1]=right[i]; }
      await fs.writeFile(path.join(directory,`${name}-browser.json`),
        JSON.stringify({haps:captured.haps,cps:captured.cps},null,2));
      const metrics=correlate(native,ints);
      const hold=metrics.correlation>=.99&&metrics.rmsRatio>=.85&&metrics.rmsRatio<=1.15;
      results.push({name,frames:nativeSummary.frames,voices:nativeSummary.voices,
        webHaps:captured.haps.length,webCps:captured.cps,...metrics,hold});
      console.log(name,JSON.stringify(metrics),hold?'PASS':'FAIL',
        `native voices=${nativeSummary.voices} web haps=${captured.haps.length}`);
    }
  } finally { await browser.close(); }
  const gatePassed=results.every(result=>result.hold);
  await fs.writeFile(path.join(directory,'report.json'),JSON.stringify(
    {url:'https://strudel.cc/',capturedAt:new Date().toISOString(),gatePassed,results},null,2)+'\n');
  console.log(`Live engine changes: ${results.filter(r=>r.hold).length}/${results.length} hold; ${path.join(directory,'report.json')}`);
  if (!gatePassed) process.exitCode=1;
}

await main();
