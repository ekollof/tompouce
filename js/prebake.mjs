import {parse} from 'acorn';

let baseDefaults=new Map(), defaults=new Map(), gainCurve=null, loaded=new Map();
let transpile, log;
let setupDepth=0;
export const isPrebaking=()=>setupDepth>0;
const originalDefaults={s:'triangle',gain:.8,postgain:1,density:'.03',ftype:'12db',fanchor:0,
  resonance:1,hresonance:1,bandq:1,channels:[1,2],phaserdepth:.75,shapevol:1,distortvol:1,
  distorttype:0,delay:0,byteBeatExpression:'0',delayfeedback:.5,delaysync:3/16,orbit:1,i:1,velocity:1,fft:8};

export function resetPrebakes() {
  baseDefaults=new Map(); defaults=new Map(); gainCurve=null; loaded=new Map();
  setupDepth=0;
}

export function installPrebakes(core,transpiler) {
  transpile=transpiler;
  log=message=>globalThis.__hostLog(message);
  globalThis.window=globalThis;
  globalThis.register=(name,...args)=>{
    const helper=core.register(name,...args); globalThis[name]=helper; return helper;
  };
  globalThis.setGainCurve=fn=>{
    if(typeof fn!=='function')throw new TypeError('setGainCurve requires a function');
    gainCurve=fn;
  };
  const put=(map,key,value)=>{
    if(typeof key!=='string' || !/^[a-zA-Z][a-zA-Z0-9]*$/.test(key) || key.length>64 ||
       (!map.has(key) && map.size>=128))throw new TypeError('invalid or excessive audio defaults');
    map.set(key,value);
  };
  globalThis.setDefault=(key,value)=>put(baseDefaults,key,value);
  globalThis.setDefaultValue=(key,value)=>put(defaults,key,value);
  globalThis.getDefaultValue=key=>defaults.has(key)?defaults.get(key):Object.hasOwn(originalDefaults,key)?originalDefaults[key]:undefined;
  globalThis.setDefaultValues=object=>{
    if(!object || typeof object!=='object' || Object.keys(object).length>128)throw new TypeError('invalid audio defaults');
    for(const [key,value] of Object.entries(object))put(defaults,key,value);
  };
  globalThis.resetDefaults=()=>{baseDefaults=new Map();};
  globalThis.resetDefaultValues=()=>{defaults=new Map(baseDefaults);};
  globalThis.loadPrebake=async name=>{
    if(typeof name!=='string')throw new TypeError('loadPrebake requires a file name');
    if(loaded.has(name))return loaded.get(name);
    if(loaded.size>=16)throw new RangeError('at most 16 named prebakes');
    // Mark before executing to break cycles. C enforces file/count/byte bounds
    // independently, and disables reads once pattern queries begin.
    loaded.set(name,undefined);
    const promise=runPrebake(globalThis.__hostPrebake(name),name).then(()=>{globalThis.resetDefaultValues();});
    loaded.set(name,promise);
    return promise;
  };
}

export function prepareAudioDefaults(value) {
  if(value._midi || value._osc || value.csound!==undefined || (!defaults.size && !gainCurve))return value;
  const overrides=Object.fromEntries([...defaults].filter(([key])=>Object.hasOwn(originalDefaults,key)));
  const result={...overrides,...value};
  if(gainCurve)for(const [key,fallback] of Object.entries({gain:.8,postgain:1,shapevol:1,distortvol:1,delay:0,velocity:1,tremolodepth:1})) {
    const input=result[key]===undefined?fallback:result[key];
    const output=gainCurve(input);
    if(typeof output!=='number' || !Number.isFinite(output))throw new TypeError(`gain curve returned invalid ${key}`);
    result[key]=output;
  }
  return result;
}

export async function runPrebake(code,name='prebake.strudel') {
  setupDepth++;
  try {
    if(code.startsWith('// tompouce-prebake: switchangel\n')) {
      // The known browser Volume widget maps to the native final-output
      // control. Other DOM callbacks retain their normal errors.
      const ast=parse(code,{ecmaVersion:2022,allowAwaitOutsideFunction:true});
      for(const statement of [...ast.body].reverse()) {
        const call=statement.expression, callee=call?.callee;
        if(call?.type==='CallExpression' && callee?.type==='MemberExpression' &&
           !callee.computed && callee.object?.name==='window' && callee.property?.name==='UISlider' &&
           call.arguments[1]?.type==='Literal' && call.arguments[1].value==='Volume') {
          code=code.slice(0,statement.start)+code.slice(statement.start,statement.end).replace(/[^\n]/g,' ')+code.slice(statement.end);
          log('Switch Angel Volume mapped to F4 output volume (/volume 0..1)');
        }
      }
    }
    const setup=transpile(code+'\n;void 0',{addReturn:false,emitMiniLocations:false,emitWidgets:false});
    // The pinned transpiler still embeds mini offsets with location emission
    // disabled. External-file offsets must never highlight the song editor.
    const ast=parse(setup.output,{ecmaVersion:2022,allowAwaitOutsideFunction:true}), edits=[];
    const visit=node=>{
      if(!node || typeof node!=='object')return;
      if(node.type==='CallExpression' && node.callee?.name==='m' && node.arguments.length===2 && node.arguments[1].type==='Literal') {
        const offset=node.arguments[1]; edits.push([offset.start,offset.end,'-1']);
      }
      if(node.type==='CallExpression' && node.callee?.name==='sliderWithID' && node.arguments[0]?.type==='Literal') {
        const id=node.arguments[0]; edits.push([id.start,id.end,JSON.stringify(`prebake:${name}:${id.value}`)]);
      }
      for(const child of Object.values(node))if(Array.isArray(child))child.forEach(visit);else if(child && typeof child==='object')visit(child);
    };
    visit(ast);
    let output=setup.output;
    for(const [start,end,text] of edits.sort((a,b)=>b[0]-a[0]))output=output.slice(0,start)+text+output.slice(end);
    await Function(`"use strict";return (async()=>{${output}\n})()`)();
  }catch(error){throw new Error(`${name}: ${error?.message ?? error}`);}
  finally{setupDepth--;}
}
