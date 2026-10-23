/* Native audio host for the unmodified upstream Strudel pattern libraries. */
import * as core from '@strudel/core';
import * as mini from '@strudel/mini';
import * as tonal from '@strudel/tonal';
import * as transpileModule from '@strudel/transpiler';
import { parse } from 'acorn';
import { installNative, prepareNative } from './native-synth.mjs';
import { queryFraction } from './query-time.mjs';

Object.assign(globalThis, core, mini, tonal);
installNative(core);
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
let queryCps;
let sampleCalls = [];
let source = '';
let locations = [];
let visuals = 0;
let visualSites = [];
let visualTiles = new Map();
let visualProbes = [], compiledVisualSites = [], nextProbe = 0, compiledHighlightAst;
const visualAliases = new Set();
const liveSliderReaders = new Map();
/**
 * Show a waveform for this pattern beneath its source line. Only this branch's
 * audio feeds the scope. Both spellings attach an inline terminal view.
 * _scope defaults to scale 1, pos .5; scope defaults to scale .25, pos .75.
 * @name scope
 * @synonyms _scope tscope _tscope
 * @param {object} options scale, pos (vertical center), trigger and styling options
 * @example
 * note("c3 e3 g3").s('sine')._scope({scale:1,pos:.5})
 */
/**
 * Show the notes of this pattern on a piano roll. Visual-only edits do not
 * reevaluate the playing music. Inline views follow the branch's transformations.
 * @name pianoroll
 * @synonyms _pianoroll
 * @param {object} options cycles, playhead, minMidi, maxMidi, vertical and labels
 * @example
 * note("c3 e3 g3 b3").pianoroll({cycles:4,minMidi:48,maxMidi:84})
 */
/**
 * Show a pattern's event timing as a punchcard beneath its source line.
 * @name punchcard
 * @synonyms _punchcard
 * @param {object} options cycles, playhead, vertical, labels and color options
 * @example
 * s("bd hh sd hh")._punchcard({cycles:2})
 */
/**
 * Display pattern events around a spiral. This is a terminal visualization;
 * rendering reads prepared visual data and never evaluates the music.
 * @name spiral
 * @synonyms _spiral
 * @param {object} options cycles, playhead and color options
 * @example
 * note("c3 e3 g3 b3").spiral()
 */
/**
 * Display the pattern's pitches on a pitch wheel beneath its source line.
 * @name pitchwheel
 * @synonyms _pitchwheel
 * @param {object} options root, edo and color options
 * @example
 * note("c3 e3 g3").pitchwheel({edo:12})
 */
/**
 * Display the frequency spectrum of this pattern's audio. FFT work belongs
 * to foreground visualization preparation, not the audio callback.
 * @name spectrum
 * @synonyms _spectrum
 * @param {object} options visualization styling options
 * @example
 * note("c2 e2").s('sawtooth')._spectrum()
 */
/**
 * Display falling pattern events with labels beneath the source line.
 * Defaults to a vertical view with labels enabled.
 * @name wordfall
 * @synonyms _wordfall
 * @param {object} options cycles, vertical, labels, flipTime and flipValues
 * @example
 * s("bd hh sd hh").wordfall({cycles:2,labels:1})
 */
const visualKinds = {scope:1, tscope:1, pianoroll:2, punchcard:3, spiral:4, pitchwheel:5, spectrum:6, wordfall:7};
const visualKind = name => visualKinds[name?.replace(/^_/, '')] ?? 0;
const visualDefaults = (kind, inline) => kind===7 ? {vertical:1,labels:1} :
  kind===1 && inline ? {pos:.5,scale:1} : {};
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
  const clean = visualDefaults(site.kind,site.inline);
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

/* Observe existing branches without reevaluating the user's program when a
   display is edited. Instrument the transpiled output so mini offsets remain
   those of the original source. Hidden tags never change musical values. */
