/* Capture actual deployed scope drawing, without audio devices or playback. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {BrowserClient} from './browser-client.mjs';

async function capture() {
  const url=performance.getEntriesByType('resource').map(x=>x.name)
    .find(x=>/\/spectrum\.[^/]+\.js$/.test(x));
  if (!url) throw Error('deployed scope module not found');
  const module=await import(url);
  // Resolve the real drawing export by its AudioParam-independent signature.
  const draw=Object.values(module).find(value=>typeof value==='function' &&
    String(value).includes('frequencyBinCount') && String(value).includes('trigger:'));
  if (!draw || !module.K || !module.L) throw Error('scope module interface changed');
  const pattern=note(60),prototype=Object.getPrototypeOf(pattern),original=prototype.scope;
  let inline;
  prototype.scope=function(options) {inline={scale:options.scale,pos:options.pos};return this;};
  try {pattern._scope(987654,{});}
  finally {prototype.scope=original;}
  const samples=[.5,.25,0,-.25,-.5,.5,0,-.5];
  const cases=[{name:'defaults',samples,options:{}},
    {name:'unaligned',samples,options:{align:false}},
    {name:'trigger',samples,options:{trigger:.25}},
    {name:'zero-scale',samples,options:{align:false,scale:0}},
    {name:'negative-scale',samples,options:{align:false,scale:-.25,pos:.5}},
    {name:'late-crossing',samples:[-.5,-.25,0,.5,.25,0,-.25,-.5],options:{}},
    {name:'quiet',samples:samples.map(x=>x/10),options:{align:false}},
    {name:'inline-defaults',samples:samples.map(x=>x/2),options:inline}];
  const id=987654;
  try {
    for (const entry of cases) {
      const points=[],ctx={canvas:{width:80,height:80},beginPath(){},stroke(){},
        moveTo(x,y){points.push([x,y]);},lineTo(x,y){points.push([x,y]);}};
      const analyser={frequencyBinCount:entry.samples.length,
        getFloatTimeDomainData(destination){destination.set(entry.samples);}};
      module.K[id]=analyser; module.L[id]=new Float32Array(entry.samples.length);
      draw(analyser,{...entry.options,ctx,id});
      entry.points=points; entry.thickness=ctx.lineWidth;
    }
  } finally {delete module.K[id];delete module.L[id];}
  return {url,inline,cases};
}

const browser=await BrowserClient.create();
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function"',20000);
  const result=await browser.evaluate('('+capture.toString()+')()');
  assert.deepEqual(result.inline,{scale:1,pos:.5},'web inline wrapper defaults changed');
  const expected=[[[0,60],[10,65],[20,70],[30,50],[40,60],[50,70]],
    [[0,50],[10,55],[20,60],[30,65],[40,70],[50,50],[60,60],[70,70]],
    [[0,65],[10,70],[20,50],[30,60],[40,70]],
    Array.from({length:8},(_,i)=>[i*10,60]),
    [[0,50],[10,45],[20,40],[30,35],[40,30],[50,50],[60,40],[70,30]],
    [[0,60],[10,65],[20,70]],
    [[0,59],[10,59.5],[20,60],[30,60.5],[40,61],[50,59],[60,60],[70,61]],
    [[0,40],[10,50],[20,60],[30,20],[40,40],[50,60]]];
  result.cases.forEach((entry,index)=>{
    assert.equal(entry.thickness,3);
    assert.equal(entry.points.length,expected[index].length);
    entry.points.forEach((point,i)=>point.forEach((v,j)=>
      assert(Math.abs(v-expected[index][i][j])<1e-6,entry.name+' coordinate changed')));
  });
  const artifacts=process.argv[2]??'/tmp/tompouce-scope-reference';
  await fs.mkdir(artifacts,{recursive:true});
  await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify({
    capturedAt:new Date().toISOString(),...result},null,2)+'\n');
  console.log('Web scope: defaults, fixed amplitude, falling/late trigger, sample spacing and zero/negative scale verified');
} finally {await browser.close();}
