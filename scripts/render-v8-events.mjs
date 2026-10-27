#!/usr/bin/env node
/* Event-only adapter for the maintained comparison harness. Diagnostic V8
   execution of the fixed public corpus; never use this as an untrusted host. */
import fs from 'node:fs';
import {createProbe} from './v8-probe-runtime.mjs';
import {createHash} from 'node:crypto';
try {
  if (process.argv.length!==4) throw Error('usage: render-v8-events.mjs request.json output.json');
  const fd=fs.openSync(process.argv[2],fs.constants.O_RDONLY|fs.constants.O_NONBLOCK),bytes=Buffer.alloc(2*1024*1024+1);let length=0;
  try {
    if (!fs.fstatSync(fd).isFile()) throw Error('regular request file required');
    while (length<bytes.length) {
      const count=fs.readSync(fd,bytes,length,bytes.length-length,null);
      if (!count) break;length+=count;
    }
  } finally {fs.closeSync(fd);}
  if (length>2*1024*1024) throw Error('request limit');
  const request=JSON.parse(bytes.subarray(0,length));
  if (request.eventsOnly!==true||!Number.isInteger(request.cycles)||request.cycles<1||request.cycles>32||
      !Number.isFinite(request.cps)||request.cps<.1||request.cps>2) throw Error('event-only request required');
  const corpus=JSON.parse(fs.readFileSync(new URL('../tests/fixtures/strudel-bakery.json',import.meta.url)));
  const hash=typeof request.code==='string'?createHash('sha256').update(request.code).digest('hex'):null;
  if (!hash||!corpus.examples.some(e=>e.code===request.code&&e.sha256===hash)) throw Error('diagnostic accepts only the pinned Bakery corpus');
  const probe=createProbe(),{metadata}=probe.compile(request.code,request.cps*240);
  const cps=Number.isFinite(metadata.bpm)&&metadata.bpm>0?metadata.bpm/240:request.cps;
  const events=[];
  for (let begin=0;begin<request.cycles;begin++) events.push(...probe.query(begin,begin+1,cps).events);
  fs.writeFileSync(process.argv[3],JSON.stringify({cps,events}));
} catch(error) {process.stderr.write(String(error)+'\n');process.exitCode=1;}