function visualKey(node) {
  if (!node || typeof node !== 'object') return node;
  if (node.type==='CallExpression' && node.callee.name==='slider' && node.arguments.length)
    return {type:'CallExpression',callee:visualKey(node.callee),arguments:[{type:'LiveSlider'},...node.arguments.slice(1).map(visualKey)]};
  if (node.type==='CallExpression' && node.callee.type==='CallExpression' &&
      (visualKind(node.callee.callee.name) || visualAliases.has(node.callee.callee.name)) && node.arguments.length===1)
    return visualKey(node.arguments[0]);
  if (node.type==='CallExpression' && (visualKind(node.callee.name) || visualAliases.has(node.callee.name)) &&
      node.arguments.length===1 && node.arguments[0].type!=='ObjectExpression') return visualKey(node.arguments[0]);
  if (node.type === 'CallExpression' && node.callee.type === 'MemberExpression' && !node.callee.computed &&
      (visualKind(node.callee.property.name) || visualAliases.has(node.callee.property.name) || node.callee.property.name === 'p'))
    return visualKey(node.callee.object);
  if (node.type === 'CallExpression' && node.callee.name === 'm' && node.arguments.length === 2 &&
      node.arguments[0].type === 'Literal') return {type:'Mini', value:node.arguments[0].value};
  if (node.type === 'Literal' && typeof node.value === 'string' && node.raw?.startsWith('"'))
    return {type:'Mini', value:node.value};
  if (node.type === 'TemplateLiteral' && node.expressions.length === 0)
    return {type:'Mini', value:node.quasis[0].value.raw};
  const clean = {};
  for (const key of Object.keys(node).sort()) {
    if (['start','end','loc','raw'].includes(key)) continue;
    clean[key] = Array.isArray(node[key]) ? node[key].map(visualKey) : visualKey(node[key]);
  }
  return clean;
}
const branchKey = node => JSON.stringify(visualKey(node));
function walkVisualSource(ast, visit) {
  function walk(node, root) {
    if (!node || typeof node !== 'object' || !node.type) return;
    if (node.type === 'CallExpression') {
      const chain = root ?? node;
      visit(node, chain);
      if (node.callee.type === 'MemberExpression') walk(node.callee.object, chain);
      else walk(node.callee, chain);
      for (const arg of node.arguments) walk(arg, null);
      return;
    }
    for (const value of Object.values(node)) {
      if (Array.isArray(value)) value.forEach(child => walk(child, null));
      else if (value && typeof value === 'object') walk(value, null);
    }
  }
  walk(ast, null);
}
function observeBranches(output) {
  const ast = parse(output,{ecmaVersion:2022,allowAwaitOutsideFunction:true,allowReturnOutsideFunction:true});
  const insertions = [];
  walkVisualSource(ast,(node,root) => {
    if (node !== root || node.callee.name === 'm' || visualProbes.length >= 512) return;
    const id = visualProbes.length, key=branchKey(node);
    const occurrence=visualProbes.filter(probe => probe.key===key).length;
    visualProbes.push({key, occurrence, start:node.start, end:node.end, mask:0});
    const target=node.callee.type==='MemberExpression' && node.callee.property.name==='p' ? node.callee.object : node;
    insertions.push([target.start,`__observeBranch(${id},`],[target.end,')']);
  });
  function bindings(node) {
    if (!node || typeof node!=='object') return;
    if (node.type==='VariableDeclarator' && node.id.type==='Identifier' && node.init) {
      const probe=visualProbes.find(probe => probe.start===node.init.start && probe.end===node.init.end);
      if (probe) (probe.names ??= []).push(branchKey(node.id));
    }
    for (const value of Object.values(node)) {
      if (Array.isArray(value)) value.forEach(bindings);
      else if (value && typeof value==='object') bindings(value);
    }
  }
  bindings(ast);
  for (const [at,text] of insertions.sort((a,b) => b[0]-a[0])) output=output.slice(0,at)+text+output.slice(at);
  return output;
}
globalThis.__observeBranch = (id, value) => {
  const probe=visualProbes[id];
  if (!probe || !core.isPattern(value)) return value;
  if (!probe.mask && nextProbe < 32) probe.mask=(1 << nextProbe++) >>> 0;
  return probe.mask ? value.withContext(context => ({...context,probeMask:((context?.probeMask ?? 0)|probe.mask) >>> 0})) : value;
};

/* Parse display requests only: no Function/eval, imports, sliders or sample
   declarations execute here. Options accept literal data, never user code. */
function literalOption(node) {
  if (node?.type === 'Literal' && ['string','number','boolean'].includes(typeof node.value)) return node.value;
  if (node?.type === 'UnaryExpression' && ['+','-'].includes(node.operator) && typeof node.argument.value === 'number')
    return node.operator === '-' ? -node.argument.value : node.argument.value;
  throw new Error('Live visual options require literal values');
}

