#!/usr/bin/env node
/* Opt-in pattern-query worker running the same committed bundle under Node's
   V8. QuickJS remains the default backend: this process exists so a host can
   compare or (when explicitly enabled) delegate queries without linking V8.

   Transport: newline-delimited JSON on stdin/stdout, one request per line.
   The parent sends an init frame with the bundle, then compile/query frames.
   Every request is bounded; a request that exceeds its budget is not answered
   and the parent is expected to kill the worker.

   node:vm is not a security boundary. Confinement, watchdogs and memory
   policy belong to the parent (see docs/v8-assessment.md). */
import fs from 'node:fs';
import vm from 'node:vm';

const MAX_FRAME = 8 * 1024 * 1024;
const MAX_SOURCE = 4 * 1024 * 1024;

let context = null;
/* The engine creates a fresh QuickJS context per evaluation. Keep the bundle
   so each compile can rebuild the VM context too: reusing one context made
   prebake globals accumulate across evaluations (e.g. preCount 1 -> 2). */
let currentBundle = null;
/* Query-time console output is forwarded to the parent, bounded per request. */
let requestLogs = [];
const note = (value) => {
  if (requestLogs.length >= 64) return;
  const text = String(value);
  requestLogs.push(text.length > 512 ? text.slice(0, 512) : text);
};
let compileScript = null;
let queryScript = null;
let probeScript = null;

const write = (value) => {
  const text = JSON.stringify(value);
  if (Buffer.byteLength(text) > MAX_FRAME) throw new Error('response exceeds the frame limit');
  process.stdout.write(text + '\n');
};

function boot(bundle, timeoutMs) {
  if (typeof bundle !== 'string' || bundle.length === 0 || Buffer.byteLength(bundle) > MAX_FRAME) {
    throw new Error('invalid bundle');
  }
  currentBundle = bundle;
  context = vm.createContext(vm.constants.DONT_CONTEXTIFY, {
    microtaskMode: 'afterEvaluate',
    codeGeneration: {strings: true, wasm: false},
  });
  context.__workerLog = note;
  const prelude = `globalThis.performance={now:()=>Date.now()};
    globalThis.console={log(){},warn(){},error(){},info(){},debug(){}};
    globalThis.__hostLog=(value)=>__workerLog(String(value));
    globalThis.__workerSliders=[];
    globalThis.__workerBinds=[];
    globalThis.__hostSlider=v=>v;
    /* The host sends its own bind indices, so source-position binds that reuse
       a slot keep the same identity here as in the QuickJS evaluation. */
    globalThis.__hostSliderBind=()=>__workerBinds.length?__workerBinds.shift():-1;
    globalThis.__hostSliderRead=i=>__workerSliders[i];
    globalThis.__midiCC=new Map();
    globalThis.__hostMidiOpen=()=>1;
    globalThis.__hostMidiRead=(cc,chan)=>__midiCC.get(String(cc)+':'+String(chan ?? 0)) ?? 0;
    globalThis.__hostMidiKeys=()=>{throw new Error('midikeys are unavailable in the query worker')};
    /* Orchestra loads are applied by the engine's confined Csound renderer
       after evaluation. Here they only need to be bounded and accepted so the
       same source compiles; query events do not depend on the engine state. */
    globalThis.__csoundLoads=0;
    globalThis.__hostCsound=(kind,text)=>{
      if(++__csoundLoads>16)throw new Error('too many Csound loads');
      if(typeof kind!=='string'||typeof text!=='string'||
         (kind==='code'&&text.length>65536)||(kind==='url'&&text.length>480))
        throw new Error('invalid Csound load');
    };
    globalThis.__prebakeLoadingAllowed=true;
    globalThis.__hostPrebake=name=>{
      if(!__prebakeLoadingAllowed)throw new Error('prebake loads are closed after evaluation');
      if(!Object.hasOwn(__prebakes,name))throw new Error('prebake unavailable in the query worker');
      return __prebakes[name];
    };`;
  new vm.Script(prelude).runInContext(context, {timeout: timeoutMs});
  new vm.Script(bundle, {filename: 'tompouce-runtime.js'}).runInContext(context, {timeout: timeoutMs});
  compileScript = new vm.Script(`globalThis.__compileResult=undefined;globalThis.__compileError=undefined;
    __compile(__source,__bpm,__prebake).then(x=>{__compileResult=x},e=>{__compileError=String(e)});`);
  queryScript = new vm.Script('__query(__begin,__end,__cps,false)');
  probeScript = new vm.Script('__query(__begin,__end,__cps,true)');
}

