/* Native preparation only. No JavaScript from this module runs in audio. */
import { parse } from 'acorn';
import * as kabelCore from '@kabelsalat/core';
import * as kabel from '@kabelsalat/lib';

/**
 * Build a native Kabelsalat source or effect using Strudel's K(...) source
 * transform. sFreq is the event frequency and sGate follows its note gate.
 * Include audioin() for an effect on the existing sound; otherwise the graph
 * is the sound source. Feedback needs a delay. Device input, custom UGens,
 * MIDI/mouse graph nodes and arbitrary JavaScript nodes are unavailable.
 * @name K
 * @param {expression | function} graph Kabelsalat graph, at most 256 native operations
 * @example
 * note("c3 e3 g3").K(sine(sFreq).mul(sGate.adsr(.001,.1,.5,.1)).out())
 * @example
 * note(48).s('sawtooth').K(audioin().fold(.5).out())
 */
/**
 * Bridge a Strudel pattern into a K(...) graph input. This is a source
 * transform inside K, rather than an ordinary standalone runtime call.
 * @name S
 * @param {Pattern} pattern values to supply to the graph per event
 * @example
 * note("c3 e3").K(sine(sFreq).mul(S(rand.range(.1,.5))).out())
 */
/**
 * Event frequency in hertz inside K(...). Use it as a graph input, not as a
 * function. Each accepted note keeps the frequency prepared for that event.
 * @name sFreq
 * @example
 * note("c3 e3").K(sine(sFreq).out())
 */
/**
 * Note gate inside K(...). Combine it with a graph envelope to control the
 * source's attack, decay, sustain and release.
 * @name sGate
 * @example
 * note(60).K(sine(sFreq).mul(sGate.adsr(.01,.1,.6,.2)).out())
 */
const LIMIT = 256;
const graphTypes = new Set(('n time nativeGate audioin exit poly output src thru '+
  'add sub mul div mod pow exp log sin cos tan asin acos atan abs round floor ceil sign min max '+
  'clamp clip range remap midinote greater lower xor and or not bool ifelse '+
  'sine saw zaw tri pulse noise brown pink lcgnoise dust impulse clock clockdiv '+
  'adsr delay hold lag slide slew filter bpf fold distort seq pick trig').split(' '));
const binary = new Set(['+','-','*','/','%','**','&','|','^','<<','>>','>>>','<','<=','>','>=','==','!=','===','!==','&&','||']);
const unary = new Set(['+','-','~','!']);
const math = new Set(['sin','cos','tan','asin','acos','atan','abs','floor','ceil','round','sqrt','exp','log','log2','log10','pow','min','max','sign','trunc']);
const constants = {PI:Math.PI,E:Math.E,LN2:Math.LN2,LN10:Math.LN10,LOG2E:Math.LOG2E,LOG10E:Math.LOG10E,SQRT2:Math.SQRT2,SQRT1_2:Math.SQRT1_2};
// Presets from superdough 1.2.6 synth.mjs (Strudel contributors, AGPL-3.0-or-later).
export const bytebeatPresets = [
  '(t%255 >= t/255%255)*255', '(t*(t*8%60 <= 300)|(-t)*(t*4%512 < 256))+t/400',
  't', 't*(t >> 10^t)', 't&128', 't&t>>8',
  '((t%255+t%128+t%64+t%32+t%16+t%127.8+t%64.8+t%32.8+t%16.8)/3)',
  '((t%64+t%63.8+t%64.15+t%64.35+t%63.5)/1.25)', '(t&(t>>7)-t)',
  '(sin(t*PI/128)*127+127)', '((t^t/2+t+64*(sin((t*PI/64)+(t*PI/32768))+64))%128*2)',
  '((t^t/2+t+64*(cos >> 0))%127.85*2)', '((t^t/2+t+64)%128*2)',
  '(((t * .25)^(t * .25)/100+(t * .25))%128)*2', '((t^t/2+t+64)%7 * 24)',
];
function emit(ops, op, ins=[], value=0) {
  if (ops.length >= LIMIT) throw new Error('Native synth exceeds 256 operations');
  ops.push({op,ins,value}); return ops.length-1;
}
function expression(code) {
  if (typeof code !== 'string' || code.length>4096) throw new Error('Bytebeat requires at most 4096 characters');
  const ast=parse(`(${code})`,{ecmaVersion:2022});
  const ops=[];
  function node(n, depth=0) {
    if (depth>64) throw new Error('Bytebeat expression is too deep');
    const child=x=>node(x,depth+1);
    if (n.type==='Literal' && typeof n.value==='number' && Number.isFinite(n.value)) return emit(ops,'n',[],n.value);
    if (n.type==='Identifier') {
      if (n.name==='t') return emit(ops,'byteTime');
      if (Object.hasOwn(constants,n.name)) return emit(ops,'n',[],constants[n.name]);
      // A Math function coerced to a number becomes NaN (preset 11).
      if (math.has(n.name)) return emit(ops,'nan');
    }
    if (n.type==='MemberExpression' && !n.computed && n.object.name==='Math' && Object.hasOwn(constants,n.property.name))
      return emit(ops,'n',[],constants[n.property.name]);
    if ((n.type==='BinaryExpression' || n.type==='LogicalExpression') && binary.has(n.operator))
      return emit(ops,n.operator,[child(n.left),child(n.right)]);
    if (n.type==='UnaryExpression' && unary.has(n.operator)) return emit(ops,'unary'+n.operator,[child(n.argument)]);
    if (n.type==='ConditionalExpression') return emit(ops,'ifelse',[child(n.test),child(n.consequent),child(n.alternate)]);
    if (n.type==='CallExpression') {
      const name=n.callee.type==='Identifier'?n.callee.name:
        n.callee.type==='MemberExpression' && !n.callee.computed && n.callee.object.name==='Math'?n.callee.property.name:'';
      if ((math.has(name) || name==='int') && n.arguments.length>=1 && n.arguments.length<=8)
        return emit(ops,name==='int'?'floor':name,n.arguments.map(child));
    }
    if (n.type==='SequenceExpression' && n.expressions.length<=16) {
      let last; for (const part of n.expressions) last=child(part); return last;
    }
    throw new Error(`Unsupported native bytebeat expression: ${n.type}`);
  }
  const result=node(ast.body[0].expression);
  return {kind:'bytebeat',ops,result};
}

