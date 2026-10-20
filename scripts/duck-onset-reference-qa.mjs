/* Exact browser Number conversion at binary64 boundaries, without audio I/O. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {BrowserClient} from './browser-client.mjs';

function fixtures() {
  const cases=[];
  const encode=value=>{
    if(Number.isNaN(value)) return 'NaN';
    const view=new DataView(new ArrayBuffer(8)); view.setFloat64(0,value);
    return view.getBigUint64(0).toString(16).padStart(16,'0');
  };
  const add=(base,onset)=>{
    for(const attack of [.002,.1,1,1e21]) cases.push({base,onset,attack,
      expected:[encode(Number(String(base)+onset)),encode(Number(String(base)+onset+String(attack)))]});
  };
  const onsets=[];
  for(const length of [127,128,800,801,10000])
    onsets.push('0'.repeat(length),'e'+'0'.repeat(length),
      '0'.repeat(length)+'1e-'+length,'1'+' '.repeat(length));
  onsets.push('0'.repeat(100000),'\u00a0'.repeat(1000),'x','b','o',
    'x'+'0'.repeat(1000)+'10','b'+'0'.repeat(1000)+'1','o'+'0'.repeat(1000)+'7',
    'x'+'f'.repeat(256),'x'+'f'.repeat(257),'b'+'1'.repeat(1024),'b'+'1'.repeat(1025),
    'o1'+'0'.repeat(341),'o7'+'0'.repeat(341));
  /* Every Unicode WhiteSpace/LineTerminator plus confusable non-space code
     points, in every position a suffix can occupy after base digits. */
  const wsPoints=[0x09,0x0a,0x0b,0x0c,0x0d,0x20,0xa0,0x1680,0x2000,0x2001,
    0x2002,0x2003,0x2004,0x2005,0x2006,0x2007,0x2008,0x2009,0x200a,0x2028,
    0x2029,0x202f,0x205f,0x3000,0xfeff,
    0x180e,0x200b,0x200c,0x200d,0x0085,0x00ad,0x061c].map(c=>String.fromCharCode(c));
  for(const ws of wsPoints)
    onsets.push('',' ','12'+ws,'12'+ws+ws,'0x10'+ws,'1_0'+ws,'e5'+ws,ws,
      '1'+ws+'e5','1e'+ws+'5','-'+ws+'5',ws+'-'+ws+'5');
  let seed=0xc0ffee;
  for(let test=0;test<12;test++) {
    let digits='';
    for(let i=0;i<1200;i++) {seed=(Math.imul(seed,1664525)+1013904223)>>>0;digits+=seed%10;}
    onsets.push(digits+'e-1200','e-'+'0'.repeat(1000)+test);
  }
  for(const base of [0,.3,1,1e-7,1e21]) for(const onset of onsets) add(base,onset);
  const fraction=(numerator,places)=>'.'+numerator.toString().padStart(places,'0');
  const half=5n**1075n;
  for(const numerator of [half-1n,half,half+1n,(2n**53n-1n)*half]) {
    const text=fraction(numerator,1075);
    add(0,text); add(0,text+'0'.repeat(1000)+'1');
  }
  for(const multiplier of [1n,3n]) {
    const midpoint=multiplier*5n**53n;
    for(const numerator of [midpoint-1n,midpoint,midpoint+1n]) {
      const text=fraction(numerator,53);
      add(1,text); add(1,text+'0'.repeat(1000)+'1');
    }
  }
  const overflow=(2n**54n-1n)*2n**970n;
  for(const integer of [overflow-1n,overflow,overflow+1n]) {
    add(0,integer.toString()); add(0,integer.toString()+'.'+'0'.repeat(1000)+'1');
  }
  return cases;
}
const browser=await BrowserClient.create();
let cases;
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof Number==="function"');
  cases=await browser.evaluate('('+fixtures.toString()+')()');
} finally {await browser.close();}
const artifacts=process.argv[2]??'/tmp/tompouce-duck-onset-reference';
await fs.mkdir(artifacts,{recursive:true});
await fs.writeFile(path.join(artifacts,'web.json'),JSON.stringify(cases)+'\n');
const native=spawnSync(process.env.TOMPOUCE_DUCK_ONSET_BINARY??'./build/tests/test_duck_onset',
  ['--reference'],{input:JSON.stringify(cases),encoding:'utf8',maxBuffer:8*1024*1024});
assert.equal(native.status,0,native.stderr);
const actual=JSON.parse(native.stdout);
await fs.writeFile(path.join(artifacts,'native.json'),JSON.stringify(actual)+'\n');
assert.equal(actual.length,cases.length);
cases.forEach((test,index)=>assert.deepEqual(actual[index],test.expected,
  `case ${index}, base ${test.base}, attack ${test.attack}, onset ${test.onset.length} bytes`));
console.log(`${cases.length} exact browser/native numeric concatenations passed`);