function visualDirective(node) {
  if (node?.type!=='ExpressionStatement') return false;
  const call=node.expression, arg=call.arguments?.[0];
  return call.type==='CallExpression' && ['all','each'].includes(call.callee.name) &&
    (visualKind(arg?.name) || (arg?.type==='CallExpression' && visualKind(arg.callee.name)));
}
function highlightNode(node) {
  if (node?.type!=='CallExpression') return node;
  const callee=node.callee;
  if (callee.type==='MemberExpression' && !callee.computed && visualKind(callee.property.name))
    return highlightNode(callee.object);
  if (callee.type==='CallExpression' && visualKind(callee.callee.name) && node.arguments.length===1)
    return highlightNode(node.arguments[0]);
  if (visualKind(callee.name) && node.arguments.length===1 && node.arguments[0].type!=='ObjectExpression')
    return highlightNode(node.arguments[0]);
  return node;
}
/* Compare musical syntax before accepting any offsets. Only display calls,
   comments and layout may differ; never paint old notes on changed music.
   The map contains literal ranges, in original/editor UTF-16 coordinates. */
function highlightMapping(ast, editor) {
  const mapping=[];
  function compare(a,b) {
    a=highlightNode(a); b=highlightNode(b);
    if (a===null || b===null || typeof a!=='object' || typeof b!=='object') return a===b;
    if (a.type==='CallExpression' && b.type==='CallExpression' && a.callee.name==='slider' && b.callee.name==='slider' &&
        a.arguments.length===b.arguments.length && a.arguments.length) {
      const read=liveSliderReaders.get(`slider_${a.arguments[0].start}`);
      let live=false;
      try { live=read && literalOption(b.arguments[0])===read(); } catch {}
      if (live) return compare(a.callee,b.callee) && compare(a.arguments.slice(1),b.arguments.slice(1));
    }
    if (Array.isArray(a) || Array.isArray(b)) {
      if (!Array.isArray(a) || !Array.isArray(b)) return false;
      const left=a.filter(node=>!visualDirective(node)), right=b.filter(node=>!visualDirective(node));
      return left.length===right.length && left.every((node,i)=>compare(node,right[i]));
    }
    if ((a.type==='Literal' && typeof a.value==='string') || (a.type==='TemplateLiteral' && !a.expressions.length)) {
      if (source.slice(a.start,a.end)!==editor.slice(b.start,b.end)) return false;
      if (mapping.length<8192) mapping.push([a.start,a.end,b.start,b.end]);
    }
    const keys=node=>Object.keys(node).filter(key=>!['start','end','loc'].includes(key) && !(key==='raw' && node.type==='Literal')).sort();
    const left=keys(a), right=keys(b);
    return left.length===right.length && left.every((key,i)=>key===right[i] && compare(a[key],b[key]));
  }
  return compare(compiledHighlightAst,ast) ? mapping : null;
}
globalThis.__configureVisuals = code => {
  const ast=parse(code,{ecmaVersion:2022,sourceType:'module',allowAwaitOutsideFunction:true});
  const sites=[], roots=new Map(), occurrences=new Map();
  walkVisualSource(ast,(node,root) => {
    const key=branchKey(root);
    if (!roots.has(root)) {
      roots.set(root,occurrences.get(key) ?? 0);
      occurrences.set(key,roots.get(root)+1);
    }
    let property=node.callee.type === 'MemberExpression' && !node.callee.computed ? node.callee.property : node.callee;
    const whole=['all','each'].includes(node.callee.name) && node.arguments[0]?.type==='Identifier';
    if (whole) property=node.arguments[0];
    const kind=visualKind(property.name);
    if (!kind || sites.length >= 32) return;
    const occurrence=roots.get(root);
    const original=compiledVisualSites.find(site => site.key===key && site.occurrence===occurrence && site.kind===kind);
    const candidates=visualProbes.filter(probe => ((probe.key===(whole?'@all':key) && (whole || probe.occurrence===occurrence)) || probe.names?.includes(key)) && probe.mask);
    const probe=candidates.length===1 ? candidates[0] : undefined;
    const mask=original ? (1 << original.id) >>> 0 : probe?.mask;
    if (!mask) return;
    let options=visualDefaults(kind,property.name.startsWith('_'));
    if (!whole && node.arguments.length && (node.callee.type==='MemberExpression' || node.arguments[0].type==='ObjectExpression')) {
      const object=node.arguments[0];
      if (object.type !== 'ObjectExpression') {
        if (!original) return;
        options=original.options;
      } else {
        if (object.properties.length > 48) return;
        try {
          for (const prop of object.properties) {
            if (prop.type !== 'Property' || prop.computed || prop.method || prop.kind !== 'init') throw new Error('literal options required');
            const name=prop.key.name ?? prop.key.value;
            if (!nativeVisualOptions.has(name)) continue;
            const value=literalOption(prop.value);
            if ((typeof value==='number' && !Number.isFinite(value)) || (typeof value==='string' && value.length>128)) throw new Error('invalid option');
            options[name]=value;
          }
        } catch { if (original) options=original.options; else return; }
      }
    }
    sites.push({id:sites.length,offset:property.start,kind,options,mask,key});
  });
  visualSites=sites; visualTiles=new Map();
  return JSON.stringify({sites:sites.map(({key,...site}) => site),highlightMap:highlightMapping(ast,code)});
};
let aliases = new Map();
let orchestraCalls = [];
let csoundUsed = false;
let pianoUsed = false;

