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
let compileScript = null;
let queryScript = null;

const write = (value) => {
  const text = JSON.stringify(value);
  if (Buffer.byteLength(text) > MAX_FRAME) throw new Error('response exceeds the frame limit');
  process.stdout.write(text + '\n');
};

function boot(bundle, timeoutMs) {
  if (typeof bundle !== 'string' || bundle.length === 0 || Buffer.byteLength(bundle) > MAX_FRAME) {
    throw new Error('invalid bundle');
  }
  context = vm.createContext(vm.constants.DONT_CONTEXTIFY, {
    microtaskMode: 'afterEvaluate',
    codeGeneration: {strings: true, wasm: false},
  });
  const prelude = `globalThis.performance={now:()=>Date.now()};
    globalThis.console={log(){},warn(){},error(){},info(){},debug(){}};
    globalThis.__hostLog=()=>{};
    globalThis.__workerSliders=[];
    globalThis.__hostSlider=v=>v;
    globalThis.__hostSliderBind=v=>(__workerSliders.push(v)-1);
    globalThis.__hostSliderRead=i=>__workerSliders[i];
    globalThis.__hostPrebake=()=>{throw new Error('prebakes are unavailable in the query worker')};`;
  new vm.Script(prelude).runInContext(context, {timeout: timeoutMs});
  new vm.Script(bundle, {filename: 'tompouce-runtime.js'}).runInContext(context, {timeout: timeoutMs});
  compileScript = new vm.Script(`globalThis.__compileResult=undefined;globalThis.__compileError=undefined;
    __compile(__source,__bpm,'').then(x=>{__compileResult=x},e=>{__compileError=String(e)});`);
  queryScript = new vm.Script('__query(__begin,__end,__cps,false)');
}

function compile(source, bpm, timeoutMs) {
  if (typeof source !== 'string' || Buffer.byteLength(source) > MAX_SOURCE) throw new Error('source limit');
  context.__source = source;
  context.__bpm = Number.isFinite(bpm) && bpm > 0 ? bpm : 120;
  compileScript.runInContext(context, {timeout: timeoutMs});
  if (context.__compileError) throw new Error(String(context.__compileError).slice(0, 512));
  if (typeof context.__compileResult !== 'string') throw new Error('compile did not settle');
  return context.__compileResult;
}

function query(begin, end, cps, timeoutMs) {
  context.__begin = begin;
  context.__end = end;
  context.__cps = Number.isFinite(cps) && cps > 0 ? cps : 0.5;
  const text = queryScript.runInContext(context, {timeout: timeoutMs});
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
    return {ok: true, metadata: compile(frame.source, frame.bpm, budget)};
  case 'query':
    if (!context) throw new Error('not initialized');
    return {ok: true, events: query(frame.begin, frame.end, frame.cps, budget)};
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
      const result = handle(frame, budget);
      if (result) write({id, ...result});
    } catch (error) {
      write({id, ok: false, error: String(error?.message ?? error).slice(0, 512)});
    }
  }
});
process.stdin.on('end', () => process.exit(0));
