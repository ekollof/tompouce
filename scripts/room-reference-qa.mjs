/* Deployed web long-room capacity and shared-IR convolution, owned offline audio. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {BrowserClient} from './browser-client.mjs';

async function capture(size) {
  const rate=44100,frames=Math.round(size*1.5*rate);
  const context=new OfflineAudioContext(2,frames+256,rate);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const orbit=getSuperdoughAudioController().getOrbit(1,[1,2]);
  const room=orbit.getReverb(size,.1,15000,1000);
  const deadline=performance.now()+10000;
  while (!room.buffer && performance.now()<deadline) await new Promise(r=>setTimeout(r,10));
  if (!room.buffer) throw Error('Generated web room did not complete');
  if (room.buffer.length!==frames) throw Error('Unexpected web IR duration');
  const encode=buffer=>{
    const pcm=new Float32Array(buffer.length*2);
    for (let c=0;c<2;c++) {
      const samples=buffer.getChannelData(c);
      for (let i=0;i<samples.length;i++) pcm[i*2+c]=samples[i];
    }
    const bytes=new Uint8Array(pcm.buffer); let binary='';
    for (let i=0;i<bytes.length;i+=8192) binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
    return btoa(binary);
  };
  const render=async(ctx,normalize)=>{
    const impulse=ctx.createBuffer(2,1,rate);
    impulse.getChannelData(0)[0]=1; impulse.getChannelData(1)[0]=-.5;
    const source=ctx.createBufferSource(),convolver=ctx.createConvolver();
    convolver.normalize=normalize;
    source.buffer=impulse; convolver.buffer=room.buffer;
    source.connect(convolver); convolver.connect(ctx.destination); source.start(0);
    return ctx.startRendering();
  };
  const rendered=await render(context,false);
  const normalized=await render(new OfflineAudioContext(2,frames+256,rate),true);
  return {size,frames,ir:encode(room.buffer),pcm:encode(rendered),normalized:encode(normalized)};
}

const binary=process.env.TOMPOUCE_RENDER_CONVOLUTION??'./build/tests/render_convolution';
const directory=path.resolve(process.argv[2]??'/tmp/tompouce-long-room-reference');
const browser=await BrowserClient.create();
const hash=buffer=>createHash('sha256').update(buffer).digest('hex');
try {
  await fs.mkdir(directory,{recursive:true});
  await browser.page('https://strudel.cc/');
  await browser.until('typeof initAudio==="function" && typeof superdough==="function"',20000);
  const cases=[];
  for (const size of [4,10]) {
    const result=await browser.evaluate(`(${capture.toString()})(${size})`);
    const ir=Buffer.from(result.ir,'base64'),reference=Buffer.from(result.pcm,'base64');
    const normalizedReference=Buffer.from(result.normalized,'base64');
    const input=path.join(directory,`size-${size}-ir.f32`);
    await fs.writeFile(input,ir);
    await fs.writeFile(path.join(directory,`size-${size}-web.f32`),reference);
    const outputs=[];
    for (const mode of ['c','auto']) {
      const output=path.join(directory,`size-${size}-${mode}.f32`);
      const run=spawnSync(binary,[input,output],{env:{...process.env,TOMPOUCE_SIMD:mode},
        encoding:'utf8',timeout:120000});
      assert.equal(run.status,0,run.stderr||String(run.error));
      const native=await fs.readFile(output); assert.equal(native.length,reference.length);
      let maximum=0,tailEnergy=0;
      for (let i=0;i<native.length;i+=4) {
        const a=native.readFloatLE(i),b=reference.readFloatLE(i);
        assert(Number.isFinite(a)&&Number.isFinite(b));
        maximum=Math.max(maximum,Math.abs(a-b));
        if (i>=Math.floor(size*44100)*8) tailEnergy+=a*a;
      }
      assert(maximum<3e-5,`size ${size} ${mode}: PCM error ${maximum}`);
      assert(tailEnergy>0,`size ${size}: missing late IR tail`);
      const normalizedOutput=path.join(directory,`size-${size}-${mode}-normalized.f32`);
      const normalizedRun=spawnSync(binary,[input,normalizedOutput,'normalized'],{
        env:{...process.env,TOMPOUCE_SIMD:mode},encoding:'utf8',timeout:120000});
      assert.equal(normalizedRun.status,0,normalizedRun.stderr||String(normalizedRun.error));
      const normalizedNative=await fs.readFile(normalizedOutput);
      assert.equal(normalizedNative.length,normalizedReference.length);
      let aa=0,bb=0,ab=0;
      for (let i=0;i<normalizedNative.length;i+=4) {
        const a=normalizedNative.readFloatLE(i),b=normalizedReference.readFloatLE(i);
        assert(Number.isFinite(a)&&Number.isFinite(b)); aa+=a*a; bb+=b*b; ab+=a*b;
      }
      const correlation=ab/Math.sqrt(aa*bb),rmsRatio=Math.sqrt(aa/bb);
      // Same normalized-audio criteria as the maintained audio matrix. The
      // unit-gain comparison above independently gates convolution precision.
      assert(correlation>=.99 && rmsRatio>=.85 && rmsRatio<=1.15);
      outputs.push({mode,maximum,tailEnergy,sha256:hash(native),normalizedSha256:hash(normalizedNative),
        normalized:{correlation,rmsRatio}});
    }
    assert.equal(outputs[0].sha256,outputs[1].sha256,'C/SIMD long-tail PCM mismatch');
    assert.equal(outputs[0].normalizedSha256,outputs[1].normalizedSha256);
    cases.push({size,frames:result.frames,irSha256:hash(ir),webSha256:hash(reference),outputs});
    console.log(`roomsize ${size}: ${result.frames} web IR frames; C/SIMD PCM passed`);
  }
  await fs.writeFile(path.join(directory,'report.json'),JSON.stringify({capturedAt:new Date().toISOString(),
    url:'https://strudel.cc/',scope:'actual generated web IR shared with native convolver; full tails',cases},null,2)+'\n');
} finally { await browser.close(); }
