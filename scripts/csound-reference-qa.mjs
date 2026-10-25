/* Deployed Strudel loaders and Csound WASM. Manual kernel rendering uses
   private OfflineAudioContexts whose destinations never start. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {BrowserClient} from './browser-client.mjs';

async function reference() {
  const resources=performance.getEntriesByType('resource').map(e=>e.name).filter(url=>url.endsWith('.js'));
  let moduleURL,loaderSource;
  for (const url of resources) {
    const text=await(await fetch(url)).text();
    if (!text.includes('evalCode(n)')) continue;
    const match=text.match(/import\("([^"]*csound[^"]*\.js)"\)/);
    if (match) {moduleURL=new URL(match[1],url).href;loaderSource=text;break;}
  }
  if (!moduleURL) throw Error('Cannot identify deployed Csound module');
  const {Csound}=await import(moduleURL), rate=48000, count=38400;
  const context=new OfflineAudioContext(2,rate,rate), baselineContext=new OfflineAudioContext(2,rate,rate);
  const engine=await Csound({audioContext:context,useWorker:false,useSPN:true});
  const baseline=await Csound({audioContext:baselineContext,useWorker:false,useSPN:true});
  const messages=[]; engine.on('message',message=>messages.push(String(message)));
  const setup='sr=48000\nksmps=64\nnchnls=2\n0dbfs=1\n'+
    'giLeft ftgen 1,0,64,-2,0\ngiRight ftgen 2,0,64,-2,0\n'+
    'instr 32000\naL,aR monitor\naIndex phasor sr/64\ntablew aL,aIndex,1,1\ntablew aR,aIndex,2,1\nendin\n';
  for (const instance of [engine,baseline]) {
    if (!((await instance.getAudioContext()) instanceof OfflineAudioContext)) throw Error('Refuse real audio output');
    await instance.setOption('-odac'); await instance.setOption('-m0d'); await instance.setOption('--sample-accurate');
    if (await instance.compileOrc(setup)) throw Error('Reference monitor compilation failed');
    if (await instance.start()) throw Error('Reference engine start failed');
  }
  window.__csound__=engine;
  setAudioContext(context);
  const source='giRetained init 2\ninstr Held\na1 oscili p5,p4\na0 init 0\nout a1,a0\nendin\n';
  await loadCsound(source); if (await baseline.compileOrc(source)) throw Error('Baseline compilation failed');
  for (const instance of [engine,baseline]) {
    await instance.inputMessage('i 32000 0 1');
    await instance.inputMessage('i "Held" .02 .8 441 .1');
  }
  const actualLeft=[],controlLeft=[],right=[];
  const other='giRetained += 1\ninstr Other\na1 = p5*giRetained\na0 init 0\nout a0,a1\nendin\n';
  // This browser package's tableCopyOut frees its scratch allocation before
  // cloning the view, corrupting initial samples. Read the actual table cells.
  const snapshot=(instance,table)=>Promise.all(Array.from({length:64},(_,i)=>instance.tableGet(table,i)));
  let invalidRejected=false, invalidMessages=0;
  for (let frame=0;frame<count;frame+=64) {
    if (frame===4096) {
      await loadCsound('instr Held\na1 oscili p5,2*p4\na0 init 0\nout a0,a1\nendin\n');
      await engine.inputMessage('i "Held" '+(.4-frame/rate)+' .1 441 .1');
    }
    if (frame===8192) {
      const prior=messages.length;
      try {await loadCsound('giRetained += 100\ninvalid orchestra');} catch {invalidRejected=true;}
      invalidMessages=messages.length-prior;
      await loadCsound(other);
      await engine.inputMessage('i "Other" '+(.6-frame/rate)+' .05 441 .1');
      await loadCsound();
    }
    if (frame===30464) {
      await loadCsound(other);
      await engine.inputMessage('i "Other" '+(.69-frame/rate)+' .03 441 .1');
    }
    if (await engine.performKsmps() || await baseline.performKsmps()) throw Error('Reference performance stopped');
    actualLeft.push(...await snapshot(engine,1));
    controlLeft.push(...await snapshot(baseline,1));
    right.push(...await snapshot(engine,2));
  }
  let difference=0,leftEnergy=0,rightEnergy=0,crossings=0,first=-1;
  for (let i=0;i<count;i++) {
    difference=Math.max(difference,Math.abs(actualLeft[i]-controlLeft[i]));
    leftEnergy+=actualLeft[i]**2;
    if (first<0 && Math.abs(actualLeft[i])>1e-7) first=i;
    if (i>=.43*rate && i<.48*rate) {
      rightEnergy+=right[i]**2;
      if (right[i-1]<0 && right[i]>=0) crossings++;
    }
  }
  const mean=(begin,end)=>right.slice(Math.round(begin*rate),Math.round(end*rate)).reduce((a,b)=>a+b,0)/Math.round((end-begin)*rate);
  let fetches=0,failedFetches=0,invalidFetches=0;
  const originalFetch=window.fetch;
  window.fetch=async(url,...args)=>{
    if(String(url)==='https://raw.githubusercontent.com/fixture/orchestra/main/tone.orc') {
      fetches++;return new Response('instr Cached\na1 oscili p5,p4\nout a1,a1\nendin\n');
    }
    if(String(url)==='https://raw.githubusercontent.com/fixture/orchestra/main/invalid.orc') {
      invalidFetches++;return new Response('giRetained += 100\ninvalid orchestra');
    }
    if(String(url)==='https://raw.githubusercontent.com/fixture/orchestra/main/missing.orc') {
      failedFetches++;throw new TypeError('owned fixture fetch failure');
    }
    return originalFetch(url,...args);
  };
  await loadOrc('github:fixture/orchestra/main/tone.orc');
  await loadOrc('https://raw.githubusercontent.com/fixture/orchestra/main/tone.orc');
  const priorInvalid=messages.length;
  let invalidURLRejected=false,failedRejections=0;
  try{await loadOrc('github:fixture/orchestra/main/invalid.orc');}catch{invalidURLRejected=true;}
  const invalidURLMessages=messages.length-priorInvalid;
  await loadOrc('https://raw.githubusercontent.com/fixture/orchestra/main/invalid.orc');
  const cachedInvalidMessages=messages.length-priorInvalid-invalidURLMessages;
  for(const url of ['github:fixture/orchestra/main/missing.orc','https://raw.githubusercontent.com/fixture/orchestra/main/missing.orc']) {
    try{await loadOrc(url);}catch{failedRejections++;}
  }
  const host=repl({getTime:()=>0,onSchedule:()=>{},setInterval:()=>1,clearInterval:()=>{}});
  await host.evaluate('(async()=>{await loadCsound("invalid orchestra");return note(72).csound("Held")})()',false);
  const invalidEvaluationAccepted=!host.state.evalError &&
    Number(host.scheduler.pattern.queryArc(0,1)[0].value.note)===72;
  await host.evaluate('(async()=>{await loadCsound("giEvalRetained init 7");throw Error("owned evaluation failure")})()',false);
  const laterThrowRejected=!!host.state.evalError;
  const retainedAfterThrow=await engine.evalCode('return giEvalRetained');
  await host.evaluate('(async()=>{try{await loadOrc("github:fixture/orchestra/main/missing.orc")}catch{}return note(73).csound("Held")})()',false);
  const caughtFetchAccepted=!host.state.evalError &&
    Number(host.scheduler.pattern.queryArc(0,1)[0].value.note)===73;
  window.fetch=originalFetch;
  host.stop();
  return JSON.stringify({moduleURL,loaderSource,version:String(await engine.getVersion()),
    name:engine.name,frames:count,rate,difference,leftEnergy,rightEnergy,crossings,first,
    initialGlobal:mean(.62,.63),repeatedGlobal:mean(.71,.715),fetches,invalidRejected,invalidMessages,invalidEvaluationAccepted,
    failedFetches,failedRejections,invalidFetches,invalidURLRejected,invalidURLMessages,cachedInvalidMessages,
    laterThrowRejected,retainedAfterThrow,caughtFetchAccepted,
    contextState:context.state,baselineContextState:baselineContext.state});
}

const browser=await BrowserClient.create();
const artifacts=path.resolve(process.argv[2]??'/tmp/tompouce-csound-reference-qa');
try {
  await browser.page('https://strudel.cc/');
  await browser.until("typeof loadCsound==='function' && typeof loadOrc==='function'",20000);
  const result=JSON.parse(await browser.evaluate('('+reference.toString()+')()'));
  console.log(JSON.stringify({...result,loaderSource:undefined}));
  assert.equal(result.contextState,'suspended'); assert.equal(result.baselineContextState,'suspended');
  assert(result.difference<1e-8,'orchestra update changed held Csound PCM');
  assert(result.leftEnergy>1 && result.rightEnergy>.1);
  assert(result.crossings>=43 && result.crossings<=45);
  assert(Math.abs(result.initialGlobal-.3)<1e-6 && Math.abs(result.repeatedGlobal-.4)<1e-6);
  assert.equal(result.first,Math.round(.02*result.rate)+1);
  assert.equal(result.fetches,1);
  assert.equal(result.failedFetches,1); assert.equal(result.failedRejections,2);
  assert.equal(result.invalidFetches,1); assert.equal(result.invalidURLRejected,false);
  assert(result.invalidURLMessages>0); assert.equal(result.cachedInvalidMessages,0);
  assert.equal(result.invalidRejected,false); assert(result.invalidMessages>0);
  assert.equal(result.invalidEvaluationAccepted,true);
  assert.equal(result.laterThrowRejected,true);
  assert.equal(result.retainedAfterThrow,7);
  assert.equal(result.caughtFetchAccepted,true);
  result.loaderSha256=createHash('sha256').update(result.loaderSource).digest('hex'); delete result.loaderSource;
  await fs.mkdir(artifacts,{recursive:true});
  await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify({url:'https://strudel.cc/',capturedAt:new Date().toISOString(),...result},null,2)+'\n');
  console.log('Live Csound WASM loader/held-PCM lifecycle verified: '+artifacts);
} finally {await browser.close();}