globalThis.performance = { now: () => Date.now() };
globalThis.console = Object.fromEntries(['log', 'warn', 'error', 'info', 'debug'].map(
  (name) => [name, (...values) => __hostLog(values.map(String).join(' '))],
));
/**
 * Register a named pattern branch for playback. Branches are stacked together.
 * An id beginning or ending with an underscore mutes that branch; '$' ids
 * receive unique suffixes. Dollar-label syntax uses this helper automatically.
 * @name p
 * @param {string | number} id branch identifier, default '$'
 * @example
 * s('bd*4').p('drums')
 * note("c3 e3 g3").p('melody')
 */
core.Pattern.prototype.p = function (id = '$') {
  id = String(id);
  if (id.startsWith('_') || id.endsWith('_')) return core.silence;
  if (id.includes('$')) id += anonymous++;
  collecting.set(id, this);
  return this;
};
/**
 * Replace this pattern with silence.
 * @name q
 * @example
 * s('bd*4').q()
 */
core.Pattern.prototype.q = function () { return core.silence; };
for (const [name,kind] of Object.entries(visualKinds)) for (const spelling of [name,`_${name}`]) {
  core.Pattern.prototype[spelling] = function (options = {}) {
    throw new Error(`Use a direct ${spelling}(...) call so its terminal view can be attached to source`);
  };
  globalThis[spelling] = options => pattern => pattern[spelling](options);
}
/**
 * Style the currently sounding source tokens in the terminal. Native marking
 * supports underline, bold and italic; arbitrary browser CSS is unavailable.
 * @name markcss
 * @param {string | Pattern} style supported text CSS declarations
 * @example
 * note("c3 e3 g3").markcss('font-weight:bold')
 */
if (!core.Pattern.prototype.markcss) Object.assign(globalThis,core.registerControl(['markcss']));
for (let i = 1; i <= 9; i++) {
  for (const name of [`p${i}`, `d${i}`])
    Object.defineProperty(core.Pattern.prototype, name, { get() { return this.p(i); }, configurable: true });
  core.Pattern.prototype[`q${i}`] = core.silence;
}
/**
 * Apply a transform to the complete pattern after named branches are combined.
 * Multiple all() calls compose in source order.
 * @name all
 * @param {function} fn transform receiving and returning a Pattern
 * @example
 * all(p => p.lpf(1200))
 * $: s('bd*4')
 * $: note("c3 e3 g3")
 */
globalThis.all = (fn) => { allTransforms.push(fn); return core.silence; };
/**
 * Apply a transform separately to each registered named branch before stacking.
 * The last each() call in an evaluation supplies the transform.
 * @name each
 * @param {function} fn transform receiving and returning a Pattern
 * @example
 * each(p => p.gain(.5))
 * $: s('bd*4')
 * $: s('hh*8')
 */
globalThis.each = (fn) => { eachTransform = fn; return core.silence; };
/**
 * Select the OSC destination host for a pattern routed with osc(). Native
 * output accepts localhost or literal IPv4/IPv6 addresses, not DNS names.
 * @name oschost
 * @param {string | Pattern} host destination, default '127.0.0.1'
 * @example
 * s('bd*4').oschost('127.0.0.1').oscport(57120).osc()
 */
/**
 * Select the UDP OSC destination port for a pattern routed with osc().
 * @name oscport
 * @param {number | Pattern} port UDP destination port, default 57120
 * @example
 * note("c3 e3").oscport(57120).osc()
 */
/**
 * Route this pattern to the configured OSC output instead of local audio.
 * OSC address, value and packet limits are checked before sending.
 * @name osc
 * @example
 * note("c3 e3 g3").osc()
 */
