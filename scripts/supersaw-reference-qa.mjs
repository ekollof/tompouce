/* Actual deployed supersaw worklet, private offline contexts and seeded phases.
   Compare raw sources only: full envelope/effect/onset parity is separate. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {BrowserClient} from './browser-client.mjs';

async function capture(value) {
  const originalAdd=AudioWorklet.prototype.addModule;
  const OriginalNode=window.AudioWorkletNode;
  let source, node;
  AudioWorklet.prototype.addModule=async function(url,...args) {
    const text=await(await fetch(url)).text();
    if (!text.includes('supersaw-oscillator')) return originalAdd.call(this,url,...args);
    source=text;
    const seeded='let __supersawSeed=0xc0ffee; Math.random=()=>{__supersawSeed=(Math.imul(__supersawSeed,1664525)+1013904223)>>>0;return __supersawSeed/4294967296};\n'+text;
    const owned=URL.createObjectURL(new Blob([seeded],{type:'text/javascript'}));
    try {return await originalAdd.call(this,owned,...args);}
    finally {URL.revokeObjectURL(owned);}
  };
  window.AudioWorkletNode=class extends OriginalNode {
    constructor(...args) {super(...args);if(args[1]==='supersaw-oscillator')node=this;}
  };
  try {
    const context=new OfflineAudioContext(2,44100+128,44100);
    setAudioContext(context);setSuperdoughAudioController(null);resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    await superdough(value,0,2,.5,0);
    if (!source || !node) throw Error('No deployed supersaw worklet captured');
    const parameters=Object.fromEntries([...node.parameters].map(([key,param])=>[key,param.value]));
    node.disconnect();
    const gain=context.createGain();
    gain.gain.value=.3/Math.sqrt(Math.min(100,Math.max(1,value.unison??5)));
    node.connect(gain).connect(context.destination);
    const rendered=await context.startRendering();
    const pcm=new Float32Array(rendered.length*2);
    for(let channel=0;channel<2;channel++) {
      const samples=rendered.getChannelData(channel);
      for(let i=0;i<samples.length;i++)pcm[2*i+channel]=samples[i];
    }
    const bytes=new Uint8Array(pcm.buffer);let binary='';
    for(let i=0;i<bytes.length;i+=8192)binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
    const digest=await crypto.subtle.digest('SHA-256',new TextEncoder().encode(source));
    return {parameters,pcm:btoa(binary),workletSha256:[...new Uint8Array(digest)].map(x=>x.toString(16).padStart(2,'0')).join('')};
  } finally {AudioWorklet.prototype.addModule=originalAdd;window.AudioWorkletNode=OriginalNode;}
}

const directory=path.resolve(process.argv[2]??'/tmp/tompouce-supersaw-reference');
const renderer=process.env.TOMPOUCE_RENDER_SUPERSAW??'./build/tests/render_supersaw';
const browser=await BrowserClient.create();
const cases=[
  {name:'defaults',value:{}},
  {name:'unison-one',value:{unison:1,detune:12,spread:1}},
  {name:'mono-zero-detune',value:{unison:7,detune:0,spread:0}},
  {name:'wide',value:{unison:16,detune:4,spread:1}},
  {name:'maximum-voices',value:{unison:100,detune:.18,spread:.6}},
  {name:'fractional-voices',value:{unison:2.5,detune:.3,spread:.4}},
  {name:'n-spread',value:{n:.5}},
  {name:'detune-overrides-n',value:{n:4,detune:.1}},
  {name:'high-frequency',value:{freq:12000,unison:9,detune:36}},
];
try {
  await fs.mkdir(directory,{recursive:true});
  await browser.page('https://strudel.cc/');
  await browser.until('typeof initAudio==="function" && typeof superdough==="function"',20000);
  const results=[];
  for(const spec of cases) {
    const value={s:'supersaw',freq:440,...spec.value};
    const reference=await browser.evaluate(`(${capture.toString()})(${JSON.stringify(value)})`);
    const request=path.join(directory,`${spec.name}.json`),output=path.join(directory,`${spec.name}-native.f32`);
    await fs.writeFile(request,JSON.stringify({begin:0,end:1,value}));
    const run=spawnSync(renderer,[request,output],{encoding:'utf8',timeout:30000});
    assert.equal(run.status,0,run.stderr||String(run.error));
    const native=await fs.readFile(output),web=Buffer.from(reference.pcm,'base64');
    await fs.writeFile(path.join(directory,`${spec.name}-web.f32`),web);
    assert.equal(native.length,web.length);
    let maximum=0,energy=0,error=0;
    for(let i=0;i<native.length;i+=4) {
      const a=native.readFloatLE(i),b=web.readFloatLE(i);
      assert(Number.isFinite(a)&&Number.isFinite(b));
      maximum=Math.max(maximum,Math.abs(a-b));energy+=b*b;error+=(a-b)**2;
    }
    assert(energy>0);
    assert(maximum<3e-5,`${spec.name}: raw PCM error ${maximum}`);
    assert.equal(reference.parameters.frequency,value.freq,'detune must not transpose the base pitch');
    assert.equal(reference.parameters.voices,value.unison??5);
    const result={name:spec.name,value,parameters:reference.parameters,maximum,relativeError:Math.sqrt(error/energy),
      workletSha256:reference.workletSha256,nativeSha256:createHash('sha256').update(native).digest('hex')};
    results.push(result);console.log(`${spec.name}: raw web/native PCM passed (max ${maximum})`);
  }
  await fs.writeFile(path.join(directory,'report.json'),JSON.stringify({url:'https://strudel.cc/',
    capturedAt:new Date().toISOString(),scope:'seeded actual worklet, raw stereo source; excludes ADSR/effects and native onset scheduling',results},null,2)+'\n');
} finally {await browser.close();}
