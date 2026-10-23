/* Setup files against the deployed REPL and its actual transpiler. No audio
   device is opened. Native uses a fresh adapter realm per compiled program;
   the C security gate separately exercises the confined file reader. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {BrowserClient} from './browser-client.mjs';

const cases=[
  {name:'registered-method',setup:"register('preTwice',pat=>pat.fast(2))",code:'note("60 64").preTwice()'},
  {name:'global-closure',setup:'const root=60; window.preLead=()=>note(root);',code:'preLead().fast(2)'},
  {name:'mini-helper',setup:'window.preMelody=()=>note("60 64")',code:'preMelody()'},
  {name:'async-setup',setup:'await Promise.resolve(); window.preRoot=72;',code:'note(preRoot)'},
  {name:'prototype-helper',setup:'Pattern.prototype.preRepeat=function(){return this.fast(3)}',code:'note(60).preRepeat()'},
  {name:'setup-pattern-discarded',setup:'note(99); window.preRoot=72;',code:'note(preRoot)'},
];
const starterURL='https://raw.githubusercontent.com/switchangel/strudel-scripts/706dd24814ac86193ceec6d0b263f432cba6e5ff/prebake.strudel';
const response=await fetch(starterURL);
assert(response.ok,'starter fixture download failed');
const starter=await response.text();
assert(Buffer.byteLength(starter)<256*1024);
for(const [name,code] of [
  ['acidenv',"note('c3').acidenv(.4)"],
  ['trancegate',"note('c3').trancegate(.4,3,1)"],
  ['tgate',"note('c3').tgate(.5,2,1)"],
  ['filter',"note('c3').rlpf(.5).rhpf(.1)"],
  ['octave',"note('c3').octave(4)"],
  ['scale',"setScale('e:minor'); n(0).sc()"],
  ['global-helper','DX(4,2,2)'],
])cases.push({name:`switchangel-${name}`,setup:starter,nativeSetup:'// tompouce-prebake: switchangel\n'+starter,code});
const bundle=await fs.readFile('js/dist/runtime.js','utf8');
const native=[];
for(const test of cases) {
  const result=spawnSync(process.env.TOMPOUCE_PREBAKE_BINARY??'./build/tests/test_javascript',
    ['--prebake-reference',test.nativeSetup??test.setup,test.code],{encoding:'utf8',timeout:15000,maxBuffer:4*1024*1024});
  assert.equal(result.status,0,result.stderr);
  native.push(JSON.parse(result.stdout).map(({begin,end,value})=>({begin,end,value})));
}
const browser=await BrowserClient.create();
const results=[];
let transpilerURL,transpilerHash;
try {
  for(const [i,test] of cases.entries()) {
    await browser.page('https://strudel.cc/');
    await browser.until('typeof repl==="function"',25000);
    const actual=await browser.evaluate(`(async()=>{
      let transpile;
      const urls=performance.getEntriesByType('resource').map(e=>e.name).filter(u=>u.endsWith('.js'));
      for(const url of urls){
        const text=await(await fetch(url)).text();
        if(!text.includes('emitMiniLocations'))continue;
        const exports=await import(url);
        transpile=Object.values(exports).find(fn=>typeof fn==='function' && String(fn).includes('addReturn'));
        if(transpile){window.__qaTranspiler={url,text};break;}
      }
      if(!transpile)throw Error('deployed transpiler not found');
      const context=new OfflineAudioContext(2,44100,44100);
      setAudioContext(context);
      const test=${JSON.stringify(test)};
      const instance=repl({getTime:()=>0,onSchedule:()=>{},setInterval:()=>1,clearInterval:()=>{},
        transpiler:transpile});
      try {
        // The page's mirror runs this setup evaluator before core REPL eval;
        // core repl() itself does not expose the mirror's prebake callback.
        await evaluate(test.setup,transpile,{addReturn:false});
        await instance.evaluate(test.code,false);
        if(instance.state.evalError)throw instance.state.evalError;
        if(context.state!=='suspended')throw Error('reference context unexpectedly rendered');
        return {haps:instance.scheduler.pattern.queryArc(0,1).filter(h=>h.hasOnset()).map(h=>({
          begin:Number(h.whole.begin),end:Number(h.whole.end),value:h.value})),transpiler:window.__qaTranspiler};
      }finally{instance.stop();}
    })()`);
    // Native sound defaults/gain curves are prepared at query time; web
    // applies them downstream. Compare musical controls independently here.
    const soundKeys=new Set(['gain','postgain','shapevol','distortvol','delay','velocity','tremolodepth']);
    const musical=haps=>haps.map(h=>({...h,value:Object.fromEntries(Object.entries(h.value).filter(([key])=>!soundKeys.has(key)))}));
    assert.deepEqual(test.nativeSetup?musical(native[i]):native[i],test.nativeSetup?musical(actual.haps):actual.haps,test.name);
    transpilerURL=actual.transpiler.url;
    transpilerHash=createHash('sha256').update(actual.transpiler.text).digest('hex');
    results.push({name:test.name,haps:actual.haps,passed:true});
  }
}finally{await browser.close();}
const directory=path.resolve(process.argv[2]??'/tmp/tompouce-prebake-reference');
await fs.mkdir(directory,{recursive:true});
await fs.writeFile(path.join(directory,'report.json'),JSON.stringify({url:'https://strudel.cc/',
  capturedAt:new Date().toISOString(),transpilerURL,transpilerHash,
  nativeBundleSha256:createHash('sha256').update(bundle).digest('hex'),
  starterURL,starterSha256:createHash('sha256').update(starter).digest('hex'),cases:results},null,2)+'\n');
console.log(`${results.length} deployed REPL/native prebake cases passed: ${directory}`);