core.Pattern.prototype.osc = function () { return this.fmap(value=>({...((value && typeof value==='object')?value:{note:value}),_osc:true})); };
globalThis.osc = pattern=>pattern.osc();
/* MIDI controller input, mirroring the upstream input package: midin opens
   a sender and returns a per-CC live reader (values normalized 0..1, default
   0); midikeys returns live keyboard notes with fixed durations. Native
   values merge across senders into one store; without a connected sender the
   readers stay at their defaults, and midikeys rejects a missing device.
   Named senders persist CC state across restarts, restore it to the device
   on (re)connect, and resubscribe by client name after replugs. Native readers
   currently handle CC and note-on input. Held notes sound on the next scheduled
   window after arrival (like live slider moves) and fire once: audio queries
   consume, visual queries peek. */
let midiConsumeKeys = false;
const midiInputError = (name) => new Error(
  `[midi] Midi input cannot be a pattern. Make sure to pass device name with single quotes.`);
/**
 * Open a MIDI controller input and return a live CC reader. Values are
 * normalized to 0..1, defaulting to zero. Native input merges senders; named
 * sender CC state persists across restarts. Pass device names in single quotes.
 * @name midin
 * @param {string | number} input optional ALSA sender name or client id
 * @returns {function} reader(cc, channel?) returning a live Pattern
 * @example
 * const cc = await midin('My Controller')
 * s('sawtooth').note(48).lpf(cc(1).range(200,5000))
 */
globalThis.midin = async (input) => {
  if (core.isPattern(input)) throw midiInputError('midin');
  __hostMidiOpen(input ?? 0);
  return (cc, chan) => core.ref(() => __hostMidiRead(cc, chan));
};
/**
 * Open a MIDI keyboard and return a live note-pattern function. Notes carry
 * pitch, velocity and channel. A missing device rejects the evaluation.
 * @name midikeys
 * @param {string | number} input optional ALSA sender name or client id
 * @returns {function} keyboard(noteLength=.5), with lengths in cycles
 * @example
 * const keyboard = await midikeys('My Keyboard')
 * keyboard(.25).s('sine')
 */
globalThis.midikeys = async (input) => {
  if (core.isPattern(input)) throw midiInputError('midikeys');
  const open = __hostMidiOpen(input ?? 0);
  if (!open) throw new Error(`[midi] Midi device "${input ?? 0}" not found. Connect a sender with aconnect.`);
  const tag = `${input ?? 0}`;
  const kb = (noteLength = 0.5) => {
    const nlPat = core.reify(noteLength);
    const query = (state) => {
      const windowBegin = Number(state.span.begin);
      const keys = __hostMidiKeys(midiConsumeKeys ? 1e12 : -1);
      const haps = [];
      let offset = 0;
      for (const key of keys) {
        const at = windowBegin + offset; offset += 0.001;
        const durs = nlPat.query(state.setSpan(new core.TimeSpan(at, at)));
        const nl = durs.length ? Number(durs[0].value) : 0.5;
        if (!Number.isFinite(nl) || nl <= 0) continue;
        const span = new core.TimeSpan(at, at + nl);
        const value = { _midikey: `${tag}_${key.note}`, note: key.note, velocity: key.velocity, midichan: key.midichan };
        haps.push(new core.Hap(span, span, value, {}));
      }
      return haps;
    };
    return new core.Pattern(query);
  };
  return kb;
};
/* Dominant MIDI output, mirroring the web routing (local audio bypassed,
   channel from midichan defaulting to 1). The port argument only selects a
   web device; native ALSA subscribers receive every note. An options object
   may carry midichannel as a default when the hap sets no midichan. */
/**
 * Send MIDI notes instead of local audio. Native ALSA subscribers receive the
 * notes; the web port selector does not select a separate native output port.
 * Set the channel with midichan(), defaulting to channel 1.
 * @name midi
 * @param {string | object} port optional web device selector or settings object
 * @param {object} options optional settings, including midichannel
 * @example
 * note("c3 e3 g3").midichan(2).midi()
 */
core.Pattern.prototype.midi = function (port, options) {
  if (core.isPattern(port)) throw new Error(`.midi does not accept Pattern input for midiport. Make sure to pass device name with single quotes.`);
  const settings = port && typeof port === 'object' ? {...options,...port} : {...options};
  const channel = Number.isFinite(settings.midichannel) ? settings.midichannel : undefined;
  return this.fmap(value=> {
    const object = {...((value && typeof value==='object')?value:{note:value})};
    object._midi = true;
    if (channel !== undefined && object.midichan == null) object.midichan = channel;
    return object;
  });
};
/**
 * Set this pattern's rate in cycles per minute relative to the current clock.
 * Use setcpm() to change the shared transport tempo instead.
 * @name cpm
 * @param {number | Pattern} value cycles per minute
 * @example
 * s('bd hh sd hh').cpm(30)
 */
