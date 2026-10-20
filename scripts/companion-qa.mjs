/* Real browser/bridge/native engine. Only owned processes and a private null
   sink; route is verified before any nonzero-gain source is applied. */
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import assert from 'node:assert/strict';
import {spawn,spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

const artifacts=path.resolve(process.argv[2]??'/tmp/tompouce-companion-qa');
await fs.mkdir(artifacts,{recursive:true});
const root=await fs.mkdtemp(path.join(os.tmpdir(),'tompouce-companion-qa-'));
const sinkName=`tompouce_qa_${process.pid}`;
const loaded=spawnSync('pactl',['load-module','module-null-sink',`sink_name=${sinkName}`,'channels=2'],{encoding:'utf8',timeout:5000});
if (loaded.status!==0 || !/^\d+$/.test(loaded.stdout.trim())) throw new Error('QA requires PulseAudio-compatible private null sink support');
const moduleId=loaded.stdout.trim();
let engine,bridge,browser;
function pactl(kind) {
  const result=spawnSync('pactl',['--format=json','list',kind],{encoding:'utf8',timeout:5000});
  if (result.status!==0) throw new Error(result.stderr); return JSON.parse(result.stdout);
}
const mcpToken='b'.repeat(64); // disposable owned engine only
function tool(port,name,args={}) {
  return new Promise((resolve,reject)=>{
    const socket=net.connect(port,'127.0.0.1'); let buffer='';
    socket.setTimeout(10000,()=>socket.destroy(new Error('MCP timed out')));
    socket.on('error',reject);
    socket.on('connect',()=>socket.write(JSON.stringify({jsonrpc:'2.0',id:1,auth:mcpToken,method:'tools/call',params:{name,arguments:args}})+'\n'));
    socket.on('data',bytes=>{
      buffer+=bytes; const end=buffer.indexOf('\n'); if (end<0) return;
      socket.end(); const response=JSON.parse(buffer.slice(0,end));
      if (response.error || response.result?.isError) reject(new Error(JSON.stringify(response)));
      else resolve(response.result.content[0].text);
    });
  });
}
try {
  const server=net.createServer(); await new Promise(r=>server.listen(0,'127.0.0.1',r));
  const port=server.address().port; await new Promise(r=>server.close(r));
  engine=spawn(process.env.TOMPOUCE_TEST_BINARY??'./build/tompouce',['--mcp-port',String(port),'--code','note(60).gain(0)'],{
    env:{...process.env,TOMPOUCE_MCP_TOKEN:mcpToken,XDG_CONFIG_HOME:root,XDG_CACHE_HOME:root,TOMPOUCE_BANKS:root,
      PIPEWIRE_REMOTE:'tompouce-qa-missing-native',PULSE_SINK:sinkName},stdio:['ignore','ignore','pipe'],
  });
  let diagnostics=''; engine.stderr.on('data',bytes=>{diagnostics+=bytes;});
  let ready=false;
  for (let i=0;i<100;i++) {
    try { await tool(port,'music_health'); ready=true; break; } catch {await new Promise(r=>setTimeout(r,50));}
  }
  assert(ready,diagnostics);
  const sink=pactl('sinks').find(s=>s.name===sinkName); assert(sink);
  const inputs=pactl('sink-inputs').filter(s=>String(s.properties?.['application.process.id'])===String(engine.pid));
  assert.equal(inputs.length,1,'owned stream missing'); assert.equal(inputs[0].sink,sink.index,'refuse audible QA route');
  assert((await tool(port,'music_health')).includes('backend=PulseAudio'));
  bridge=spawn('python3',['scripts/companion.py','--mcp-port',String(port)],{stdio:['ignore','pipe','pipe'],env:{...process.env,TOMPOUCE_MCP_TOKEN:mcpToken,XDG_CONFIG_HOME:root}});
  const url=await new Promise((resolve,reject)=>{
    const timeout=setTimeout(()=>reject(new Error('Companion did not start')),10000); let output='';
    bridge.on('error',reject); bridge.stdout.on('data',bytes=>{
      output+=bytes; const match=output.match(/http:\/\/127\.0\.0\.1:\d+\//); if (match) {clearTimeout(timeout);resolve(match[0]);}
    });
  });
  browser=await BrowserClient.create(); await browser.page(url);
  await browser.until(`document.getElementById('connection').textContent.includes('Connected') && document.querySelector('.cm-content')`);
  await browser.until(`document.querySelector('.cm-content').textContent.includes('gain(0)')`);
  await browser.evaluate(`document.getElementById('bpm').value='136';document.getElementById('tempo').click()`);
  await browser.until(`document.getElementById('health').textContent.includes('bpm=136')`);
  await browser.evaluate(`document.getElementById('stop').click()`);
  await browser.until(`document.getElementById('health').textContent.includes('playing=no')`);
  const code=`// Native Kabelsalat · owned silent QA\nnote("c4 e4 g4").K(sine(sFreq).mul(.1).out())\n  .gain(.2).room(.1)`;
  await browser.evaluate(`document.querySelector('.cm-content').focus()`);
  await browser.call('Input.dispatchKeyEvent',{type:'keyDown',key:'a',code:'KeyA',modifiers:2,windowsVirtualKeyCode:65});
  await browser.call('Input.dispatchKeyEvent',{type:'keyUp',key:'a',code:'KeyA',modifiers:2,windowsVirtualKeyCode:65});
  await browser.call('Input.insertText',{text:code});
  assert.equal((await tool(port,'music_get_code')).trim(),'note(60).gain(0)');
  await browser.evaluate(`document.getElementById('apply').click()`);
  await browser.until(`document.getElementById('dirty').textContent.includes('Following')`);
  await browser.until(`document.getElementById('health').textContent.includes('playing=yes')`);
  assert.equal((await tool(port,'music_get_code')).trim(),code);
  await browser.until(`document.getElementById('health').textContent.includes('voices=') && !document.getElementById('health').textContent.includes('voices=0/')`);
  await browser.evaluate(`Array.from(document.querySelectorAll('#sound-tabs button')).find(b=>b.textContent.toLowerCase().includes('synths')).click();document.getElementById('sound-query').value='bytebeat';document.getElementById('sound-query').dispatchEvent(new Event('input',{bubbles:true}))`);
  await browser.until(`document.getElementById('sound-results').children.length===1 && document.getElementById('sound-results').textContent.includes('bytebeat')`);
  await browser.screenshot(path.join(artifacts,'desktop.png'),true);
  await browser.evaluate(`document.getElementById('stop').click()`);
  await browser.until(`document.getElementById('health').textContent.includes('playing=no')`);
  const restartCode='note("c5 e5").s("sine").gain(.1)';
  await browser.evaluate(`document.querySelector('.cm-content').focus()`);
  await browser.call('Input.dispatchKeyEvent',{type:'keyDown',key:'a',code:'KeyA',modifiers:2,windowsVirtualKeyCode:65});
  await browser.call('Input.dispatchKeyEvent',{type:'keyUp',key:'a',code:'KeyA',modifiers:2,windowsVirtualKeyCode:65});
  await browser.call('Input.insertText',{text:restartCode});
  await browser.evaluate(`document.getElementById('play').click()`);
  await browser.until(`document.getElementById('health').textContent.includes('playing=yes')`);
  assert.equal((await tool(port,'music_get_code')).trim(),restartCode);
  await browser.call('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  await browser.until(`document.documentElement.clientWidth===390`);
  await browser.screenshot(path.join(artifacts,'mobile.png'),true);
  const layout=await browser.evaluate(`({width:innerWidth,scroll:document.documentElement.scrollWidth,error:document.getElementById('error').textContent,source:document.querySelector('.cm-content').textContent})`);
  assert.equal(layout.error,''); assert(layout.scroll<=layout.width,'horizontal page overflow');
  assert.equal(browser.exceptions.length,0,JSON.stringify(browser.exceptions));
  await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify({passed:true,backend:'PulseAudio',silentSink:sinkName,
    checks:['real bridge/native engine','private output verified','CodeMirror input/apply','tempo','live voices','synth search','stop/play','desktop/mobile layout'],layout},null,2));
  console.log(`Companion browser QA passed: ${artifacts}`);
} finally {
  if (browser) await browser.close();
  for (const child of [bridge,engine]) if (child && child.exitCode===null) {
    child.kill(); await new Promise(r=>child.exitCode!==null?r():child.once('exit',r));
  }
  spawnSync('pactl',['unload-module',moduleId],{encoding:'utf8',timeout:5000});
  await fs.rm(root,{recursive:true,force:true,maxRetries:10,retryDelay:100});
}
