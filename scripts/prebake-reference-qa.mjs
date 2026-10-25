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
  {name:'branches-solo-anonymous',setup:'',code:'$: note(60); S$: note(64); $: note(67)'},
  {name:'branches-solo-multiple',setup:'',code:'$: note(60); S$: note(64); S$: note(67); $: note(72)'},
  {name:'branches-solo-named',setup:'',code:'drums: note(60); Slead: note(64); Sbass: note(67)'},
  {name:'branches-solo-muted',setup:'',code:'Slead_: note(64); _Sbass: note(67); $: note(60)'},
  {name:'branches-single-S',setup:'',code:'S: note(60); lead: note(64)'},
  {name:'branches-replacement',setup:'',code:"note(60).p('lead'); note(64).p('lead'); note(67).p('$')"},
  {name:'branches-solo-transforms',setup:'',code:'each(x=>x.fast(2)); all(x=>x.add(12)); $: note(60); S$: note(64)'},
  {name:'branches-each-unlabelled',setup:'',code:'each(x=>x.fast(2)); note(60)'},
  {name:'branches-each-last',setup:'',code:'each(x=>x.fast(2)); each(x=>x.fast(3)); note(60)'},
  {name:'branches-id',setup:'',code:"lead: new Pattern(state=>note(60).set({observedId:state.controls.id}).query(state)); bass: new Pattern(state=>note(64).set({observedId:state.controls.id}).query(state))"},
  {name:'utf8-ascii',setup:'',code:"note(60).set({bytes:Array.from(new TextEncoder().encode('tompouce'))})"},
  {name:'utf8-unicode',setup:'',code:"note(60).set({bytes:Array.from(new TextEncoder().encode('café 🎵'))})"},
  {name:'utf8-surrogates',setup:'',code:"note(60).set({bytes:Array.from(new TextEncoder().encode('A\\ud800B\\udc00C'))})"},
  {name:'utf8-encode-into',setup:'',code:"const bytes=new Uint8Array(6).fill(99); const progress=new TextEncoder().encodeInto('é🎵z',bytes.subarray(1)); note(60).set({bytes:Array.from(bytes),progress})"},
  {name:'frequency-note',setup:'',code:"note(60).set({frequency:getFrequencyFromValue({note:'c3'})})",numericTolerance:1e-12},
  {name:'frequency-fallback',setup:'',code:'note(60).set({frequency:getFrequencyFromValue({note:0},48)})',numericTolerance:1e-12},
  {name:'frequency-override',setup:'',code:'note(60).set({frequency:getFrequencyFromValue({note:60,freq:330,octave:1})})',numericTolerance:1e-12},
  {name:'frequency-zero',setup:'',code:'note(60).set({frequency:getFrequencyFromValue({note:60,freq:0,octave:-1})})',numericTolerance:1e-12},
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
for(const test of [
  {name:'orbit-solo',code:"solo(2); stack(note(60).o(1),note(64).o(2))"},
  {name:'orbit-mute',code:"mute(1); stack(note(60).o(1),note(64).o(2))"},
  {name:'orbit-solo-alias',code:"soloOrbit(1,2); stack(note(60).o(1),note(64).o(3))"},
  {name:'orbit-mute-overrides-solo',code:"solo(1,2);mute(2);stack(note(60).o(1),note(64).o(2))"},
  {name:'color',code:'note("60 64").col("0 3")'},
  {name:'cue-select',code:'setCue(1); note("60 64").cue(1)'},
  {name:'cue-transform',code:'setCue(1); note("60 64").cue(1,x=>x.fast(2))'},
  {name:'cue-miss',code:'setCue(2); note("60 64").cue(1)'},
  {name:'cue-pattern',code:'setCue("<0 1>"); note(60).cue(1)',cycles:4},
  {name:'oncue',code:'setCue("<0 1>"); note(60).oncue(1,x=>x.add(7))',cycles:4},
  {name:'cue-global',code:'setCue("<0 1>"); note(cue(60,64))',cycles:4},
  {name:'arrange',code:'ar(2,note("60 64"),2,note("67 72"))',cycles:8},
  {name:'arrange-offset',code:'ar([2,1],note("<60 64 67>"),1,note(72))',cycles:6},
  {name:'tracker',code:'track([note(60),note(64)],2,"1-0",2,"0-1")',cycles:8},
  {name:'blocks',code:'blockArrange([[note(60),"<F 0 B R>"]])',cycles:4},
  {name:'fill',code:'note("60 ~ 64 ~").fill()',cycles:4},
  {name:'grab',code:'n("0 3 6 9 12").grab("c:e:g")'},
  {name:'quad-pan',code:'note(60).mpan("3:4",.7)'},
  {name:'velocity-structure',code:'s("bd*4").vstruct("1 0 .5 0")',controls:['velocity']},
  {name:'postgain',code:'note(60).postgain(.8).pg(.5)',controls:['postgain']},
  {name:'accent',code:'note(60).velocity(.5).lpenv(2).accent(2)',controls:['velocity']},
  {name:'delay',code:'note(60).dly("0 .5 .9 1")',controls:['delay']},
  {name:'colorparty',code:'note(60).colorparty(.5)'},
  {name:'fm-time',code:'note("60*4").fmtime(2,4)',cycles:4},
  {name:'pick',code:'note(pk(60,64,"0 1"))'},
  {name:'random-grid',code:'n(randm(8))',cycles:4},
  {name:'acid',code:'note(60).acid()'},
  {name:'text-ascii',code:"note(60).stxt('tompouce')",controls:['delay'],numericTolerance:1e-12},
  {name:'text-unicode',code:"note(60).stxt('café 🎵')",controls:['delay'],numericTolerance:1e-12},
  {name:'sample-fit',code:"s('bd').sf(2,4)",cycles:4},
  {name:'chord-shapes',code:'chrd("0:1 2:3").sc()'},
  {name:'note-arp',code:'note("c3,e3,g3").notearp("0 1 2 3")'},
  {name:'n-scale',code:"s('sine').nsc('0 2 4',4)"},
  {name:'swap',code:'s("bd sd".swap("bd","hh"))'},
  {name:'over',code:"s('sine').over(n('0 2'),n('4 6')).sc()"},
  {name:'overin',code:"s('sine').overin(n('0 2'),n('4 6')).sc()"},
  {name:'sometimes',code:'note("60*8").sb(.5,x=>x.add(7))',cycles:4},
  {name:'fit',code:"s('bd*4').ifit(2)",cycles:4},
  {name:'trance-arp',code:'n(trancearp("0:2:4",0,0)).sc()',cycles:4},
  {name:'clamp-minmax',code:'n("-2 0 2 4".min(0).max(2))'},
  {name:'flood',code:'note(60).flood("0 .5 1")'},
  {name:'noisehat',code:'noisehat(4)'},
  {name:'zap',code:'zap(.5,.1,"c3")'},
  {name:'parameter-locks',code:'note(60).up("1:.5 0:.25".as("velocity:pan"))',controls:['velocity']},
  {name:'oneshot',code:'note(60).oneshot(4)',cycles:8},
  {name:'filter-values',code:'s("bd hh sd hh").filtval("s","bd",x=>x.pan(.25))'},
  {name:'glide',code:'note("60 67").glide(.5)',cycles:4,numericTolerance:1e-12},
  {name:'strum',code:'note("c3,e3,g3").strum(.05)'},
  {name:'humanize',code:'note("60*4").humanize(.5)',controls:['velocity'],random:true,numericTolerance:1e-12},
  {name:'glitch',code:'note("60 64").pan(.5).glitch(.25)',random:true,numericTolerance:1e-12},
])cases.push({...test,name:`switchangel-${test.name}`,setup:starter,nativeSetup:'// tompouce-prebake: switchangel\n'+starter,
  code:(test.controls?'setGainCurve(x=>x); ':'')+(test.random?'let qaSeed=0xc0ffee; Math.random=()=>((qaSeed=(Math.imul(qaSeed,1664525)+1013904223)>>>0)/4294967296); ':'')+test.code});