globalThis.cpm = core.register('cpm', (value, pattern) => pattern._fast(Number(value) / 60 / (queryCps ?? ((tempo ?? initialBpm) / 240))));
modules['@strudel/core'] = { ...core, cpm: globalThis.cpm };
/**
 * Change the shared tempo in cycles per second. Values must be finite and
 * nonnegative. For four beats per cycle, BPM divided by 240 gives CPS.
 * @name setcps
 * @synonyms setCps
 * @param {number | Pattern} value cycles per second
 * @example
 * setcps(120/240)
 * s('bd hh sd hh')
 */
globalThis.setcps = globalThis.setCps = (v) => {
  v = Number(core.isPattern(v) ? v.queryArc(0, 1)[0]?.value : v);
  if (!Number.isFinite(v) || v < 0 || !Number.isFinite(v*240)) throw new Error('setcps requires a nonnegative finite value');
  tempo = v*240;
};
/**
 * Change the shared tempo in cycles per minute. With four beats per cycle,
 * use BPM divided by four. Accepted notes keep their scheduled timing.
 * @name setcpm
 * @synonyms setCpm
 * @param {number | Pattern} value nonnegative finite cycles per minute
 * @example
 * setcpm(120/4)
 * s('bd hh sd hh')
 */
globalThis.setcpm = globalThis.setCpm = (v) => {
  v = core.isPattern(v) ? v.queryArc(0,1)[0]?.value : v;
  setcps(Number(v) / 60);
};
/**
 * Clear the registered branches and select silence for this evaluation.
 * Already accepted notes and shared effect tails can finish.
 * @name hush
 * @example
 * hush()
 */
globalThis.hush = () => { collecting = new Map(); selected = core.silence; };
/**
 * Internal source-identity helper used by direct slider() calls. Prefer
 * slider(value,min,max,step) in artist-written source; the transpiler assigns
 * the stable identity and the host supplies query-time values.
 * @name sliderWithID
 * @param {string} id source identity assigned by the transpiler
 * @param {number} value initial value
 * @param {number} min minimum, default 0
 * @param {number} max maximum, default 1
 * @param {number} step optional step size
 * @example
 * note(60).gain(slider(.5,0,1,.01))
 */
globalThis.sliderWithID = (id, value, min = 0, max = 1, step) => {
  const args=[Number(value),Number(min),Number(max),Number(step ?? ((max-min)/1000))];
  // Reference/browser tooling without the native live-control host keeps its
  // original numeric contract. Native patterns read controls at query time.
  if (typeof __hostSliderBind !== 'function') return __hostSlider(...args);
  const index=__hostSliderBind(...args, id);
  const read=()=>__hostSliderRead(index);
  liveSliderReaders.set(id,read);
  return core.ref(read);
};
/**
 * Create an interactive terminal slider. Evaluate once to activate it, then
 * drag its chip or use Ctrl+Left/Right at its source to change future queries.
 * Accepted notes continue. Use a direct slider(...) call: aliases and member
 * calls retain the web's static fallback. Live tempo still requires evaluation.
 * @name slider
 * @param {number} value initial value
 * @param {number} min minimum, default 0
 * @param {number} max maximum, default 1
 * @param {number} step optional step, default (max-min)/1000
 * @example
 * note("c3 e3 g3").gain(slider(.5,0,1,.01))
 */
globalThis.slider = (value) => {
  // Only direct calls are rewritten to source-bound sliderWithID widgets.
  // Aliases/member calls have the web fallback's ordinary pure-pattern value.
  __hostLog('slider will only work when the transpiler is used... passing value as is');
  return core.pure(value);
};
/**
 * Register sample maps for this evaluation. Explicit maps override imports,
 * which override defaults. Assets pass through the host's confined sound store;
 * arbitrary local filesystem paths are not an alternative import mechanism.
 * @name samples
 * @param {object | string} source sample-name map or allowed manifest URL
 * @param {string} base optional base URL for relative map entries
 * @example
 * await samples({custom:['https://example.org/audio/hit.wav']})
 * s('custom')
 */
globalThis.samples = async (...args) => {
  if (sampleCalls.length >= 64) throw new Error('Too many sample maps');
  sampleCalls.push(args);
};
/* Match @strudel/repl/prebake.mjs, including its pitch-dependent pan. */
/**
 * Select the default piano sample map with the REPL helper's clipping,
 * 0.1-second release and pitch-dependent panning.
 * @name piano
 * @example
 * note("c3 e3 g3 b3").piano()
 */
