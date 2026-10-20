/* Real-backend preparation-load measurement on an owned silent sink.
   Never changes default routing: the stream is targeted at a temporary null
   sink, routing is verified with pw-dump before measurement, and the module
   is unloaded afterwards. No audio device is opened by this script itself. */
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import fs from 'node:fs/promises';
import path from 'node:path';

const binary=process.env.TOMPOUCE_PREP_DEADLINE_BINARY??'./build/tests/test_prep_deadlines';
const probe=process.env.TOMPOUCE_AUDIO_PROBE??'./build/tests/probe_audio_output';
const directory=path.resolve(process.argv[2]??'/tmp/tompouce-prep-backend');
const runtime=process.env.XDG_RUNTIME_DIR??`/run/user/${process.getuid?.()??1000}`;
const sink=`tompouce_prep_qa_${process.pid}`;

function haveServer() {
  try {
    return existsSync(path.join(runtime,'pipewire-0')) &&
      ['pactl','pw-dump'].every(tool=>{
        try { return spawnSync('which',[tool],{encoding:'utf8'}).status===0; }
        catch { return false; }
      });
  } catch { return false; }
}

async function main() {
  await fs.mkdir(directory,{recursive:true});
  if (!haveServer()) {
    const report={capturedAt:new Date().toISOString(),status:'skipped',
      reason:'no PipeWire socket or pactl/pw-dump unavailable'};
    await fs.writeFile(path.join(directory,'report.json'),JSON.stringify(report,null,2)+'\n');
    console.log('Backend measurement skipped: no PipeWire server');
    return;
  }
  const load=spawnSync('pactl',['load-module','module-null-sink',`sink_name=${sink}`,
    'sink_properties=device.description=TompoucePrepQA'],{encoding:'utf8',timeout:10000});
  assert.equal(load.status,0,load.stderr||String(load.error));
  const module=load.stdout.trim();
  assert(/^\d+$/.test(module),`unexpected module id: ${module}`);
  try {
    const routing=await verifyRouting(sink);
    const results=await measure(sink);
    const gatePassed=results.every(result=>result.backendHold);
    const report={capturedAt:new Date().toISOString(),binary:path.resolve(binary),
      probe:path.resolve(probe),sink,routing,status:gatePassed?'passed':'failed',
      backendUnderruns:results.every(r=>r.backend_drops===0)?'none observed':'OBSERVED',
      results};
    await fs.writeFile(path.join(directory,'report.json'),JSON.stringify(report,null,2)+'\n');
    console.log(`Backend preparation load: ${results.filter(r=>r.backendHold).length}/${results.length} hold; `+
      `${path.join(directory,'report.json')}`);
    if (!gatePassed) process.exitCode=1;
  } finally {
    spawnSync('pactl',['unload-module',module],{encoding:'utf8',timeout:10000});
  }
}

function verifyRouting(sink) {
  return new Promise((resolve,reject)=>{
    const child=spawn(probe,['PipeWire','--inspect'],{
      env:{...process.env,TOMPOUCE_OUTPUT_TARGET:sink},stdio:['pipe','pipe','pipe']});
    let output='',settled=false;
    const fail=(error)=>{ if (!settled) { settled=true; try { child.kill(); } catch {} reject(error); } };
    const timer=setTimeout(()=>fail(new Error(`probe startup timed out: ${output}`)),12000);
    child.stdout.on('data',(chunk)=>{
      output+=chunk;
      if (!settled && output.includes('ready backend=PipeWire')) {
        settled=true; clearTimeout(timer);
        try {
          const graph=JSON.parse(spawnSync('pw-dump',{encoding:'utf8',timeout:10000}).stdout);
          const pid=String(child.pid);
          const clients=new Set(graph.filter(item=>String(item.type).endsWith(':Client') &&
            String(item.info?.props?.['application.process.id'])===pid).map(item=>item.id));
          assert(clients.size>0,'owned stream client missing from graph');
          const nodes=graph.filter(item=>String(item.type).endsWith(':Node') &&
            clients.has(item.info?.props?.['client.id']));
          assert.equal(nodes.length,1,`expected one stream node, saw ${nodes.length}`);
          assert.equal(nodes[0].info?.props?.['media.class'],'Stream/Output/Audio');
          const byId=new Map(graph.filter(item=>String(item.type).endsWith(':Node')).map(item=>[item.id,item]));
          const links=graph.filter(item=>String(item.type).endsWith(':Link') &&
            item.info?.['output-node-id']===nodes[0].id);
          assert.equal(links.length,2,`stereo stream must have two links, saw ${links.length}`);
          const targets=new Set();
          for (const link of links) {
            assert.equal(link.info.state,'active','stream link is not active');
            targets.add(byId.get(link.info['input-node-id'])?.info?.props?.['node.name']);
          }
          assert.deepEqual([...targets],[sink],`stream routes outside the owned sink: ${[...targets]}`);
          child.stdin.write('\n');
        } catch (error) { try { child.kill(); } catch {} reject(error); }
      }
    });
    child.on('error',fail);
    child.on('exit',(code)=>{
      if (!settled) fail(new Error(`probe exited early with ${code}: ${output}`));
      else if (code!==0) reject(new Error(`probe failed with ${code}: ${output}`));
      else resolve({clientLinks:2,sink,state:'active'});
    });
  });
}

async function measure(sink) {
  const budget=Number(spawnSync(binary,['--cpu-budget'],{encoding:'utf8',timeout:10000}).stdout.trim());
  assert(Number.isInteger(budget)&&budget>0);
  const max=Math.min(4,budget);
  const workers=[...new Set([1,max])].sort((a,b)=>a-b);
  const cases=[];
  for (const count of workers) for (const workload of [0,1]) cases.push([count,workload]);
  cases.push([max,2]);
  cases.push([max,3]);
  const results=[];
  for (const [count,workload] of cases) {
    const run=spawnSync(binary,[String(count),String(workload),'128','backend'],{
      env:{...process.env,TOMPOUCE_OUTPUT_TARGET:sink},
      encoding:'utf8',timeout:90000,maxBuffer:1024*1024});
    assert.equal(run.status,0,run.stderr||run.stdout||String(run.error));
    const line=run.stdout.trim().split('\n').find(entry=>entry.startsWith('backend '));
    assert(line,run.stdout);
    const result=Object.fromEntries([...line.matchAll(/(\w+)=([^\s]+)/g)].map(([,key,value])=>
      [key,Number.isFinite(Number(value))?Number(value):value]));
    /* Cold/live must hold fully; burst overload must shed as skips without
       late publication/reservation, render overruns or stream drops. The
       voice burst must additionally prove saturation via retiring graphs. */
    result.backendHold=result.status==='measured'&&result.backend==='PipeWire'&&
      result.backend_drops===0&&result.rendered_s>=1.75&&result.render_overruns===0&&
      result.published_late===0&&result.reserved_late===0&&
      (workload>=2||result.skipped_late===0)&&
      (workload!==3||(result.peak_retiring>0&&result.controls>0));
    results.push(result); console.log(line);
  }
  return results;
}

await main();
