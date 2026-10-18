/* Native audio host for the unmodified upstream Strudel pattern libraries. */
import * as core from '@strudel/core';
import * as mini from '@strudel/mini';
import * as tonal from '@strudel/tonal';
import * as transpileModule from '@strudel/transpiler';
import { parse } from 'acorn';

Object.assign(globalThis, core, mini, tonal);
const { transpiler } = transpileModule;
const modules = { '@strudel/core': core, '@strudel/mini': mini, '@strudel/tonal': tonal, '@strudel/transpiler': transpileModule };
globalThis.__strudelModules = modules;
globalThis.__moduleSource = (name) => {
  if (!Object.hasOwn(modules, name)) throw new Error(`Unavailable module: ${name}`);
  const names = Object.keys(modules[name]).filter((key) => /^[a-zA-Z_$][\w$]*$/.test(key) && key !== 'default');
  return `const m=globalThis.__strudelModules[${JSON.stringify(name)}];\n` +
    names.map((key) => `export const ${key}=m.${key};`).join('\n');
};
globalThis.__import = async (name) => {
  if (!Object.hasOwn(modules, name)) return import(name);
  return modules[name];
};
globalThis.__importBinding = async (name, key) => {
  const module = await __import(name);
  if (!Object.hasOwn(module, key)) throw new SyntaxError(`${name} does not export ${key}`);
  return module[key];
};
let selected = core.silence;
let collecting = new Map();
let anonymous = 0;
let eachTransform;
let allTransforms = [];
let initialBpm = 120;
let tempo;
let sampleCalls = [];
let source = '';
let locations = [];
let visuals = 0;
let visualSites = [];
let visualTiles = new Map();
const visualKinds = {scope:1, tscope:1, pianoroll:2, punchcard:3, spiral:4, pitchwheel:5, spectrum:6, wordfall:7};
const visualKind = name => visualKinds[name?.replace(/^_/, '')] ?? 0;
const nativeVisualOptions = new Set(('cycles playhead overscan minMidi maxMidi thickness scale pos trigger stretch steady inset size padding '+
  'playheadLength playheadThickness hapRadius margin root min max speed vertical flipTime flipValues labels fold autorange '+
  'hideNegative hideInactive colorizeInactive fill fillActive stroke strokeActive smear align edo circle hapcircles mode fade logSpiral '+
  'active inactive activeColor inactiveColor background playheadColor color').split(' '));
const warnedVisualOptions = new Set();
function attachVisual(site, pattern, options = {}) {
  if (!core.isPattern(pattern)) throw new Error('Visual feedback needs a pattern');
  if (!options || typeof options !== 'object' || Array.isArray(options)) throw new Error('Visual options must be an object');
  const entries = Object.entries(options);
  if (entries.length > 48) throw new Error('Too many visual options');
  const clean = site.kind === 7 ? {vertical:1, labels:1} : {};
  for (const [key,value] of entries) {
    if (key.length > 48 || (typeof value === 'number' && !Number.isFinite(value))) throw new Error('Invalid visual option');
    if (!nativeVisualOptions.has(key)) {
      if (!warnedVisualOptions.has(key)) { __hostLog(`Native visual option ${key} is unavailable`); warnedVisualOptions.add(key); }
      continue;
    }
    if (typeof value === 'boolean' || typeof value === 'number' || (typeof value === 'string' && value.length <= 128)) clean[key] = value;
    else throw new Error(`Visual option ${key} must be a bounded number, boolean or string`);
  }
  site.options = clean;
  visuals |= 1 << (site.kind-1);
  return pattern.withContext(context => ({...context, scopeMask: (((context?.scopeMask ?? 0) | (1 << site.id)) >>> 0)}));
}
let aliases = new Map();
let orchestraCalls = [];
let csoundUsed = false;
let pianoUsed = false;