core.Pattern.prototype.piano = function () {
  pianoUsed = true;
  return this.fmap((v) => ({ ...v, clip: v.clip ?? 1 })).s('piano').release(.1)
    .fmap((v) => ({ ...v, pan: (v.pan || 1) * (Math.min(Math.round(core.valueToMidi(v))/core.noteToMidi('C8'),1)*.5+.25) }));
};
/**
 * Load orchestra text into the persistent confined Csound renderer. Existing
 * definitions and sounding instances survive later evaluations. The renderer
 * cannot use orchestra code to open files, access the network or run processes.
 * @name loadCsound
 * @synonyms loadCSound loadcsound
 * @param {string | template} code orchestra source, at most 65536 characters per call
 * @example
 * await loadCsound`instr Tone
 * a1 oscili p5,p4
 * out a1,a1
 * endin`
 * note("c3 e3 g3").csound('Tone')
 */
globalThis.loadCsound = globalThis.loadCSound = globalThis.loadcsound = async (code = '', ...values) => {
  if (Array.isArray(code) && Object.hasOwn(code,'raw'))
    code = code.reduce((s,part,i) => s+part+(i < values.length ? String(values[i]) : ''),'');
  if (typeof code !== 'string' || code.length > 65536 || orchestraCalls.length >= 16)
    throw new TypeError('loadCsound requires bounded orchestra text');
  orchestraCalls.push({ code }); csoundUsed = true;
};
/**
 * Load a Csound orchestra from an allowed public HTTP(S) or github: URL.
 * Fetching uses the host's asset policy; orchestra execution stays confined.
 * Successful and failed URL loads are cached for the Csound session.
 * @name loadOrc
 * @param {string} url orchestra URL, at most 480 characters
 * @example
 * await loadOrc('github:kunstmusik/csound-live-code/master/livecode.orc')
 * note("c3 e3").csound('FM1')
 */
globalThis.loadOrc = async (url) => {
  if (typeof url !== 'string' || url.length > 480 || orchestraCalls.length >= 16)
    throw new TypeError('loadOrc requires a bounded URL');
  orchestraCalls.push({ url }); csoundUsed = true;
};
/**
 * Route events to a named or numeric Csound instrument instead of local audio.
 * Named instruments receive frequency, gain and the Strudel control map as
 * p4/p5/p6. Requires the native Csound renderer and loaded definitions.
 * @name csound
 * @param {string | number} instrument name or fractional instance number
 * @example
 * note("c3 e3 g3").gain(.2).csound('triangle')
 */
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
/**
 * Route events to Csound using MIDI key and velocity pfields. Fractional note
 * numbers are preserved. Use csound() for frequency/gain/control-map pfields.
 * @name csoundm
 * @param {string | number} instrument loaded Csound instrument or instance
 * @example
 * note("60 64 67").csoundm('MidiInstrument')
 */
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
/**
 * Register another name for an existing sound during this evaluation.
 * Both names must be strings of at most 128 characters; up to 256 aliases
 * can be registered. Define the original sound before using its alias.
 * @name soundAlias
 * @param {string} original existing sound name
 * @param {string} alias new sound name
 * @example
 * soundAlias('sine','lead')
 * note("c3 e3 g3").s('lead')
 */
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
  compiledHighlightAst=ast;
  const edits = [];
  function visual(property) {
    const kind = visualKind(property.name);
    if (!kind) return;
    if (visualSites.length >= 32) throw new Error('At most 32 inline visualizations are supported');
    const id = visualSites.length;
    const alias = ('v' + String(id).padStart(property.end-property.start-1, '0'));
    visualAliases.add(alias);
    const site = {id, offset:property.start, kind, inline:property.name.startsWith('_'), options:{}};
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
  const roots=new Map(), occurrences=new Map();
  walkVisualSource(ast,(node,root) => {
    const key=branchKey(root);
    if (!roots.has(root)) { roots.set(root,occurrences.get(key) ?? 0); occurrences.set(key,roots.get(root)+1); }
    let property=node.callee.type==='MemberExpression' ? node.callee.property : node.callee;
    if (['all','each'].includes(node.callee.name) && node.arguments[0]?.type==='Identifier') property=node.arguments[0];
    for (const site of visualSites) if (site.offset===property.start) { site.key=key; site.occurrence=roots.get(root); }
  });
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
  liveSliderReaders.clear();
  collecting = new Map(); anonymous = 0; eachTransform = undefined; allTransforms = [];
  sampleCalls = []; tempo = undefined; initialBpm = bpm; visuals = 0; visualSites = []; visualTiles = new Map(); aliases = new Map();
  queryCps=undefined;
  orchestraCalls = []; csoundUsed = false; pianoUsed = false;
  visualProbes=[]; visualAliases.clear();
  const result = prepare(code);
  compiledVisualSites=visualSites;
  nextProbe=visualSites.length;
  const value = await Function(`"use strict";return (async()=>{${observeBranches(result.output)}})()`)();
  let pattern = collecting.size ? core.stack(...Array.from(collecting.values(), (p) => eachTransform ? eachTransform(p) : p)) : value;
  if (pattern === undefined) pattern = core.silence;
  if (!core.isPattern(pattern)) pattern = core.reify(pattern);
  for (const transform of allTransforms) pattern = transform(pattern);
  const whole=visualProbes.length;
  visualProbes.push({key:'@all',mask:0});
  selected = __observeBranch(whole,pattern);
  source = code;
  locations = result.miniLocations ?? [];
  if (pianoUsed) sampleCalls.unshift(['https://raw.githubusercontent.com/felixroos/dough-samples/main/piano.json']);
  return JSON.stringify({ bpm: tempo, samples: sampleCalls, locations, visuals, visualSites, voices: Math.max(1, collecting.size), csound: csoundUsed ? orchestraCalls : null });
};