const graphs=new Map(), beats=new Map();
function graph(code, frequency, inputs=[]) {
  if (typeof code!=='string' || code.length>16384 || inputs.length>32 || inputs.some(x=>typeof x!=='number' || !Number.isFinite(x)))
    throw new Error('Invalid native Kabelsalat source/inputs');
  const key=JSON.stringify([code,frequency,inputs]);
  if (graphs.has(key)) return graphs.get(key);
  const scope={...kabelCore,...kabel,sFreq:frequency,sGate:kabelCore.node('nativeGate'),pat:inputs};
  const root=kabelCore.evaluate(`return (${code});`,scope);
  const ordered=[], visited=new Set(), visiting=new Set();
  function visit(node, depth=0) {
    if (!node || typeof node!=='object' || !Array.isArray(node.ins) || depth>64) throw new Error('Invalid or deep Kabelsalat graph');
    if (visited.has(node) || visiting.has(node)) return;
    if (!graphTypes.has(node.type)) throw new Error(`Unsupported native Kabelsalat node: ${node.type}`);
    if (visited.size+visiting.size>=LIMIT || node.ins.length>16) throw new Error('Native Kabelsalat graph is too large');
    visiting.add(node); node.ins.forEach(x=>visit(x,depth+1)); visiting.delete(node);
    visited.add(node); ordered.push(node);
  }
  visit(root);
  if (!ordered.some(n=>n.type==='output')) throw new Error('Kabelsalat requires .out()');
  const ids=new Map(ordered.map((n,i)=>[n,i]));
  const ops=ordered.map((n,i)=>{
    const ins=n.ins.map(x=>ids.get(x));
    if (ins.some((id,k)=>id>=i && !(n.type==='delay' && k===0))) throw new Error('Kabelsalat feedback requires a delay');
    if (n.type==='n' && (typeof n.value!=='number' || !Number.isFinite(n.value))) throw new Error('Kabelsalat constants must be finite numbers');
    if (n.type==='output' && (!n.ins[1] || n.ins[1].type!=='n' || ![0,1].includes(n.ins[1].value)))
      throw new Error('Native Kabelsalat outputs are channels 0 and 1');
    return {op:n.type,ins,value:n.value??0};
  });
  const result={kind:'graph',ops,result:ops.length-1,effect:ordered.some(n=>n.type==='audioin')};
  if (graphs.size>=256) graphs.delete(graphs.keys().next().value);
  graphs.set(key,result); return result;
}
export function prepareNative(value, frequency=440) {
  if (value.workletSrc!==undefined) return {...value,_nativeProgram:graph(value.workletSrc,frequency,value.workletInputs??[])};
  if (value.s?.split(':')[0]!=='bytebeat') return value;
  const n=Number(value.n??value.s.split(':')[1]??0);
  const code=value.byteBeatExpression??bytebeatPresets[((Math.floor(n)%15)+15)%15];
  let program=beats.get(code);
  if (!program) {
    program=expression(code);
    if (beats.size>=256) beats.delete(beats.keys().next().value);
    beats.set(code,program);
  }
  const offset=value.byteBeatStartTime;
  if (offset!==undefined && (typeof offset!=='number' || !Number.isFinite(offset) || Math.abs(offset)>1e12)) throw new Error('Invalid bytebeat start time');
  return {...value,_nativeProgram:{...program,...(offset===undefined?{}:{offset:Math.floor(offset)})}};
}
export function installNative(core) {
  /**
   * Supply a custom bytebeat expression, compiled to native operations before
   * playback. Select s('bytebeat') to use it. The variable t is bytebeat time;
   * arithmetic, bitwise operators and supported numeric Math calls are allowed.
   * Expressions are at most 4096 characters. Loops, assignments, arbitrary
   * object access and arbitrary JavaScript function calls are rejected.
   * @name bbexpr
   * @synonyms byteBeatExpression
   * @param {string} code bytebeat expression, in single quotes
   * @example
   * note("c3 e3").s('bytebeat').bbexpr('t*(t>>5|t>>8)')
   */
  for (const name of ['bbexpr','byteBeatExpression']) {
    const method=core.Pattern.prototype[name];
    core.Pattern.prototype[name]=function(code) {
      if (typeof code==='string') expression(code);
      return method.call(this,code);
    };
  }
  /**
   * Supply a supported Kabelsalat graph source string and optional per-event
   * inputs. K(...) normally constructs this automatically. Native execution
   * compiles a bounded graph; it does not execute JavaScript in the callback.
   * @name worklet
   * @param {string} code supported Kabelsalat graph source, at most 16384 characters
   * @param {...number | Pattern} inputs optional graph inputs
   * @example
   * note(60).worklet('sine(440).out()')
   */
  const original=core.Pattern.prototype.worklet;
  core.Pattern.prototype.worklet=function(code,...inputs) {
    // Reject unsupported graph syntax before replacing a playable program.
    graph(code,440,inputs.map(()=>0));
    if (!inputs.length) return this.fmap(value=>({...value,workletSrc:code,workletInputs:[]}));
    return original.call(this,code,...inputs);
  };
}