const bundle=await fs.readFile('js/dist/runtime.js','utf8');
const native=[];
for(const test of cases) {
  const result=spawnSync(process.env.TOMPOUCE_PREBAKE_BINARY??'./build/tests/test_javascript',
    ['--prebake-reference',test.nativeSetup??test.setup,test.code,String(test.cycles??1)],{encoding:'utf8',timeout:15000,maxBuffer:4*1024*1024});
  native.push(result.status===0 && !/\[query\] error:/.test(result.stderr)?
    {haps:JSON.parse(result.stdout).map(({begin,end,value})=>({begin,end,value}))}:{error:result.stderr||String(result.error||result.status)});
}
const browser=await BrowserClient.create();
const results=[];
let transpilerURL,transpilerHash;
const failures=[];
try {
  for(const [i,test] of cases.entries()) {
    const targetId=await browser.page('https://strudel.cc/');
    try {
    await browser.until('typeof repl==="function"',25000);
    let actual;
    try {actual=await browser.evaluate(`(async()=>{
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
        const haps=[];
        for(let cycle=0;cycle<(test.cycles??1);cycle++)haps.push(...instance.scheduler.pattern.queryArc(cycle,cycle+1,{_cps:.5})
          .filter(h=>h.hasOnset() && Number(h.whole.begin)>=cycle && Number(h.whole.begin)<cycle+1).map(h=>({
          begin:Number(h.whole.begin),end:Number(h.whole.end),value:h.value})));
        return {haps,transpiler:window.__qaTranspiler};
      }finally{instance.stop();}
    })()`);}catch(error){actual={error:String(error)};}
    if(native[i].error || actual.error) {
      failures.push(test.name);results.push({name:test.name,native:native[i],web:actual.error?actual:{haps:actual.haps},passed:false});continue;
    }
    // Native sound defaults/gain curves are prepared at query time; web
    // applies them downstream. Compare musical controls independently here.
    const soundKeys=new Set(['gain','postgain','shapevol','distortvol','delay','velocity','tremolodepth']);
    const musical=haps=>haps.map(h=>({...h,value:Object.fromEntries(Object.entries(h.value).filter(([key])=>!soundKeys.has(key)||test.controls?.includes(key)))}));
    let passed=true;
    const compare=(a,b)=>{
      if(test.numericTolerance && typeof a==='number' && typeof b==='number') {
        assert(Number.isFinite(a) && Number.isFinite(b) && Math.abs(a-b)<=test.numericTolerance);return;
      }
      if(a && b && typeof a==='object' && typeof b==='object') {
        assert.equal(Array.isArray(a),Array.isArray(b));
        assert.deepEqual(Object.keys(a).sort(),Object.keys(b).sort());
        for(const key of Object.keys(a))compare(a[key],b[key]);return;
      }
      assert.deepEqual(a,b);
    };
    try{compare(test.nativeSetup?musical(native[i].haps):native[i].haps,test.nativeSetup?musical(actual.haps):actual.haps);}
    catch{passed=false;failures.push(test.name);}
    transpilerURL=actual.transpiler.url;
    transpilerHash=createHash('sha256').update(actual.transpiler.text).digest('hex');
    results.push({name:test.name,native:native[i],web:actual.haps,passed});
    }finally{await browser.call('Target.closeTarget',{targetId},null);}
  }
}finally{await browser.close();}
const directory=path.resolve(process.argv[2]??'/tmp/tompouce-prebake-reference');
await fs.mkdir(directory,{recursive:true});
await fs.writeFile(path.join(directory,'report.json'),JSON.stringify({url:'https://strudel.cc/',
  capturedAt:new Date().toISOString(),transpilerURL,transpilerHash,
  nativeBundleSha256:createHash('sha256').update(bundle).digest('hex'),
  starterURL,starterSha256:createHash('sha256').update(starter).digest('hex'),cases:results},null,2)+'\n');
assert.deepEqual(failures,[],`prebake parity failures; see ${directory}/report.json`);
console.log(`${results.length} deployed REPL/native prebake cases passed: ${directory}`);
