/* Serial, isolated preparation/scheduler load measurements. No audio device. */
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs/promises';
import path from 'node:path';

const binary=process.env.TOMPOUCE_PREP_DEADLINE_BINARY??'./build/tests/test_prep_deadlines';
const directory=path.resolve(process.argv[2]??'/tmp/tompouce-prep-deadlines');
const budgetResult=spawnSync(binary,['--cpu-budget'],{encoding:'utf8',timeout:10000});
assert.equal(budgetResult.status,0,budgetResult.stderr||String(budgetResult.error));
const budget=Number(budgetResult.stdout.trim());
assert(Number.isInteger(budget)&&budget>0);
const max=Math.min(4,budget);
const workers=[...new Set([1,2,4,max].filter(n=>n<=max))].sort((a,b)=>a-b);
const results=[];
for(const count of workers) for(const block of [128,4096]) for(let workload=0;workload<4;workload++) {
  const run=spawnSync(binary,[String(count),String(workload),String(block)],{
    encoding:'utf8',timeout:60000,maxBuffer:1024*1024,
  });
  assert.equal(run.status,0,run.stderr||String(run.error));
  const line=run.stdout.trim();
  assert(line.startsWith('deadline '),line);
  const result=Object.fromEntries([...line.matchAll(/(\w+)=([^\s]+)/g)].map(([,key,value])=>
    [key,Number.isFinite(Number(value))?Number(value):value]));
  result.deadlinesHold=result.skipped_late===0&&result.published_late===0&&
    result.reserved_late===0&&result.callback_overruns===0&&result.wake_misses===0&&
    result.submitted===result.jobs;
  /* Overload sheds as skips under the specified policy (cold preparation
     that misses its onset is skipped, never burst late). Shed-clean means
     no late publication/reservation, callback overrun or host wake miss
     anywhere. 4,096 frames is the output adapter's render-call cap, so both
     block sizes are realizable configurations. */
  result.shedClean=result.published_late===0&&result.reserved_late===0&&
    result.callback_overruns===0&&result.wake_misses===0;
  results.push(result); console.log(line);
}
await fs.mkdir(directory,{recursive:true});
const coldLive=results.filter(result=>!['burst','voiceburst'].includes(result.workload));
const coldLiveHold=coldLive.every(result=>result.deadlinesHold);
const shedClean=results.every(result=>result.shedClean);
/* The voice-burst proof is vacuous unless the pool actually saturated:
   every voiceburst case must show retiring graphs (evictions happened). */
const voiceCases=results.filter(result=>result.workload==='voiceburst');
const voiceProof=voiceCases.length>0&&voiceCases.every(result=>result.peak_retiring>0);
const report={capturedAt:new Date().toISOString(),binary:path.resolve(binary),budget,
  backendUnderruns:'unmeasured: paced offline callback, no device; see profile-prep-backend',
  coldFetch:'local owned WAV reads/decodes, not remote network latency',
  overloadPolicy:'burst skips are specified shed (skipped_late), not late bursts; '+
    '4,096 frames is the output adapter render-call cap, so both block sizes are realizable',
  timingGatePassed:coldLiveHold&&shedClean&&voiceProof,
  coldLiveHold,shedClean,voiceProof,results};
await fs.writeFile(path.join(directory,'report.json'),JSON.stringify(report,null,2)+'\n');
console.log(`Deadline observations: ${results.filter(r=>r.deadlinesHold).length}/${results.length} without violations; `+
  `cold/live hold ${coldLive.filter(r=>r.deadlinesHold).length}/${coldLive.length}, `+
  `shed-clean ${results.filter(r=>r.shedClean).length}/${results.length}, `+
  `voiceburst saturated ${voiceCases.filter(r=>r.peak_retiring>0).length}/${voiceCases.length}; ${path.join(directory,'report.json')}`);
if(!report.timingGatePassed) process.exitCode=1;