globalThis.performance = { now: () => Date.now() };
globalThis.console = Object.fromEntries(['log', 'warn', 'error', 'info', 'debug'].map(
  (name) => [name, (...values) => __hostLog(values.map(String).join(' '))],
));
core.Pattern.prototype.p = function (id = '$') {
  id = String(id);
  if (id.startsWith('_') || id.endsWith('_')) return core.silence;
  if (id.includes('$')) id += anonymous++;
  collecting.set(id, this);
  return this;
};
core.Pattern.prototype.q = function () { return core.silence; };
for (const [name,kind] of Object.entries(visualKinds)) for (const spelling of [name,`_${name}`]) {
  core.Pattern.prototype[spelling] = function (options = {}) {
    throw new Error(`Use a direct ${spelling}(...) call so its terminal view can be attached to source`);
  };
  globalThis[spelling] = options => pattern => pattern[spelling](options);
}
if (!core.Pattern.prototype.markcss) Object.assign(globalThis,core.registerControl(['markcss']));
for (let i = 1; i <= 9; i++) {
  for (const name of [`p${i}`, `d${i}`])
    Object.defineProperty(core.Pattern.prototype, name, { get() { return this.p(i); }, configurable: true });
  core.Pattern.prototype[`q${i}`] = core.silence;
}
globalThis.all = (fn) => { allTransforms.push(fn); return core.silence; };
globalThis.each = (fn) => { eachTransform = fn; return core.silence; };
globalThis.cpm = core.register('cpm', (value, pattern) => pattern._fast(Number(value) / 60 / ((tempo ?? initialBpm) / 240)));
modules['@strudel/core'] = { ...core, cpm: globalThis.cpm };
globalThis.setcps = globalThis.setCps = (v) => {
  v = Number(core.isPattern(v) ? v.queryArc(0, 1)[0]?.value : v);
  if (!Number.isFinite(v) || v <= 0) throw new Error('setcps requires a positive finite value');
  tempo = Math.max(20, Math.min(400, v * 240));
};
globalThis.setcpm = globalThis.setCpm = (v) => setcps(Number(v) / 60);
globalThis.hush = () => { collecting = new Map(); selected = core.silence; };
globalThis.sliderWithID = (_id, value, min = 0, max = 1, step) =>
  __hostSlider(Number(value), Number(min), Number(max), Number(step ?? ((max - min) / 20)));
globalThis.slider = (value, min, max, step) => sliderWithID('', value, min, max, step);
globalThis.samples = async (...args) => {
  if (sampleCalls.length >= 64) throw new Error('Too many sample maps');
  sampleCalls.push(args);
};
/* Match @strudel/repl/prebake.mjs, including its pitch-dependent pan. */
core.Pattern.prototype.piano = function () {
  pianoUsed = true;
  return this.fmap((v) => ({ ...v, clip: v.clip ?? 1 })).s('piano').release(.1)
    .fmap((v) => ({ ...v, pan: (v.pan || 1) * (Math.min(Math.round(core.valueToMidi(v))/core.noteToMidi('C8'),1)*.5+.25) }));
};
globalThis.loadCsound = globalThis.loadCSound = globalThis.loadcsound = async (code = '', ...values) => {
  if (Array.isArray(code) && Object.hasOwn(code,'raw'))
    code = code.reduce((s,part,i) => s+part+(i < values.length ? String(values[i]) : ''),'');
  if (typeof code !== 'string' || code.length > 65536 || orchestraCalls.length >= 16)
    throw new TypeError('loadCsound requires bounded orchestra text');
  orchestraCalls.push({ code }); csoundUsed = true;
};
globalThis.loadOrc = async (url) => {
  if (typeof url !== 'string' || url.length > 480 || orchestraCalls.length >= 16)
    throw new TypeError('loadOrc requires a bounded URL');
  orchestraCalls.push({ url }); csoundUsed = true;
};
globalThis.csound = core.register('csound',(instrument,pattern) => {
  csoundUsed = true;
  return pattern.fmap((value) => ({ ...value, csound: instrument || 'triangle' }));
});
const csoundMethod = core.Pattern.prototype.csound;
core.Pattern.prototype.csound = function (...args) {
  csoundUsed = true;
  return csoundMethod.apply(this,args);
};
const csoundFunction = globalThis.csound;
globalThis.csound = (...args) => { csoundUsed = true; return csoundFunction(...args); };
const csoundMidiFunction = core.register('csoundm',(instrument,pattern) =>
  pattern.fmap((value) => ({ ...value, csound: instrument, _csoundMidi: true })));
const csoundMidiMethod = core.Pattern.prototype.csoundm;
core.Pattern.prototype.csoundm = function (...args) {
  csoundUsed = true;
  return csoundMidiMethod.apply(this,args);
};
globalThis.csoundm = (...args) => { csoundUsed = true; return csoundMidiFunction(...args); };
modules['@strudel/csound'] = {
  csound, csoundm, loadCsound, loadCSound, loadcsound, loadOrc,
};
globalThis.soundAlias = (original, alias) => {
  if (typeof original !== 'string' || typeof alias !== 'string' || original.length > 128 || alias.length > 128 || aliases.size >= 256)
    throw new TypeError('soundAlias requires bounded string names');
  aliases.set(alias, original);
};
core.setTime(() => 0);
core.setCpsFunc(() => (tempo ?? initialBpm) / 240);

