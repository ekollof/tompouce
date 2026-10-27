/* Compile-stage wall-time probe, not a query-budget or parity gate. Each run
   owns a fresh engine/config root through the existing reference worker. */
import fs from 'node:fs';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
const runs=Number(process.env.TOMPOUCE_PROFILE_RUNS??5);
if (!Number.isInteger(runs)||runs<1||runs>20) throw Error('runs must be 1..20');
const windows=Number(process.env.TOMPOUCE_PROFILE_WINDOWS??0);
if (!Number.isInteger(windows)||windows<0||windows>32) throw Error('windows must be 0..32');
const binaries=process.argv.slice(2);
if (!binaries.length) binaries.push('./build/tests/test_javascript');
const corpus=JSON.parse(fs.readFileSync('tests/fixtures/strudel-bakery.json','utf8'));
const results=[];
for (const id of [13587,78179]) {
  const example=corpus.examples.find(e=>Number(e.id)===id);
  if (createHash('sha256').update(example.code).digest('hex')!==example.sha256)
    throw Error(`source hash mismatch: ${id}`);
  const prefix=windows?
    `all(p=>{const samples=[],counts=[];for(let i=0;i<${windows};i++){const t=Date.now();const h=p.queryArc(i*.025,(i+1)*.025,{_cps:.5});samples.push(Date.now()-t);counts.push(h.length)}return note(60).fmap(v=>({...v,samples,counts}));});\n`:
    'all(p=>{const t=Date.now();const h=p.queryArc(0,1,{_cps:.5});return note(Date.now()-t).gain(h.length);});\n';
  const samples=binaries.map(binary=>({binary,ms:[],haps:[]}));
  for (let run=0;run<runs;run++) for (const sample of samples) {
    const child=spawnSync(sample.binary,['--prebake-reference','',prefix+example.code],
      {encoding:'utf8',timeout:15000,maxBuffer:1024*1024});
    if (child.status!==0) throw Error(`${id}: ${sample.binary}: ${child.error??child.stderr}`);
    const [event]=JSON.parse(child.stdout);
    if (windows) {sample.ms.push(...event.value.samples);sample.haps.push(...event.value.counts);}
    else {sample.ms.push(event.value.note);sample.haps.push(event.value.gain);}
  }
  results.push({id,sha256:example.sha256,samples:samples.map(sample=>{
    const ordered=[...sample.ms].sort((a,b)=>a-b);
    return {...sample,medianMs:ordered[Math.floor(ordered.length/2)]};
  })});
}
process.stdout.write(JSON.stringify({kind:'compile-stage-query-probe',clock:'Date.now wall milliseconds',queryCps:.5,runs,
  queryWindowSeconds:windows ? .05 : null,queryBudgetGate:false,results},null,2)+'\n');