function compile(source, bpm, timeoutMs, prebake, prebakes, sliders) {
  if (typeof source !== 'string' || Buffer.byteLength(source) > MAX_SOURCE) throw new Error('source limit');
  if (typeof prebake !== 'string' || Buffer.byteLength(prebake) > MAX_FRAME) throw new Error('prebake limit');
  if (!prebakes || typeof prebakes !== 'object' || Array.isArray(prebakes)) throw new Error('invalid prebake table');
  if (!sliders || typeof sliders !== 'object' || !Array.isArray(sliders.binds) ||
      !Array.isArray(sliders.values) || sliders.binds.length > 64) throw new Error('invalid slider table');
  /* Match the engine's per-evaluation context so prebake and pattern globals
     cannot leak from an earlier evaluation into this one. */
  if (currentBundle) boot(currentBundle, timeoutMs);
  context.__source = source;
  context.__bpm = Number.isFinite(bpm) && bpm > 0 ? bpm : 120;
  /* The engine sends its confined startup prebake so the worker sees the same
     defaults as the QuickJS evaluation. Named loadPrebake() calls still need a
     host callback and are excluded from delegation. */
  context.__prebake = prebake;
  context.__prebakes = prebakes;
  context.__workerBinds = sliders.binds.slice();
  context.__workerSliders = sliders.values.slice();
  context.__midiCC = new Map();
  compileScript.runInContext(context, {timeout: timeoutMs});
  context.__prebakeLoadingAllowed = false;
  if (context.__compileError) throw new Error(String(context.__compileError).slice(0, 512));
  if (typeof context.__compileResult !== 'string') throw new Error('compile did not settle');
  return context.__compileResult;
}

function query(begin, end, cps, probes, timeoutMs, sliders, midi) {
  if (midi !== undefined) {
    if (!Array.isArray(midi) || midi.length > 2048) throw new Error('invalid midi snapshot');
    context.__midiCC = new Map(midi.map(entry =>
      [`${Number(entry[0])}:${Number(entry[1])}`, Number(entry[2])]));
  }
  if (sliders !== undefined) {
    if (!Array.isArray(sliders) || sliders.length > 64) throw new Error('invalid slider values');
    context.__workerSliders = sliders.slice();
  }
  context.__begin = begin;
  context.__end = end;
  /* Omitted cps lets the bundle fall back to the program tempo, as the QuickJS
     host does when it passes NaN. */
  context.__cps = Number.isFinite(cps) && cps > 0 ? cps : undefined;
  const text = (probes ? probeScript : queryScript).runInContext(context, {timeout: timeoutMs});
  if (typeof text !== 'string' || Buffer.byteLength(text) > MAX_FRAME) throw new Error('event output limit');
  return text;
}

function handle(frame, budget) {
  switch (frame.op) {
  case 'init':
    boot(frame.bundle, budget);
    return {ok: true, v8: process.versions.v8};
  case 'compile':
    if (!context) throw new Error('not initialized');
    return {ok: true, metadata: compile(frame.source, frame.bpm, budget,
      frame.prebake ?? '', frame.prebakes ?? {}, frame.sliders ?? {binds: [], values: []})};
  case 'query':
    if (!context) throw new Error('not initialized');
    return {ok: true, events: query(frame.begin, frame.end, frame.cps, frame.probes === true,
      budget, frame.sliders, frame.midi)};
  case 'close':
    process.stdout.write(JSON.stringify({id: frame.id ?? null, ok: true}) + '\n', () => process.exit(0));
    return null;
  default:
    throw new Error('unknown op');
  }
}

let pending = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => {
  pending += chunk;
  if (Buffer.byteLength(pending) > MAX_FRAME) {
    write({id: null, ok: false, error: 'frame limit'});
    process.exit(2);
  }
  let newline;
  while ((newline = pending.indexOf('\n')) >= 0) {
    const line = pending.slice(0, newline);
    pending = pending.slice(newline + 1);
    if (!line.trim()) continue;
    let frame;
    try {
      frame = JSON.parse(line);
    } catch (error) {
      write({id: null, ok: false, error: 'malformed frame'});
      continue;
    }
    const id = Number.isInteger(frame.id) ? frame.id : null;
    const budget = frame.op === 'query' ? 2000 : 4000;
    try {
      requestLogs = [];
      const result = handle(frame, budget);
      if (result) write({id, ...result, logs: requestLogs});
    } catch (error) {
      write({id, ok: false, error: String(error?.message ?? error).slice(0, 512)});
    }
  }
});
process.stdin.on('end', () => process.exit(0));