/* Static imports are hoisted; blanking preserves source offsets for mini. */
function prepare(code) {
  const ast = parse(code, { ecmaVersion: 2022, sourceType: 'module', allowAwaitOutsideFunction: true });
  const edits = [];
  function visual(property) {
    const kind = visualKind(property.name);
    if (!kind) return;
    if (visualSites.length >= 32) throw new Error('At most 32 inline visualizations are supported');
    const id = visualSites.length;
    const alias = ('v' + String(id).padStart(property.end-property.start-1, '0'));
    const site = {id, offset:property.start, kind, options:{}};
    visualSites.push(site); edits.push([property.start,property.end,alias]);
    core.Pattern.prototype[alias] = function (options = {}) { return attachVisual(site,this,options); };
    globalThis[alias] = function (options = {}) {
      return core.isPattern(options) ? attachVisual(site,options) : pattern => attachVisual(site,pattern,options);
    };
  }
  function visit(node) {
    if (!node || typeof node !== 'object') return;
    if (node.type === 'CallExpression' && node.callee.type === 'MemberExpression' && !node.callee.computed) {
      const property = node.callee.property;
      visual(property);
    } else if (node.type === 'CallExpression' && node.callee.type === 'Identifier') {
      visual(node.callee);
      if (['all','each'].includes(node.callee.name) && node.arguments[0]?.type === 'Identifier') visual(node.arguments[0]);
    }
    for (const value of Object.values(node)) {
      if (Array.isArray(value)) value.forEach(visit);
      else if (value && typeof value === 'object') visit(value);
    }
  }
  visit(ast);
  /* Same-length aliases keep the upstream mini-notation source offsets intact. */
  for (const [a,b,text] of edits.sort((a,b) => b[0]-a[0])) code = code.slice(0,a)+text+code.slice(b);
  let prefix = '';
  const blanks = [];
  for (const node of ast.body) {
    if (node.type !== 'ImportDeclaration') continue;
    const name = JSON.stringify(node.source.value);
    for (const item of node.specifiers) {
      if (item.type === 'ImportNamespaceSpecifier') prefix += `const ${item.local.name}=await __import(${name});\n`;
      else {
        const key = item.type === 'ImportDefaultSpecifier' ? 'default' : item.imported.name ?? item.imported.value;
        prefix += `const ${item.local.name}=await __importBinding(${name},${JSON.stringify(key)});\n`;
      }
    }
    if (!node.specifiers.length) prefix += `await __import(${name});\n`;
    blanks.push([node.start, node.end]);
  }
  for (const [a, b] of blanks.reverse())
    code = code.slice(0, a) + code.slice(a, b).replace(/[^\n]/g, ' ') + code.slice(b);
  if (ast.body.length && ast.body.at(-1).type !== 'ExpressionStatement' &&
      ast.body.at(-1).type !== 'LabeledStatement') code += '\nsilence';
  const result = transpiler(code);
  return { ...result, output: prefix + result.output };
}

globalThis.__compile = async (code, bpm = 120) => {
  collecting = new Map(); anonymous = 0; eachTransform = undefined; allTransforms = [];
  sampleCalls = []; tempo = undefined; initialBpm = bpm; visuals = 0; visualSites = []; visualTiles = new Map(); aliases = new Map();
  orchestraCalls = []; csoundUsed = false; pianoUsed = false;
  const result = prepare(code);
  const value = await Function(`"use strict";return (async()=>{${result.output}})()`)();
  let pattern = collecting.size ? core.stack(...Array.from(collecting.values(), (p) => eachTransform ? eachTransform(p) : p)) : value;
  if (pattern === undefined) pattern = core.silence;
  if (!core.isPattern(pattern)) pattern = core.reify(pattern);
  for (const transform of allTransforms) pattern = transform(pattern);
  selected = pattern;
  source = code;
  locations = result.miniLocations ?? [];
  if (pianoUsed) sampleCalls.unshift(['https://raw.githubusercontent.com/felixroos/dough-samples/main/piano.json']);
  return JSON.stringify({ bpm: tempo, samples: sampleCalls, locations, visuals, visualSites, voices: Math.max(1, collecting.size), csound: csoundUsed ? orchestraCalls : null });
};

globalThis.__query = (begin, end, cps) => {
  core.setTime(() => begin);
  cps = Number.isFinite(cps) && cps > 0 ? cps : (tempo ?? initialBpm)/240;
  const haps = selected.queryArc(begin, end, { _cps: cps });
  if (haps.length > 4096) throw new Error('Pattern exceeds 4096 haps per query');
  const events = [];
  for (const hap of haps) {
    if (!hap.hasOnset() || !hap.whole) continue;
    const at = Number(hap.whole.begin), finish = Number(hap.whole.end);
    if (at < begin || at >= end || !Number.isFinite(at) || !Number.isFinite(finish)) continue;
    let value = typeof hap.value === 'object' && hap.value !== null ? hap.value : { note: hap.value };
    if (aliases.has(value.s)) value = { ...value, s: aliases.get(value.s) };
    const locs = hap.context?.locations ?? [];
    const values = Array.isArray(value.note) ? value.note.map((note) => ({ ...value, note })) : [value];
    for (const item of values) {
      if (events.length >= 4096) throw new Error('Pattern exceeds 4096 events per query');
      events.push({ begin: at, end: finish, value: item, locations: locs, scopeMask: hap.context?.scopeMask ?? 0 });
    }
  }
  return JSON.stringify(events);
};

