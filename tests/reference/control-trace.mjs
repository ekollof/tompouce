// Shared scenario for the captured scheduler, live strudel.cc and native engine.
export async function controlTrace(Ctor,lib) {
  let now=0, timer, cps=.5;
  const events=[], states=[], errors=[];
  const scheduler=new Ctor({getTime:()=>now,setInterval:fn=>(timer=fn,1),clearInterval:()=>{timer=null;},
    onError:e=>errors.push(String(e)),
    onSchedule:(hap,time,cps)=>events.push({time,duration:Number(hap.duration)/cps,note:hap.value.note,cps})});
  const notes=(...values)=>lib.note(lib.fastcat(...values.map(lib.pure)));
  const control=lib.ref(()=>cps);
  await scheduler.setPattern(lib.stack(notes(60,64).fast(10),notes(72,76).fast(10).cps(control)));
  const state=()=>states.push({time:now,cycle:Number(scheduler.lastEnd),cps:scheduler.cps,started:scheduler.started,phase:scheduler.clock.getPhase()});
  await scheduler.start(); state();
  cps=.75; now=.1; timer(); state();
  scheduler.setCps(2); now=.2; timer(); state();
  cps=.25; for (const time of [.3,.4]) {now=time;timer();state();}
  cps=0; for (const time of [.5,.6]) {now=time;timer();state();}
  cps=1; scheduler.setCps(1); now=.7; timer(); state();
  now=.75; scheduler.stop(); state();
  now=2; await scheduler.start(); state();
  now=2.1; timer(); state(); scheduler.stop();
  return {events,states,errors};
}

export const normalize=value=>JSON.parse(JSON.stringify(value,(_key,v)=>typeof v==='number'?
  Number.isFinite(v)?Math.round(v*1e9)/1e9:String(v):v));
