/* Owned engine + private Pulse null sink. Verify the route before applying
   nonzero gain; record only that sink's monitor, never desktop output. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {spawn,spawnSync} from 'node:child_process';

const root=await fs.mkdtemp(path.join(os.tmpdir(),'tompouce-csound-qa-'));
const artifacts=path.resolve(process.argv[2]??'/tmp/tompouce-csound-native-qa');
const sinkName='tompouce_csound_qa_'+process.pid;
const loaded=spawnSync('pactl',['load-module','module-null-sink','sink_name='+sinkName,'channels=2'],{encoding:'utf8',timeout:5000});
assert.equal(loaded.status,0,loaded.stderr); assert.match(loaded.stdout.trim(),/^\d+$/);
let engine,recorder,port,diagnostics='',samples=[],sampleCount=0;
const levels=[];
function pactl(kind) {
  const result=spawnSync('pactl',['--format=json','list',kind],{encoding:'utf8',timeout:5000});
  assert.equal(result.status,0,result.stderr); return JSON.parse(result.stdout);
}
function tool(port,name,args={}) {
  return new Promise((resolve,reject)=>{
    const socket=net.connect(port,'127.0.0.1'); let buffer='';
    socket.setTimeout(10000,()=>socket.destroy(new Error('MCP timed out')));
    socket.on('error',reject);
    socket.on('connect',()=>socket.write(JSON.stringify({jsonrpc:'2.0',id:1,method:'tools/call',params:{name,arguments:args}})+'\n'));
    socket.on('data',bytes=>{
      buffer+=bytes;
      const end=buffer.indexOf('\n'); if (end<0) return;
      socket.end(); const reply=JSON.parse(buffer.slice(0,end));
      if (reply.error || reply.result?.isError) reject(Error(reply.result?.content?.[0]?.text??JSON.stringify(reply.error)));
      else resolve(reply.result.content.map(item=>item.text??'').join('\n'));
    });
  });
}
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
// Strudel treats double-quoted strings as mini notation. Orchestra arguments
// must be ordinary JavaScript strings, like the single-quoted unit fixtures.
const literal=text=>"'"+text.replaceAll('\\','\\\\').replaceAll("'","\\'").replaceAll('\n','\\n')+"'";
async function level(name,left,right) {
  const prior=sampleCount, deadline=Date.now()+5000;
  while (Date.now()<deadline) {
    await pause(25);
    if (sampleCount<prior+2048 || samples.length<2048) continue;
    let l=0,r=0;
    for (let i=samples.length-2048;i<samples.length;i+=2) {l+=samples[i];r+=samples[i+1];}
    l/=1024; r/=1024;
    if (Math.abs(l-left)<1e-4 && Math.abs(r-right)<1e-4) {
      levels.push({name,left:l,right:r,frames:sampleCount/2}); return;
    }
  }
  throw Error(name+': expected '+left+'/'+right+'; '+JSON.stringify(samples.slice(-8))+'\n'+diagnostics);
}
try {
  const listener=net.createServer(); await new Promise(resolve=>listener.listen(0,'127.0.0.1',resolve));
  port=listener.address().port; await new Promise(resolve=>listener.close(resolve));
  engine=spawn(process.env.TOMPOUCE_TEST_BINARY??'./build/tompouce',['--mcp-port',String(port),'--code','note(60).gain(0)'],{
    stdio:['ignore','ignore','pipe'],env:{...process.env,XDG_CONFIG_HOME:root,XDG_CACHE_HOME:root,XDG_STATE_HOME:root,TOMPOUCE_BANKS:root,
      PIPEWIRE_REMOTE:'tompouce-csound-qa-missing',PULSE_SINK:sinkName}});
  engine.stderr.on('data',bytes=>{diagnostics=(diagnostics+bytes).slice(-12000);});
  let ready=false;
  for (let i=0;i<100;i++) {
    try {await tool(port,'music_health');ready=true;break;} catch {await pause(50);}
  }
  assert(ready,diagnostics);
  const sink=pactl('sinks').find(item=>item.name===sinkName); assert(sink);
  const routes=pactl('sink-inputs').filter(item=>String(item.properties?.['application.process.id'])===String(engine.pid));
  assert.equal(routes.length,1); assert.equal(routes[0].sink,sink.index,'refuse audible QA route');
  assert((await tool(port,'music_health')).includes('backend=PulseAudio'));
  recorder=spawn('parec',['--raw','--rate=44100','--channels=2','--format=float32le','--device='+sinkName+'.monitor','--latency-msec=10'],
    {stdio:['ignore','pipe','pipe']});
  let partial=Buffer.alloc(0);
  recorder.stdout.on('data',bytes=>{
    const data=Buffer.concat([partial,bytes]), length=data.length-data.length%8;
    for (let i=0;i<length;i+=4) {const value=data.readFloatLE(i);assert(Number.isFinite(value));samples.push(value);}
    sampleCount+=length/4; samples=samples.slice(-8192); partial=data.subarray(length);
  });
  recorder.on('error',error=>{diagnostics+=String(error);});
  await tool(port,'music_stop');
  const held='instr HeldQA\na1 = p5\nout a1,a1\nendin\n';
  await tool(port,'music_set_code',{code:'await loadCsound('+literal(held)+"); note(69).csound('HeldQA').gain(.5).slow(4)",autostart:true});
  await level('held note',.1,.1);
  const added='instr AddedQA\na1 = p5\na0 init 0\nout a0,a1\nendin\n';
  await tool(port,'music_set_code',{code:'await loadCsound('+literal(added)+"); note(69).csound('AddedQA').gain(.5).slow(4).late(1)"});
  await level('changed orchestra retains held note',.1,.1);
  await level('new instrument available',.1,.2);
  await tool(port,'music_set_code',{code:'note(60).gain(0)'});
  await level('omitted orchestra retains both notes',.1,.2);
  await tool(port,'music_stop'); await level('Stop retains both notes',.1,.2);
  await tool(port,'music_set_code',{code:"await loadCsound('invalid orchestra'); note(60).gain(0)"});
  await level('syntax error retains both notes',.1,.2);
  await tool(port,'music_set_code',{code:"note(69).csound('HeldQA').gain(.5)",autostart:true});
  await level('previously loaded instrument survives native interlude',.2,.3);
  await fs.mkdir(artifacts,{recursive:true});
  await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify({passed:true,binary:process.env.TOMPOUCE_TEST_BINARY??'./build/tompouce',
    sink:sinkName,levels},null,2)+'\n');
  console.log('Native Csound real-stream lifecycle QA passed: '+artifacts);
} catch(error) {
  if (engine && engine.exitCode===null) console.error(await tool(port,'music_get_console').catch(()=>diagnostics));
  const snapshot=engine && engine.exitCode===null ? await tool(port,'music_debug').catch(String) : null;
  const health=engine && engine.exitCode===null ? await tool(port,'music_health').catch(String) : null;
  await fs.mkdir(artifacts,{recursive:true});
  await fs.writeFile(path.join(artifacts,'last-failure.json'),JSON.stringify({passed:false,error:String(error),diagnostics,levels,snapshot,health,
    binary:process.env.TOMPOUCE_TEST_BINARY??'./build/tompouce'},null,2)+'\n');
  throw error;
} finally {
  for (const child of [recorder,engine]) if (child && child.exitCode===null) {
    child.kill(); await new Promise(resolve=>child.exitCode!==null?resolve():child.once('exit',resolve));
  }
  spawnSync('pactl',['unload-module',loaded.stdout.trim()],{encoding:'utf8',timeout:5000});
  await fs.rm(root,{recursive:true,force:true,maxRetries:10,retryDelay:100});
}