/* Separate, bounded drawing queries. Only the generator calls this; no visual
   query is scheduled as audio, and the UI never executes user JavaScript. */
globalThis.__visualQuery = (time, cps) => {
  core.setTime(() => time);
  const started = Date.now(), windows = new Map(), jobs = [];
  const quantum = .125;
  for (const site of visualSites) {
    // Waveforms need only the audio tap; querying the final pattern here can
    // otherwise prevent an unrelated piano-roll snapshot from ever completing.
    if (site.kind === 1) continue;
    const options = site.options;
    const cycles = Math.max(.125,Math.min(16,Number(options.cycles ?? 4)));
    const playhead = Math.max(0,Math.min(1,Number(options.playhead ?? .5)));
    const overscan = Math.max(0,Math.min(4,Number(options.overscan ?? 1)));
    let begin = time-cycles*playhead-overscan, end = time+cycles*(1-playhead)+overscan;
    if (options.hideNegative) begin = Math.max(0,begin);
    // Terminal feedback follows the same transformed timeline as audio.
    // Site context tags retain branch isolation through stack/slow/etc.
    const pattern = selected;
    if (!pattern) continue;
    windows.set(site.id,{begin,end});
    for (let tile=Math.floor(begin/quantum); tile<Math.ceil(end/quantum); tile++) {
      const key=`${site.id}:${tile}`, cached=visualTiles.get(key);
      jobs.push({site,pattern,tile,key,cached,priority:cached && cached.cps===cps ? 1e6+cached.updated : Math.abs((tile+.5)*quantum-time)});
    }
  }
  for (const [key,tile] of visualTiles) {
    const window=windows.get(tile.site);
    if (!window || tile.end<=window.begin || tile.begin>=window.end || tile.cps!==cps) visualTiles.delete(key);
  }
  jobs.sort((a,b)=>a.priority-b.priority);
  let queried=0;
  for (const job of jobs) {
    if (queried && Date.now()-started>=12) break;
    const {site,pattern,tile,key,cached}=job;
    if (cached && cached.cps===cps && started-cached.updated<500) continue;
    const begin=tile*quantum,end=begin+quantum;
    const events=[];
    const haps = pattern.queryArc(begin,end,{_cps:cps});
    if (haps.length > 4096) throw new Error('Visual pattern exceeds 4096 haps');
    for (const hap of haps) {
      if (!hap.whole || !((hap.context?.scopeMask ?? 0)&(1 << site.id))) continue;
      const at = Number(hap.whole.begin), finish = Number(hap.whole.end);
      if (!Number.isFinite(at) || !Number.isFinite(finish)) continue;
      let value = typeof hap.value === 'object' && hap.value !== null ? hap.value : {note:hap.value};
      if (aliases.has(value.s)) value = {...value,s:aliases.get(value.s)};
      const values = Array.isArray(value.note) ? value.note.map(note => ({...value,note})) : [value];
      for (const item of values) {
        if (events.length >= 4096) throw new Error('Visual patterns exceed 4096 events');
        events.push({begin:at,end:finish,value:item,visualId:site.id});
      }
    }
    let total=events.length;
    for (const [other,record] of visualTiles) if (other!==key) total+=record.events.length;
    if (total>8192) throw new Error('Visual tile cache exceeds 8192 events');
    visualTiles.set(key,{site:site.id,begin,end,cps,events,updated:started});
    queried++;
    if (visualTiles.size>256) {
      const oldest=[...visualTiles].sort((a,b)=>a[1].updated-b[1].updated)[0];
      visualTiles.delete(oldest[0]);
    }
  }
  const events=[],seen=new Set();
  for (const record of visualTiles.values()) {
    const window=windows.get(record.site);
    if (!window) continue;
    const occurrences=new Map();
    for (const event of record.events) {
      if (event.end<window.begin || event.begin>window.end) continue;
      const identity=JSON.stringify(event), occurrence=occurrences.get(identity) ?? 0;
      occurrences.set(identity,occurrence+1);
      const key=`${identity}:${occurrence}`;
      if (seen.has(key)) continue;
      seen.add(key);
      if (events.length>=4096) throw new Error('Visual patterns exceed 4096 events');
      events.push(event);
    }
  }
  return JSON.stringify(events);
};

/* Useful in the standalone language tests, without adding a production API. */
globalThis.__runtimeVersion = 'Strudel 1.2.6';
