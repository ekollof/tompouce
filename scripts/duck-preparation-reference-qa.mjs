/* Deployed pre-source duck ordering, disposable browser and offline audio. */
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

async function capture() {
  const context=new OfflineAudioContext(2,44100,44100);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const target=getSuperdoughAudioController().getOrbit(2,[1,2]);
  const sound=getSound('sine'),original=sound.onTrigger,duck=target.duck;
  let calls=0,entered=false,release;
  const gate=new Promise(resolve=>{release=resolve;});
  target.duck=(...args)=>{calls++;return duck.apply(target,args);};
  sound.onTrigger=async(...args)=>{entered=true;await gate;return original(...args);};
  try {
    const pending=superdough({s:'sine',note:60,orbit:1,duckorbit:2,duckdepth:.5},.5,.1,.5,0);
    await Promise.resolve(); await Promise.resolve();
    const result={sourceEntered:entered,duckCallsWhileSourceBlocked:calls};
    release(); await pending; await context.startRendering();
    return result;
  } finally {sound.onTrigger=original;target.duck=duck;release();}
}

const browser=await BrowserClient.create();
let web;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function" && typeof getSound==="function"',20000);
  web=await browser.evaluate('('+capture.toString()+')()');
} finally {await browser.close();}
assert.deepEqual(web,{sourceEntered:true,duckCallsWhileSourceBlocked:1});
const native=spawnSync(process.env.TOMPOUCE_LIVE_CONTROLS_BINARY??'./build/tests/test_live_sliders',
  ['--duck-preparation-reference'],{encoding:'utf8',timeout:30000,maxBuffer:1024*1024});
assert.equal(native.status,0,native.stderr||String(native.error));
assert.deepEqual(JSON.parse(native.stdout),{duckBeforeSource:true,survivesReplacement:true});
console.log('Web/native duck controls precede blocked source preparation; native accepted callback survives replacement');