/* JSON turns non-finite numbers into null. Duck numeric conversion accepts
   their JS spellings, including inside arrays; non-finite onset spellings
   reject the same downward endpoint as numeric addition would. Keep OSC's
   original event values on its separate output path. */
function prepareDuckNumbers(value) {
  if (value._osc) return value;
  const encode = (item, depth = 0) => {
    if (typeof item === 'number' && !Number.isFinite(item)) return String(item);
    // Native JSON strings are NUL-terminated. NUL is invalid in every duck
    // numeric conversion, including concatenated onset times and arrays.
    if (typeof item === 'string' && item.includes('\0')) return 'NaN';
    if (Array.isArray(item) && depth < 128) return item.map(entry => encode(entry, depth + 1));
    return item;
  };
  let result = value;
  for (const key of ['duckonset', 'duckattack', 'duckatt', 'datt', 'duckdepth']) {
    if (!Object.hasOwn(value, key)) continue;
    if (result === value) result = { ...value };
    let item = value[key];
    // The controller reuses entry zero for nullish/missing array entries.
    // An undefined first entry reaches the orbit's parameter defaults, except
    // attack has already passed through Math.max(undefined, .002).
    if (Array.isArray(item) && item[0] === undefined) {
      item = item.slice();
      item[0] = key === 'duckonset' ? 0 : key === 'duckdepth' ? 1 : 'NaN';
    }
    result[key] = encode(item);
  }
  return result;
}

globalThis.__query = (begin, end, cps, probes = false) => {
  // The upstream query converts boundaries to rational time. Compare using
  // those same boundaries: raw accumulated doubles can drop exact onsets.
  const beginTime=queryFraction(core.Fraction,begin),endTime=queryFraction(core.Fraction,end);
  begin=Number(beginTime); end=Number(endTime);
  core.setTime(() => begin);
  cps = Number.isFinite(cps) && cps > 0 ? cps : (tempo ?? initialBpm)/240;
  queryCps=cps;
  midiConsumeKeys = true;
  let haps;
  try {
    haps = selected.queryArc(beginTime, endTime, { _cps: cps });
  } finally {
    midiConsumeKeys = false;
  }
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
      const mask=(hap.context?.scopeMask ?? 0) | (probes ? hap.context?.probeMask ?? 0 : 0);
      const frequency=item.workletSrc===undefined?440:Number.isFinite(item.freq)?item.freq:
        440*Math.pow(2,(core.valueToMidi({note:36,...item})-69)/12);
      events.push({ begin: at, end: finish, value: prepareNative(prepareDuckNumbers(item),frequency), locations: locs, scopeMask:mask >>> 0 });
    }
  }
  return JSON.stringify(events);
};

/* Separate, bounded drawing queries. Only the generator calls this; no visual
   query is scheduled as audio, and the UI never executes user JavaScript. */
globalThis.__visualQuery = (time, cps) => {
  queryCps=cps;
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
      const mask=site.mask ?? (1 << site.id);
      if (!hap.whole || !(((hap.context?.scopeMask ?? 0)|(hap.context?.probeMask ?? 0))&mask)) continue;
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
