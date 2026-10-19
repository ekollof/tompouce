/* Deployed Superdough behavior; private OfflineAudioContexts only. */
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {BrowserClient} from './browser-client.mjs';

async function silentControls() {
  const results=[];
  for (const sound of ['sine','piano']) {
    const runs=[];
    let baseline;
    for (const silent of [false,true]) {
      const context=new OfflineAudioContext(2,Math.round(.7*44100),44100);
      setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
      await initAudio({maxPolyphony:128,multiChannelOrbits:false});
      const group=(sound==='sine'?1100:1200)+(silent?1:0);
      const events=[];
      const stop=AudioScheduledSourceNode.prototype.stop;
      AudioScheduledSourceNode.prototype.stop=function(...args){events.push(args[0]);return stop.apply(this,args)};
      try {
        await superdough({s:sound,note:60,gain:.5,postgain:1,cut:group,attack:.001,decay:0,sustain:1,release:.02,clip:1},.1,.5,.5,0);
        if (silent) await superdough({s:sound,note:60,gain:0,postgain:1,cut:group,attack:.001,decay:0,sustain:1,release:.02,clip:1},.3,.05,.5,0);
        const pcm=(await context.startRendering()).getChannelData(0);
        const ratios=[];
        if (!silent) baseline=Float32Array.from(pcm);
        else for (const time of [.29,.299,.3,.301,.305,.31,.315,.32,.325,.34,.35]) {
          let sum=0,n=0;
          for (let i=Math.round(time*44100);i<Math.round(time*44100)+16;i++)
            if (Math.abs(baseline[i])>1e-6) {sum+=pcm[i]/baseline[i];n++;}
          ratios.push({time,gain:n?sum/n:null});
        }
        const energy=(begin,end)=>{let sum=0;for(let i=Math.round(begin*44100);i<Math.round(end*44100);i++)sum+=pcm[i]**2;return sum;};
        runs.push({silent,before:energy(.2,.25),after:energy(.4,.45),stops:events.slice(),ratios});
      } finally {AudioScheduledSourceNode.prototype.stop=stop;}
    }
    results.push({sound,runs});
  }
  return results;
}
async function duckControls() {
  const results=[],rate=44100;
  for (const depth of [0,.25,1,'0.25',null,false,true,[.25],[[.25]],[null]]) {
    const context=new OfflineAudioContext(2,Math.round(.55*rate),rate);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const source=time=>{const node=context.createConstantSource();node.offset.value=1;node.start(time);return node;};
    await superdough({source,gain:.1,postgain:1,attack:.001,decay:0,sustain:1,release:.01,orbit:1},0,.5,.5,0);
    const orbit=getSuperdoughAudioController().getOrbit(1,[1,2]);
    const calls=[];
    for (const method of ['cancelScheduledValues','setValueAtTime','exponentialRampToValueAtTime']) {
      const original=orbit.output.gain[method].bind(orbit.output.gain);
      orbit.output.gain[method]=(...args)=>{calls.push({method,args,now:context.currentTime});return original(...args);};
    }
    const suspended=context.suspend(.29),rendered=context.startRendering();
    await suspended;
    const callbackAt=context.currentTime;
    // The web schedules ducking through a zero-gain ConstantSource's ended
    // event. Dispatch that event while suspended, so OfflineAudioContext's
    // faster-than-realtime render cannot race the main-thread callback.
    const stop=AudioScheduledSourceNode.prototype.stop;
    let timer;
    AudioScheduledSourceNode.prototype.stop=function(...args){
      if (this instanceof ConstantSourceNode && this.onended) timer=this;
      return stop.apply(this,args);
    };
    try {orbit.duck(.3,0,.1,depth);}
    finally {AudioScheduledSourceNode.prototype.stop=stop;}
    if (!timer?.onended) throw Error('web duck timer was not captured');
    timer.onended.call(timer,new Event('ended'));
    await context.resume();
    const pcm=(await rendered).getChannelData(0),points=[];
    for (const time of [.28,.29,.295,.3,.31,.325,.35,.375,.4,.425])
      points.push({time,gain:pcm[Math.round(time*rate)]/.1});
    results.push({depth,callbackAt,calls,points});
  }
  return results;
}
async function clipControls() {
  const results=[];
  for (const clip of [0,1]) {
    const context=new OfflineAudioContext(2,13230,44100);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    await superdough({s:'sine',note:69,gain:.5,postgain:1,clip,attack:.001,decay:0,sustain:1,release:.02},.1,.1,.5,0);
    const pcm=(await context.startRendering()).getChannelData(0);
    let energy=0;for(const value of pcm)energy+=value*value;
    results.push({clip,energy});
  }
  return results;
}
async function invalidDuckControls() {
  const results=[];
  for(const controls of [
    {depth:-1},{depth:'bad'},{depth:[true]},
    {onset:'0.02'},{attack:'bad'},{attack:'Infinity'},
    {onset:true},{onset:false},{onset:null},
    {onset:'1'},{onset:''},{onset:[.02]},{onset:[null]},
    {onset:'1',attack:1},
    {depth:NaN},{depth:Infinity},{depth:-Infinity},
    {attack:NaN},{attack:Infinity},{attack:-Infinity},
    {onset:NaN},{onset:Infinity},{onset:-Infinity},
    {depth:[[NaN]]},{depth:[[Infinity]]}]) {
    const context=new OfflineAudioContext(2,22050,44100);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const source=time=>{const node=context.createConstantSource();node.start(time);return node;};
    await superdough({source,gain:.1,postgain:1,orbit:1,attack:.001,sustain:1,release:.01},0,.5,.5,0);
    const orbit=getSuperdoughAudioController().getOrbit(1,[1,2]);
    orbit.output.gain.value=.8;
    const suspended=context.suspend(.29),rendered=context.startRendering(); await suspended;
    const callbackAt=context.currentTime;
    const stop=AudioScheduledSourceNode.prototype.stop;
    let timer,error,errorAt,rejectedRamp;
    const ramp=orbit.output.gain.exponentialRampToValueAtTime.bind(orbit.output.gain);
    let ramps=0;
    orbit.output.gain.exponentialRampToValueAtTime=(...args)=>{
      const stage=++ramps;
      try {return ramp(...args);}
      catch(caught) {
        if(errorAt===undefined) {errorAt=context.currentTime;rejectedRamp=stage;}
        throw caught;
      }
    };
    AudioScheduledSourceNode.prototype.stop=function(...args){
      if(this instanceof ConstantSourceNode && this.onended) timer=this;
      return stop.apply(this,args);
    };
    try {orbit.duck(.3,controls.onset??0,controls.attack??.1,controls.depth??.25);}
    finally {AudioScheduledSourceNode.prototype.stop=stop;}
    if(errorAt!==undefined) throw Error('duck automation rejected before its timer callback');
    if(!timer?.onended) throw Error('missing invalid-control duck timer');
    try {timer.onended.call(timer,new Event('ended'));} catch(caught) {error=String(caught);}
    await context.resume(); const pcm=(await rendered).getChannelData(0);
    results.push({controls,error,errorAt,rejectedRamp,callbackAt,
      gain:pcm[Math.round(.45*44100)]/.1});
  }
  return results;
}
async function duckOrbitCreation() {
  const context=new OfflineAudioContext(2,44100,44100);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const controller=getSuperdoughAudioController(),getOrbit=controller.getOrbit.bind(controller);
  const seen=new Set(),created=[],ducked=[];
  controller.getOrbit=(id,...args)=>{
    const orbit=getOrbit(id,...args);
    if(!seen.has(id)) {
      seen.add(id); created.push(id);
      orbit.duck=()=>ducked.push(id);
    }
    return orbit;
  };
  await superdough({s:'sine',gain:0,orbit:1,duckorbit:9},.1,.05,.5,0);
  await superdough({s:'~',orbit:9},.2,.05,.5,0);
  await superdough({s:'~',orbit:1,duckorbit:9},.3,.05,.5,0);
  try {await superdough({s:'__missing_duck_qa__',orbit:7,duckorbit:7},.4,.05,.5,0);} catch {}
  await context.startRendering();
  return {created,ducked};
}
async function duckMapping() {
  const context=new OfflineAudioContext(2,44100,44100);
  setAudioContext(context); setSuperdoughAudioController(null);
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const controller=getSuperdoughAudioController(),calls=[];
  for (const id of [1,2]) controller.getOrbit(id,[1,2]).duck=(...args)=>calls.push({id,args});
  controller.duck([1,2],.3,[.02,.04],[.1,.2],[.25,1]);
  controller.duck([1,2],.4,[.01],[0],[0]);
  await superdough({s:'sine',gain:0,clip:0,duckorbit:1,duckdepth:.25},.5,.1,.5,0);
  await superdough({s:'~',gain:1,duckorbit:1,duckdepth:.5},.6,.1,.5,0);
  try {await superdough({s:'__missing_duck_qa__',duckorbit:1,duckdepth:1},.7,.1,.5,0);}
  catch {}
  controller.duck([1,3],.8,0,.1,1); /* Orbit 3 has never been created. */
  return calls;
}
async function duckTargetCoercion() {
  const context=new OfflineAudioContext(2,4410,44100);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const controller=getSuperdoughAudioController(),results=[];
  let calls=[];
  for (const id of [1,2]) controller.getOrbit(id,[1,2]).duck=()=>calls.push(id);
  for (const target of [1,'1','01','1:2',[1,2],[[1],[2]],[[1,2]],[null,1],['2','1']]) {
    calls=[]; controller.duck(target,.1,0,.1,1);
    results.push({target,called:calls.slice()});
  }
  const ordered=[];
  for (const id of [1,2]) controller.getOrbit(id,[1,2]).duck=(...args)=>ordered.push({id,args});
  controller.duck([2,1,2],.3,[.02,.04,.06],[.1,.2,.3],[.25,.5,1]);
  await context.startRendering();
  return {targets:results,ordered};
}
function numericStringCoercion() {
  return ['\u00a0.25\ufeff','\u2028.25\u2029','\u2003','\u0085.25',
    '0b10','0o10','0x10','+0x10','-0b1','0x1.8','0x1p2',
    'Infinity','-Infinity','1_0','',' \t.25\r\n'].map(text=>String(Number(text)));
}
async function nestedDuckControls() {
  const results=[];
  const inputs=[undefined,[],[undefined],[null],[.25,null],[null,.25],
    [[null]],[[]],[[[]]],[[false]],[[.25]],[[[.25]]],[[1,2]],{},[[{}]],
    Array(2),[undefined,.25]];
  for (let index=0;index<inputs.length;index++) {
    const context=new OfflineAudioContext(2,4410,44100);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const controller=getSuperdoughAudioController(),timers=[],records=[];
    for (const id of [1,2]) {
      const orbit=controller.getOrbit(id,[1,2]),ramps=[];
      orbit.output.gain.value=.8;
      const ramp=orbit.output.gain.exponentialRampToValueAtTime.bind(orbit.output.gain);
      orbit.output.gain.exponentialRampToValueAtTime=(...args)=>{
        ramps.push(args.map(String)); return ramp(...args);
      };
      const duck=orbit.duck;
      orbit.duck=(...args)=>{records.push({id,attack:String(args[2]),ramps});return duck.apply(orbit,args);};
    }
    const stop=AudioScheduledSourceNode.prototype.stop;
    AudioScheduledSourceNode.prototype.stop=function(...args){
      if(this instanceof ConstantSourceNode && this.onended) timers.push(this);
      return stop.apply(this,args);
    };
    try {controller.duck([1,2],.3,inputs[index],inputs[index],inputs[index]);}
    finally {AudioScheduledSourceNode.prototype.stop=stop;}
    if(timers.length!==2) throw Error('missing nested-control duck timers');
    for (let i=0;i<timers.length;i++) {
      try {timers[i].onended.call(timers[i],new Event('ended'));records[i].rejected=0;}
      catch {records[i].rejected=records[i].ramps.length;}
    }
    results.push({index,records});
    // Finish the owned context before the next setAudioContext call. Prevent
    // natural ended events from invoking the manually dispatched timers again.
    for (const timer of timers) timer.onended=null;
    await context.startRendering();
  }
  return results;
}
async function duckOutputRouting() {
  const context=new OfflineAudioContext(2,4410,44100);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const controller=getSuperdoughAudioController(),ducked=[];
  controller.getOrbit(1,[1,2]).duck=()=>ducked.push(1);
  // Building the MIDI pattern must never request access to physical devices.
  // Its scheduling callback is replaced below before any event dispatch.
  navigator.requestMIDIAccess=async()=>{throw Error('QA refuses physical MIDI access');};
  const patterns=[s('~'),s('~').midichan(1),note(60).csound('Fixture'),
    note(60).osc(),note(60).midi('QA fixture')];
  const results=[];
  for (let index=0;index<patterns.length;index++) {
    let tick,time=0,external=0,local=0;
    const pattern=patterns[index].duckorbit(1).withHap(hap=>hap.setContext({
      ...hap.context,...(hap.context.onSchedule?{onSchedule:()=>external++}:{})}));
    const host=repl({getTime:()=>time,setInterval:fn=>(tick=fn,1),clearInterval:()=>{},
      onSchedule:async(hap,at,cps)=>{local++;await webaudioOutput(hap,at,cps);}});
    const before=ducked.length;
    await host.setPattern(pattern,true); time=.1; tick();
    await Promise.resolve(); await Promise.resolve();
    host.stop();
    results.push({index,local,external,ducked:ducked.length-before});
  }
  await context.startRendering();
  return results;
}
async function effectMethods() {
  const context=new OfflineAudioContext(2,44100,44100);
  setAudioContext(context); setSuperdoughAudioController(null);
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const controller=getSuperdoughAudioController(),orbit=controller.getOrbit(1,[1,2]);
  const reverseInput=context.createBuffer(2,4,44100);
  reverseInput.copyToChannel(new Float32Array([1,2,3,4]),0);
  reverseInput.copyToChannel(new Float32Array([5,6,7,8]),1);
  const reversed=reverseBuffer(reverseInput);
  const delay=orbit.getDelay(.1,.5,0),reverb=orbit.getReverb(.1,.05,18000,8000);
  const methods={delay:String(orbit.getDelay),delayMax:delay.delayTime.maxValue,
    feedbackGraph:String(context.createFeedbackDelay),
    feedbackClass:String(delay.constructor),
    reverb:String(orbit.getReverb),generate:String(reverb.generate),
    duck:String(orbit.duck),controllerDuck:String(controller.duck),
    reverseCopy:[Array.from(reversed.getChannelData(0)),Array.from(reversed.getChannelData(1))]};
  methods.irSyntax=s('sine').ir('roomfixture:0').queryArc(0,1).map(hap=>hap.value);
  methods.irIndexSyntax=s('sine').ir('roomfixture').set({i:0}).queryArc(0,1).map(hap=>hap.value);
  try {
    await superdough({s:'sine',gain:0,room:1,ir:'piano:0'},.1,.05,.5,0);
    methods.irColonRejected=false;
  } catch(error) {methods.irColonRejected=true;methods.irColonError=String(error);}
  orbit.getReverb();
  methods.roomAfterOmitted={duration:reverb.duration,fade:reverb.fade,lp:reverb.lp,dim:reverb.dim};
  // Probe the public source path as well as the orbit method: Superdough may
  // supply defaults before invoking getReverb.
  const fresh=controller.getOrbit(2,[1,2]),calls=[];
  const getReverb=fresh.getReverb.bind(fresh);
  fresh.getReverb=(...args)=>{calls.push(args.map(x=>x===undefined?null:x));return getReverb(...args);};
  await superdough({s:'sine',gain:0,room:1,orbit:2},.1,.05,.5,0);
  const initial=fresh.getReverb();
  methods.roomSourceDefault={calls:calls.slice(),duration:initial.duration,
    fade:initial.fade,lp:initial.lp,dim:initial.dim};
  await superdough({s:'sine',gain:0,room:1,orbit:2,
    roomsize:.3,roomfade:.02,roomlp:9000,roomdim:500},.6,.05,.5,0);
  methods.roomBeforeFutureOnset={now:context.currentTime,duration:initial.duration,
    fade:initial.fade,lp:initial.lp,dim:initial.dim};
  await superdough({s:'sine',gain:0,room:1,orbit:2},.7,.05,.5,0);
  methods.roomSourceInherited={duration:initial.duration,fade:initial.fade,
    lp:initial.lp,dim:initial.dim};
  await superdough({s:'sine',gain:0,room:1,orbit:2,roomsize:.4},.8,.05,.5,0);
  methods.roomPartialChange={duration:initial.duration,fade:initial.fade,
    lp:initial.lp,dim:initial.dim};
  const customOrbit=controller.getOrbit(3,[1,2]);
  const customIR=context.createBuffer(2,4410,44100);
  const custom=customOrbit.getReverb(.3,.02,9000,500,customIR,2,.25);
  const customBuffer=custom.buffer;
  customOrbit.getReverb(undefined,undefined,undefined,undefined,customIR);
  methods.customRoomInherited={duration:custom.duration,fade:custom.fade,
    lp:custom.lp,dim:custom.dim,speed:custom.irspeed,begin:custom.irbegin,
    sameBuffer:custom.buffer===customBuffer};
  customOrbit.getReverb(undefined,undefined,undefined,undefined,
    context.createBuffer(2,4410,44100));
  methods.customRoomChanged={duration:custom.duration,fade:custom.fade,
    lp:custom.lp,dim:custom.dim,speed:custom.irspeed??null,begin:custom.irbegin??null};
  const defaultOrbit=controller.getOrbit(4,[1,2]);
  const defaultRoom=defaultOrbit.getReverb(.3,.02,9000,500,customIR);
  const defaultBuffer=defaultRoom.buffer;
  defaultOrbit.getReverb(undefined,undefined,undefined,undefined,customIR,1);
  const explicitBuffer=defaultRoom.buffer;
  defaultOrbit.getReverb(undefined,undefined,undefined,undefined,customIR);
  methods.customExplicitDefault={replaced:explicitBuffer!==defaultBuffer,
    retainedAfterOmission:defaultRoom.buffer===explicitBuffer,
    duration:defaultRoom.duration,fade:defaultRoom.fade,speed:defaultRoom.irspeed};
  orbit.getDelay(.2,.75,.2);
  methods.delayWhileScheduled={time:delay.delayTime.value,feedback:delay.feedback.value};
  orbit.getDelay(.1,.5,.3);
  await context.startRendering();
  methods.delayAfterReturn={time:delay.delayTime.value,feedback:delay.feedback.value};
  return methods;
}
async function delayAutomation() {
  const results=[]; let baseline;
  for (const returning of [false,true]) {
    const context=new OfflineAudioContext(2,88200,44100);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const buffer=context.createBuffer(1,1,44100); buffer.getChannelData(0)[0]=1;
    const source=time=>{const node=context.createBufferSource();node.buffer=buffer;node.start(time);return node;};
    await superdough({source,gain:.5,postgain:1,dry:0,delay:1,delaytime:.25,delayfeedback:.5,release:0},.1,.001,.5,0);
    await superdough({source,gain:0,postgain:1,dry:0,delay:1,delaytime:.5,delayfeedback:.75,release:0},.6,.001,.5,0);
    if (returning)
      await superdough({source,gain:0,postgain:1,dry:0,delay:1,delaytime:.25,delayfeedback:.5,release:0},1.1,.001,.5,0);
    const pcm=(await context.startRendering()).getChannelData(0);
    if (!returning) baseline=Float32Array.from(pcm);
    let difference=0;for(let i=0;i<pcm.length;i++) difference=Math.max(difference,Math.abs(pcm[i]-baseline[i]));
    const node=getSuperdoughAudioController().getOrbit(1,[1,2]).delayNode;
    results.push({returning,difference,time:node.delayTime.value,feedback:node.feedback.value});
  }
  return results;
}
async function roomReplacement() {
  const results=[];
  for (const replace of [false,true]) {
    const context=new OfflineAudioContext(2,13230,44100);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const ir=context.createBuffer(2,13230,44100);
    for(let ch=0;ch<2;ch++) ir.getChannelData(ch).fill(.1);
    const orbit=getSuperdoughAudioController().getOrbit(1,[1,2]);
    const room=orbit.getReverb(1,.1,15000,1000,ir,1,0);
    room.normalize=false; room.buffer=ir;
    const impulse=context.createBuffer(1,1,44100); impulse.getChannelData(0)[0]=1;
    const source=context.createBufferSource(); source.buffer=impulse;
    source.connect(room); source.start(0);
    const suspended=context.suspend(.1),rendered=context.startRendering();
    await suspended;
    const changedAt=context.currentTime;
    if(replace) orbit.getReverb(1,.1,15000,1000,context.createBuffer(2,13230,44100),1,0);
    await context.resume();
    const pcm=(await rendered).getChannelData(0);
    const energy=(a,b)=>{let sum=0;for(let i=Math.round(a*44100);i<Math.round(b*44100);i++)sum+=pcm[i]**2;return sum;};
    results.push({replace,changedAt,before:energy(.05,.08),after:energy(.15,.2)});
  }
  return results;
}
async function generatedRoomHandoff() {
  const context=new OfflineAudioContext(2,4410,44100);
  setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
  await initAudio({maxPolyphony:128,multiChannelOrbits:false});
  const orbit=getSuperdoughAudioController().getOrbit(1,[1,2]);
  const ir=context.createBuffer(2,4410,44100);
  const room=orbit.getReverb(.05,.01,9000,500,ir,1,0);
  const old=room.buffer;
  orbit.getReverb(.06,.01,9000,500);
  const retainedWhilePending=room.buffer===old;
  const intervening=context.createBuffer(2,4410,44100);
  orbit.getReverb(.06,.01,9000,500,intervening,1,0);
  const deadline=performance.now()+5000;
  while(room.buffer===intervening && performance.now()<deadline)
    await new Promise(resolve=>setTimeout(resolve,10));
  const result={retainedWhilePending,replacedWhenReady:room.buffer!==old,
    replacedInterveningCustom:room.buffer!==intervening,
    frames:room.buffer?.length,duration:room.duration};
  const completed=room.buffer;
  let threw=false,retryThrew=false;
  try {orbit.getReverb(0,.01,9000,500);} catch {threw=true;}
  try {orbit.getReverb(0,.01,9000,500);} catch {retryThrew=true;}
  result.failure={threw,retryThrew,retained:room.buffer===completed,duration:room.duration};
  await context.startRendering();
  return result;
}
async function shortDelays() {
  const results=[],rate=44100;
  for (const frames of [.25,.5,32,128,128.5,2205.25]) {
    const context=new OfflineAudioContext(2,13230,rate);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const buffer=context.createBuffer(1,1,rate); buffer.getChannelData(0)[0]=1;
    const source=time=>{const node=context.createBufferSource();node.buffer=buffer;node.start(time);return node;};
    await superdough({source,gain:.5,postgain:1,dry:0,delay:1,delaytime:frames/rate,delayfeedback:.5,release:0},.1,.001,.5,0);
    const pcm=(await context.startRendering()).getChannelData(0),nonzero=[];
    for (let i=0;i<pcm.length && nonzero.length<6;i++) if (Math.abs(pcm[i])>1e-7) nonzero.push({frame:i-4410,value:pcm[i]});
    results.push({frames,nonzero});
  }
  return results;
}
async function delayControls() {
  const results=[],rate=44100;
  for (const kind of ['baseline','feedback-zero','time-zero','gain-zero','long','feedback-omitted']) {
    const context=new OfflineAudioContext(2,Math.round((kind==='long'?1.3:.6)*rate),rate);
    setAudioContext(context); setSuperdoughAudioController(null); resetGlobalEffects();
    await initAudio({maxPolyphony:128,multiChannelOrbits:false});
    const buffer=context.createBuffer(1,1,rate); buffer.getChannelData(0)[0]=1;
    const source=time=>{const node=context.createBufferSource();node.buffer=buffer;node.start(time);return node;};
    const initial={source,gain:.5,postgain:1,dry:0,delay:1,delaytime:kind==='long'?2:.05,release:0};
    if (kind!=='feedback-omitted') initial.delayfeedback=.5;
    await superdough(initial,.1,.001,.5,0);
    if (!['baseline','long','feedback-omitted'].includes(kind))
      await superdough({source,gain:kind==='gain-zero'?0:.5,postgain:1,dry:0,delay:1,
        delaytime:kind==='time-zero'?0:.075,delayfeedback:kind==='feedback-zero'?0:.25,release:0},.225,.001,.5,0);
    const pcm=(await context.startRendering()).getChannelData(0);
    results.push({kind,pcm:Array.from(pcm)});
  }
  const baseline=results[0].pcm;
  return results.map(({kind,pcm})=>{
    let difference=0,energy=0,first=-1;
    for (let i=0;i<pcm.length;i++) {
      if (kind!=='long') difference=Math.max(difference,Math.abs(pcm[i]-baseline[i]));
      energy+=pcm[i]**2; if (first<0 && Math.abs(pcm[i])>1e-7) first=i;
    }
    return {kind,difference,energy,first};
  });
}
const browser=await BrowserClient.create();
const artifacts=path.resolve(process.argv[2]??'/tmp/tompouce-effects-reference-qa');
try {
  await browser.page('https://strudel.cc/');
  await browser.until('typeof superdough==="function" && !!getSound("piano")',20000);
  const silent=await browser.evaluate('('+silentControls.toString()+')()');
  const methods=await browser.evaluate('('+effectMethods.toString()+')()');
  assert.deepEqual(methods.reverseCopy,[[4,3,2,1],[0,8,7,6]],
    'deployed reverseBuffer channel copy offsets changed');
  assert.deepEqual(methods.irSyntax,[{s:'sine',ir:'roomfixture:0'}]);
  assert.deepEqual(methods.irIndexSyntax,[{s:'sine',ir:'roomfixture',i:0}]);
  assert.equal(methods.irColonRejected,true,
    'the web treats colon IR syntax as a literal sound name, not a sample slot');
  const roomReplacementResult=await browser.evaluate('('+roomReplacement.toString()+')()');
  assert(roomReplacementResult[0].before>0 && roomReplacementResult[0].after>0,
    'unchanged custom IR must retain its sounding tail');
  assert.equal(roomReplacementResult[1].before,roomReplacementResult[0].before);
  assert.equal(roomReplacementResult[1].after,0,
    'replacing the shared IR clears its old tail immediately');
  const handoff=await browser.evaluate('('+generatedRoomHandoff.toString()+')()');
  assert.equal(handoff.retainedWhilePending,true,
    'generated replacement must retain the old convolver buffer until ready');
  assert.equal(handoff.replacedWhenReady,true,'generated replacement must eventually activate');
  assert.equal(handoff.replacedInterveningCustom,true,
    'an older generated completion still replaces an intervening custom IR');
  assert.deepEqual(handoff.failure,{threw:true,retryThrew:false,retained:true,duration:0},
    'failed generation retains the buffer and updated equality guards');
  assert.equal(handoff.duration,.06);
  assert.deepEqual(methods.roomSourceDefault,{calls:[Array(7).fill(null),[]],
    duration:2,fade:.1,lp:15000,dim:1000},
    'public Superdough sources leave omitted room parameters undefined');
  assert.deepEqual(methods.roomBeforeFutureOnset,
    {now:0,duration:.3,fade:.02,lp:9000,dim:500},
    'future silent sources prepare shared room settings immediately');
  assert.deepEqual(methods.roomSourceInherited,
    {duration:.3,fade:.02,lp:9000,dim:500},
    'public sources retain omitted shared room settings');
  assert.deepEqual(methods.roomPartialChange,
    {duration:.4,fade:.1,lp:15000,dim:1000},
    'rebuilding a room restores defaults for omitted arguments');
  assert.deepEqual(methods.customRoomInherited,
    {duration:.3,fade:.02,lp:9000,dim:500,speed:2,begin:.25,sameBuffer:true},
    'unchanged custom IR retains omitted controls without replacing its buffer');
  assert.deepEqual(methods.customRoomChanged,
    {duration:2,fade:.1,lp:15000,dim:1000,speed:null,begin:null},
    'changing IR identity resets omitted room controls');
  assert.deepEqual(methods.customExplicitDefault,
    {replaced:true,retainedAfterOmission:true,duration:2,fade:.1,speed:1},
    'explicit default speed rebuilds an IR whose prior speed was omitted');
  const delay=await browser.evaluate('('+delayControls.toString()+')()');
  const automation=await browser.evaluate('('+delayAutomation.toString()+')()');
  const short=await browser.evaluate('('+shortDelays.toString()+')()');
  const shortFrames=[[0,1,128,129,130,256],[0,1,128,129,130,256],
    [32,192,352,512,672,832],[128,384,640,896,1152,1408],
    [128,129,384,385,386,640],[2205,2206,4538,4539,4540,6871]];
  const shortValues=[[.375,.125,.140625,.09375,.015625,.052734375],
    [.25,.25,.0625,.125,.0625,.015625],[.5,.25,.125,.0625,.03125,.015625],
    [.5,.25,.125,.0625,.03125,.015625],[.25,.25,.0625,.125,.0625,.015625],
    [.375,.125,.140625,.09375,.015625,.052734375]];
  short.forEach((test,index)=>assert.deepEqual(test.nonzero,
    shortFrames[index].map((frame,i)=>({frame,value:shortValues[index][i]})),
    'web fractional delay or 128-frame feedback cycle changed'));
  assert.deepEqual(automation,[
    {returning:false,difference:0,time:.5,feedback:.75},
    {returning:true,difference:0,time:.5,feedback:.75},
  ],'future return values changed the shared delay despite the AudioParam equality guard');
  const duck=await browser.evaluate('('+duckControls.toString()+')()');
  const clip=await browser.evaluate('('+clipControls.toString()+')()');
  assert.equal(clip[0].energy,clip[1].energy,'web clip-zero no longer falls back to full duration');
  const orbitCreation=await browser.evaluate('('+duckOrbitCreation.toString()+')()');
  assert.deepEqual(orbitCreation,{created:[1,9,7],ducked:[9,7]},
    'source orbit creation precedes duck dispatch, including rests and missing sources');
  const targetCoercion=await browser.evaluate('('+duckTargetCoercion.toString()+')()');
  const numericStrings=await browser.evaluate('('+numericStringCoercion.toString()+')()');
  const nestedDuck=await browser.evaluate('('+nestedDuckControls.toString()+')()');
  const outputRouting=await browser.evaluate('('+duckOutputRouting.toString()+')()');
  assert.deepEqual(outputRouting,[
    {index:0,local:1,external:0,ducked:1},
    {index:1,local:1,external:0,ducked:1},
    {index:2,local:0,external:1,ducked:0},
    {index:3,local:0,external:1,ducked:0},
    {index:4,local:0,external:1,ducked:0},
  ],'dominant external outputs must bypass local orbit preparation and ducking');
  assert.deepEqual(nestedDuck.map(test=>test.records.map(record=>record.rejected)),
    [[0,0],[2,2],[2,2],[0,0],[0,0],[0,0],[2,2],[2,2],[2,2],
      [1,1],[1,1],[1,1],[1,1],[1,1],[1,1],[2,2],[2,0]],
    'nested/empty/sparse duck controls changed ramp rejection stages');
  assert.deepEqual(nestedDuck.map(test=>test.records.map(record=>record.attack)),
    [['0.1','0.1'],['NaN','NaN'],['NaN','NaN'],['0.002','0.002'],
      ['0.25','0.25'],['0.002','0.25'],['0.002','0.002'],['0.002','0.002'],
      ['0.002','0.002'],['NaN','NaN'],['0.25','0.25'],['0.25','0.25'],
      ['NaN','NaN'],['NaN','NaN'],['NaN','NaN'],['NaN','NaN'],['NaN','0.25']],
    'controller defaults must precede orbit defaults for missing array values');
  assert.deepEqual(numericStrings,['0.25','0.25','0','NaN','2','8','16',
    'NaN','NaN','NaN','NaN','Infinity','-Infinity','NaN','0','0.25']);
  const invalidDuck=await browser.evaluate('('+invalidDuckControls.toString()+')()');
  invalidDuck.forEach((test,index)=>{
    if(test.error) {
      assert.equal(test.errorAt,test.callbackAt,'duck rejection must be reported at callback execution');
      assert.equal(test.rejectedRamp,[1,1,1,1,2,2,0,0,0,2,2,1,2,0,
        1,0,1,2,2,0,1,1,1,1,0][index],
        'duck rejection must identify the first failing ramp');
    }
    if(index>=14) {
      const expected=[.8,1,.8,.5,.5,1,.8,.8,.8,.8,1][index-14];
      assert.equal(!!test.error,![15,19,24].includes(index),
        'non-finite duck controls changed ramp acceptance');
      assert(Math.abs(test.gain-expected)<2e-6,'non-finite duck controls changed held gain');
      return;
    }
    if(index>=9) {
      assert.equal(!!test.error,index!==13,'string onset rejection stage differs');
      assert(Math.abs(test.gain-[.5,.5,.8,.5,1][index-9])<2e-6,
        'string onset concatenation differs');
      return;
    }
    if(index>=6) {
      assert(!test.error,'boolean/null onset must use numeric addition');
      const expected=test.controls.onset===true?
        .8*(.5/.8)**((.45-test.callbackAt)/(1.3-test.callbackAt)):1;
      assert(Math.abs(test.gain-expected)<2e-6,'numeric onset coercion differs');
      return;
    }
    assert(test.error,'invalid duck controls must report a WebAudio rejection');
    assert(Math.abs(test.gain-(index<4?.8:.5))<2e-6,
      'invalid duck must preserve the gain after its last accepted automation');
  });
  assert.deepEqual(targetCoercion.targets.map(test=>test.called),
    [[1],[1],[],[],[1,2],[1,2],[],[1],[2,1]],
    'duck targets use JavaScript property keys after one array flatten');
  assert.deepEqual(targetCoercion.ordered,[
    {id:2,args:[.3,.02,.1,.25]},{id:1,args:[.3,.04,.2,.5]},
    {id:2,args:[.3,.06,.3,1]}], 'duplicate targets retain order and per-entry values');
  const mapping=await browser.evaluate('('+duckMapping.toString()+')()');
  assert.deepEqual(mapping.slice(0,4),[
    {id:1,args:[.3,.02,.1,.25]},{id:2,args:[.3,.04,.2,1]},
    {id:1,args:[.4,.01,.002,0]},{id:2,args:[.4,.01,.002,0]},
  ],'web duck array mapping/defaults changed');
  assert.deepEqual(mapping.slice(4),[
    {id:1,args:[.5,0,.1,.25]},{id:1,args:[.6,0,.1,.5]},{id:1,args:[.7,0,.1,1]},
    {id:1,args:[.8,0,.1,1]},
  ],'silent, rest or missing-source events no longer dispatch duck controls');
  for (const test of duck) {
    assert.equal(test.calls.length,4,'duck must cancel, anchor and schedule two ramps');
    const target=Math.max(.01,1-Math.sqrt(test.depth));
    assert.equal(test.calls[2].args[0],target);
    assert.equal(test.calls[2].args[1],.3);
    assert.equal(test.calls[3].args[1],.4);
    for (const point of test.points) {
      const time=Math.round(point.time*44100)/44100;
      const expected=time<=test.callbackAt?1:time<.3?
        target**((time-test.callbackAt)/(.3-test.callbackAt)):
        time<.4?target**(1-(time-.3)/.1):1;
      assert(Math.abs(point.gain-expected)<2e-6,'web duck envelope differs at '+point.time);
    }
  }
  assert(silent.every(test=>test.runs[0].after>1e-5),'reference sources must remain audible in the comparison window');
  assert(Math.abs(silent[0].runs[0].after-silent[0].runs[1].after)<1e-8,'synth cut behavior changed');
  assert.equal(silent[1].runs[1].after,0,'silent sampled event no longer cuts its group');
  assert.equal(delay[1].difference,0,'zero feedback changed an existing delay tail');
  assert.equal(delay[2].difference,0,'zero time changed an existing delay tail');
  assert(delay[3].difference>1e-4,'silent events no longer update shared delay');
  assert.equal(delay[4].first,Math.round(1.1*44100),'web shared delay maximum changed');
  assert.equal(delay[5].difference,0,'web default delay feedback changed');
  assert.deepEqual(methods.roomAfterOmitted,{duration:.1,fade:.05,lp:18000,dim:8000},
    'omitted room controls no longer retain the shared convolver parameters');
  assert(Math.abs(methods.delayWhileScheduled.feedback-.5)<1e-7,
    'future delay automation unexpectedly changed the current AudioParam value');
  assert(Math.abs(methods.delayAfterReturn.feedback-.75)<1e-7,
    'web delay automation equality behavior changed');
  await fs.mkdir(artifacts,{recursive:true});
  await fs.writeFile(path.join(artifacts,'report.json'),JSON.stringify({url:'https://strudel.cc/',capturedAt:new Date().toISOString(),silent,methods,roomReplacement:roomReplacementResult,handoff,delay,automation,short,duck,orbitCreation,targetCoercion,numericStrings,nestedDuck,outputRouting,invalidDuck,mapping,clip},null,2)+'\n');
  console.log(JSON.stringify({silent,delay,duck}));
} finally {await browser.close();}
