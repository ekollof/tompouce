/* Diagnostic only: trusted corpus sources in Node's V8. node:vm is NOT a
   security boundary or a proposed production host. No native accelerators. */
import vm from 'node:vm';
import fs from 'node:fs';
import {performance} from 'node:perf_hooks';

const bundle=fs.readFileSync(new URL('../js/dist/runtime.js',import.meta.url),'utf8');
if (!vm.constants?.DONT_CONTEXTIFY) throw Error('V8 assessment requires Node with vm.constants.DONT_CONTEXTIFY');
const boot=new vm.Script(bundle,{filename:'tompouce-runtime.js'});
const compile=new vm.Script(`globalThis.__compileResult=undefined;globalThis.__compileError=undefined;
  __compile(__source,__bpm).then(x=>{__compileResult=x},e=>{__compileError=String(e)});`);
const query=new vm.Script('__query(__begin,__end,__cps)');
const prelude=`globalThis.performance={now:()=>Date.now()};
  globalThis.console={log(){},warn(){},error(){},info(){},debug(){}};
  globalThis.__hostLog=()=>{};
  globalThis.__hostSlider=v=>v;
  globalThis.__probeSliders=[];
  globalThis.__hostSliderBind=v=>(__probeSliders.push(v)-1);
  globalThis.__hostSliderRead=i=>__probeSliders[i];`;

export function createProbe() {
  const started=performance.now();
  const context=vm.createContext(vm.constants.DONT_CONTEXTIFY,{microtaskMode:'afterEvaluate',codeGeneration:{strings:true,wasm:false}});
  new vm.Script(prelude).runInContext(context,{timeout:2000});
  boot.runInContext(context,{timeout:2000});
  const bootstrapMs=performance.now()-started;
  return {context,bootstrapMs,
    compile(code,bpm=120) {
      if (typeof code!=='string'||Buffer.byteLength(code)>4*1024*1024) throw Error('source limit');
      context.__source=code;context.__bpm=bpm;
      const t=performance.now();compile.runInContext(context,{timeout:2000});
      if (context.__compileError) throw Error(context.__compileError);
      if (typeof context.__compileResult!=='string') throw Error('compile did not settle without external I/O');
      return {ms:performance.now()-t,metadata:JSON.parse(context.__compileResult)};
    },
    query(begin,end,cps=.5,timeout=200) {
      context.__begin=begin;context.__end=end;context.__cps=cps;
      const started=performance.now();
      const json=query.runInContext(context,{timeout});
      const vmMs=performance.now()-started;
      if (typeof json!=='string'||Buffer.byteLength(json)>8*1024*1024) throw Error('event output limit');
      const events=JSON.parse(json);
      return {events,vmMs,ms:performance.now()-started,started};
    },
    run(source,timeout=200) {return new vm.Script(source).runInContext(context,{timeout});},
  };
}
