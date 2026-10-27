/* Owned diagnostic child processes only. No engine, audio device or production
   backend is changed. vm contexts here execute only the fixed public corpus. */
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {performance,PerformanceObserver} from 'node:perf_hooks';
import v8 from 'node:v8';
import vm from 'node:vm';
import os from 'node:os';
import {createHash} from 'node:crypto';
import {createProbe} from './v8-probe-runtime.mjs';

const corpus=JSON.parse(fs.readFileSync(new URL('../tests/fixtures/strudel-bakery.json',import.meta.url)));
const stats=xs=>{
  const a=[...xs].sort((x,y)=>x-y),q=p=>a[Math.max(0,Math.ceil(a.length*p)-1)]??null;
  return {count:a.length,minMs:a[0]??null,medianMs:q(.5),p95Ms:q(.95),p99Ms:q(.99),maxMs:a.at(-1)??null,
    over200:xs.filter(x=>x>200).length};
};
async function benchmark({id,whole=96,windows=512,pressure=32,timeout=200,playbackFirst=false}) {
  if (!Number.isInteger(whole)||whole<1||whole>256||!Number.isInteger(windows)||windows<1||windows>2048||
      !Number.isInteger(pressure)||pressure<0||pressure>128||![200,1000].includes(timeout)) throw Error('probe bounds');
  const example=corpus.examples.find(e=>Number(e.id)===id);
  if (!example||createHash('sha256').update(example.code).digest('hex')!==example.sha256) throw Error('corpus source hash');
  const gc=[];
  const observer=new PerformanceObserver(list=>{for (const e of list.getEntries())
    gc.push({start:e.startTime,ms:e.duration,kind:e.detail?.kind??null});});
  observer.observe({entryTypes:['gc']});
  const probe=createProbe(),compiled=probe.compile(example.code);
  const cps=Number.isFinite(compiled.metadata.bpm)&&compiled.metadata.bpm>0?compiled.metadata.bpm/240:.5;
  const result={id,sha256:example.sha256,bootstrapMs:probe.bootstrapMs,compileMs:compiled.ms,cps,
    budgetMs:timeout,playbackFirst,heapLimit:v8.getHeapStatistics().heap_size_limit,phases:{}};
  async function phase(name,count,width) {
    const samples=[],eventCounts=[],spans=[],start=performance.now();let failure,failureMs;
    for (let i=0;i<count;i++) {
      const begin=width===1?i%32:i*width;
      const attempt=performance.now();
      try {
        const sample=probe.query(begin,begin+width,cps,timeout);
        samples.push(sample.ms);eventCounts.push(sample.events.length);
        spans.push([sample.started,sample.started+sample.ms]);
      } catch(error) {failure=String(error);failureMs=performance.now()-attempt;spans.push([attempt,performance.now()]);break;}
    }
    const finish=performance.now();
    await new Promise(setImmediate);
    result.phases[name]={...stats(samples),firstMs:samples[0]??null,
      attempts:samples.length+(failure?1:0),deadlineTimeouts:failure?.includes('timed out')?1:0,
      minEvents:eventCounts.length?Math.min(...eventCounts):null,maxEvents:eventCounts.length?Math.max(...eventCounts):null,failure,failureMs,
      start,finish,spans,
      memory:process.memoryUsage(),maxRssKiB:process.resourceUsage().maxRSS};
    return !failure;
  }
  const first=playbackFirst?['playbackWindows',windows,.05*cps]:['wholeCycles',whole,1];
  const second=playbackFirst?['wholeCycles',whole,1]:['playbackWindows',windows,.05*cps];
  if (await phase(...first)) {
    if (await phase(...second)) {
      // Artificial retained managed-heap pressure, separate from parity inputs.
      probe.run('globalThis.__pressure=Array.from({length:64},()=>new Array(65536).fill(1))',3000);
      await phase('retained32MiB',pressure,1);
    }
  }
  await new Promise(setImmediate);await new Promise(setImmediate);
  for (const e of observer.takeRecords()) gc.push({start:e.startTime,ms:e.duration,kind:e.detail?.kind??null});
  observer.disconnect();
  for (const phase of Object.values(result.phases)) {
    const collections=gc.filter(e=>e.start>=phase.start&&e.start<phase.finish);
    phase.gc=stats(collections.map(e=>e.ms));
    phase.gcKinds=collections.reduce((a,e)=>(a[e.kind]=(a[e.kind]??0)+1,a),{});
    phase.gcDuringQueries=collections.filter(e=>phase.spans.some(([b,e2])=>e.start>=b&&e.start<e2)).length;
    delete phase.start;delete phase.finish;delete phase.spans;
  }
  result.gc=stats(gc.map(e=>e.ms));return result;
}
function safety(kind) {
  const context=vm.createContext(vm.constants.DONT_CONTEXTIFY,{microtaskMode:'afterEvaluate',codeGeneration:{strings:true,wasm:false}});
  const run=(code,timeout=200)=>new vm.Script(code).runInContext(context,{timeout});
  if (kind==='oom') {run('globalThis.held=[];for(;;)held.push(new Array(100000).fill(1))',5000);return {};}
  if (kind==='external') {
    run('globalThis.held=Array.from({length:24},()=>new Uint8Array(4*1024*1024));for(const x of held)x.fill(1)',3000);
    return {heapLimit:v8.getHeapStatistics().heap_size_limit,memory:process.memoryUsage()};
  }
  const code=kind==='microtask'?'Promise.resolve().then(()=>{while(true){}})':
    kind==='chain'?'function again(){Promise.resolve().then(again)};again()':
    kind==='atomics'?'Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0)':'while(true){}';
  const start=performance.now();let error;
  try {run(code);}catch(e){error={code:e.code,message:e.message};}
  const ms=performance.now()-start;let recovery,recoveryError;
  try {recovery=run('6*7');}catch(e){recoveryError=String(e);}
  return {ms,error,recovery,recoveryError,
    globals:run('JSON.stringify({process:typeof process,require:typeof require,fetch:typeof fetch})')};
}
if (process.argv[2]==='--worker') {
  try {
    const spec=JSON.parse(process.argv[3]);
    const result=spec.safety?safety(spec.safety):await benchmark(spec);
    process.stdout.write(JSON.stringify(result)+'\n');
  } catch(error) {process.stderr.write(String(error)+'\n');process.exitCode=1;}
} else {
  const directory=path.resolve(process.argv[2]??'/tmp/tompouce-v8-assessment');fs.mkdirSync(directory,{recursive:true});
  const report={node:process.version,v8:process.versions.v8,platform:process.platform,arch:process.arch,
    availableParallelism:os.availableParallelism(),contextKind:'DONT_CONTEXTIFY',
    v8SandboxEnabled:process.config.variables.v8_enable_sandbox,pointerCompressionEnabled:process.config.variables.v8_enable_pointer_compression,
    diagnosticOnly:true,nativeHostBridgeTested:false,benchmarks:[],safety:[]};
  const affinity=fs.readFileSync('/proc/self/status','utf8').match(/^Cpus_allowed_list:\s*(\d+)/m)?.[1];
  function child(spec,{jitless=false,oneCpu=false,old=104}={}) {
    const args=['--max-old-space-size='+old,'--max-semi-space-size=8',...(jitless?['--jitless']:[]),
      fileURLToPath(import.meta.url),'--worker',JSON.stringify(spec)];
    let executable=process.execPath,argv=args;
    const pinned=!!(oneCpu&&affinity&&fs.existsSync('/usr/bin/taskset'));
    if (pinned) {executable='/usr/bin/taskset';argv=['-c',affinity,process.execPath,...args];}
    if (spec.safety==='oom'&&!fs.existsSync('/usr/bin/prlimit')) return {spec,skipped:'prlimit required to disable diagnostic core dumps'};
    if (fs.existsSync('/usr/bin/prlimit')) {argv=['--core=0','--',executable,...argv];executable='/usr/bin/prlimit';}
    const r=spawnSync(executable,argv,{encoding:'utf8',timeout:spec.safety?5000:120000,maxBuffer:2*1024*1024});
    let data;try{data=JSON.parse(r.stdout);}catch{}
    return {spec,jitless,oneCpu:pinned?affinity:null,managedOldMiB:old,status:r.status,signal:r.signal,
      error:r.error?.message,stderr:r.stderr.slice(-3000),data};
  }
  for (const id of [13587,78179]) {
    report.benchmarks.push(child({id}));
    process.stderr.write(`V8 benchmark ${id} finished\n`);
    fs.writeFileSync(path.join(directory,'report.json'),JSON.stringify(report,null,2));
  }
  report.benchmarks.push(child({id:13587,whole:32,windows:128,pressure:16},{oneCpu:true}));
  report.benchmarks.push(child({id:13587,whole:32,windows:128,pressure:16,playbackFirst:true},{oneCpu:true}));
  report.benchmarks.push(child({id:13587,whole:8,windows:16,pressure:4,timeout:1000},{jitless:true}));
  for (const kind of ['loop','microtask','chain','atomics','oom','external']) {
    report.safety.push(child({safety:kind},{old:kind==='oom'||kind==='external'?32:128}));
    process.stderr.write(`V8 safety probe ${kind} finished\n`);
  }
  report.complete=true;fs.writeFileSync(path.join(directory,'report.json'),JSON.stringify(report,null,2));
  process.stdout.write(directory+'/report.json\n');
}
