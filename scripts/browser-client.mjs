/* Disposable Chromium only; never attaches to the user's browser. */
import {spawn} from 'node:child_process';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {StringDecoder} from 'node:string_decoder';

export class BrowserClient {
  static async create() {
    const client=new BrowserClient();
    client.profile=await fs.mkdtemp(path.join(os.tmpdir(),'tompouce-browser-'));
    client.pending=new Map(); client.next=0; client.session=null; client.exceptions=[];
    client.process=spawn(process.env.CHROMIUM??'chromium',[
      '--headless','--no-first-run','--disable-gpu','--remote-debugging-pipe',
      `--user-data-dir=${client.profile}`,'about:blank',
    ],{stdio:['ignore','ignore','ignore','pipe','pipe']});
    let buffer=''; const decoder=new StringDecoder('utf8');
    client.process.stdio[4].on('data',bytes=>{
      buffer+=decoder.write(bytes);
      for (;;) {
        const end=buffer.indexOf('\0'); if (end<0) break;
        const message=JSON.parse(buffer.slice(0,end)); buffer=buffer.slice(end+1);
        const pending=client.pending.get(message.id);
        if (pending) {
          client.pending.delete(message.id); clearTimeout(pending.timer);
          message.error?pending.reject(new Error(JSON.stringify(message.error))):pending.resolve(message.result);
        } else if (message.method==='Runtime.exceptionThrown') client.exceptions.push(message.params);
      }
    });
    const fail=error=>{ client.error=error; for (const p of client.pending.values()) {clearTimeout(p.timer);p.reject(error);} client.pending.clear(); };
    client.process.on('error',fail); client.process.on('exit',()=>fail(new Error('Chromium exited')));
    return client;
  }
  call(method,params={},session=this.session) {
    return new Promise((resolve,reject)=>{
      if (this.error) {reject(this.error);return;}
      const id=++this.next;
      const timer=setTimeout(()=>{this.pending.delete(id);reject(new Error(`Timed out: ${method}`));},30000);
      this.pending.set(id,{resolve,reject,timer});
      this.process.stdio[3].write(JSON.stringify({id,method,params,...(session?{sessionId:session}:{})})+'\0');
    });
  }
  async page(url,width=1440,height=1100) {
    const {targetId}=await this.call('Target.createTarget',{url:'about:blank'},null);
    this.session=(await this.call('Target.attachToTarget',{targetId,flatten:true},null)).sessionId;
    await this.call('Runtime.enable'); await this.call('Page.enable');
    await this.call('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:false});
    await this.call('Page.navigate',{url}); return targetId;
  }
  async evaluate(expression) {
    const result=await this.call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  }
  async until(expression,timeout=10000) {
    const end=Date.now()+timeout;
    while (Date.now()<end) {if (await this.evaluate(`Boolean(${expression})`)) return; await new Promise(r=>setTimeout(r,50));}
    throw new Error(`Browser condition timed out: ${expression}`);
  }
  async screenshot(file,full=false) {
    let clip;
    if (full) {
      const metrics=await this.call('Page.getLayoutMetrics'), content=metrics.cssContentSize??metrics.contentSize;
      clip={x:0,y:0,width:Math.min(2048,content.width),height:Math.min(4096,content.height),scale:1};
    }
    const {data}=await this.call('Page.captureScreenshot',{format:'png',captureBeyondViewport:full,...(clip?{clip}:{})});
    await fs.writeFile(file,Buffer.from(data,'base64'));
  }
  async close() {
    this.process.kill();
    await new Promise(resolve=>this.process.exitCode!==null || !this.process.pid?resolve():this.process.once('exit',resolve));
    await fs.rm(this.profile,{recursive:true,force:true,maxRetries:10,retryDelay:100});
  }
}
