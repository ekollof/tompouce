/* Headless smoke test for the WebGL Hydra backend and the /hydra page.
   Serves a disposable page and a stub analysis endpoint, renders GLSL
   generator/effect chains in Chromium, reads pixels back, and then drives the
   real page through one poll and a custom graph. No engine or audio device is
   touched. */
import http from 'node:http';
import fs from 'node:fs/promises';
import path from 'node:path';
import {BrowserClient} from './browser-client.mjs';

const barePage = `<!doctype html>
<html><head><meta charset="utf-8"><style>html,body{margin:0;background:#090b12}</style></head>
<body><canvas id="stage" width="320" height="180"></canvas>
<script src="/hydra.js"></script></body></html>`;
const realPage = (await fs.readFile(path.resolve('web/hydra.html'), 'utf8'))
  .replaceAll('COMPANION_TOKEN', 'test-token');
const scope = Array.from({length:192}, (_, i) => Math.sin(i * .19) * .7);
const scopeReply = JSON.stringify({result:{content:[{type:'text',
  text:JSON.stringify({scope,bpm:120,playing:1,generation:1})}]}});

const server = http.createServer(async (request, response) => {
  try {
    if (request.url === '/api/tool') {
      request.resume();
      response.writeHead(200, {'Content-Type': 'application/json', 'Cache-Control': 'no-store'});
      response.end(scopeReply);
      return;
    }
    if (request.url === '/' || request.url === '/index.html') {
      response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store'});
      response.end(barePage);
      return;
    }
    if (request.url === '/page') {
      response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store'});
      response.end(realPage);
      return;
    }
    if (request.url === '/hydra.js') {
      const script = await fs.readFile(path.resolve('web/hydra.js'));
      response.writeHead(200, {'Content-Type': 'text/javascript; charset=utf-8', 'Cache-Control': 'no-store'});
      response.end(script);
      return;
    }
    response.writeHead(404); response.end();
  } catch (error) {
    response.writeHead(500); response.end();
  }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/`;

let browser = null;
let failures = 0;
try {
  try {
    browser = await BrowserClient.create();
  } catch (error) {
    console.log('SKIP hydra WebGL: Chromium unavailable');
    process.exit(0);
  }
  await browser.page(url);
  await browser.until('window.Hydra && Hydra.ready === true', 10000);
  const results = await browser.evaluate(`(() => {
    const gl = __hydraGL;
    const canvas = document.getElementById('stage');
    const scope = ${JSON.stringify(scope)};
    const read = () => {
      const data = new Uint8Array(canvas.width*canvas.height*4);
      gl.readPixels(0,0,canvas.width,canvas.height,gl.RGBA,gl.UNSIGNED_BYTE,data);
      return data;
    };
    const stats = data => {
      let min = 999, max = -1, sum = 0;
      for (let i=0;i<data.length;i+=4) {
        const value = data[i]+data[i+1]+data[i+2];
        if (value<min) min=value;
        if (value>max) max=value;
        sum+=value;
      }
      return {min,max,mean:sum/(data.length/4)};
    };
    const run = (node, time) => {
      node.out();
      node._render(scope, time);
      return {error: gl.getError(), ...stats(read())};
    };
    return {
      ready: Hydra.ready,
      solid: run(solid(1,.2,.1), 1000),
      osc: run(osc(6).color(1,.5,.2), 1000),
      chain: run(osc(6).rotate(.3).scale(1.2).repeat(2).kaleid(4)
        .pixelate(40).posterize(4).colorama(.2), 1200),
      combine: run(osc(5).modulate(noise(3),.3).blend(shape(4,.4),.5), 1400),
      binary: run(noise(4).add(voronoi(6),.3).mult(gradient(.1),1.2), 1600),
      scope: run(src(), 1800),
      coords: run(osc(6).rotate(.3).scale(1.2,1,1,.5,.5).pixelate(30)
        .repeat(2,2,.1,.1).repeatX(2,.1).repeatY(2,.1).kaleid(4)
        .scroll(.1,.2,.01,.02).scrollX(.1,.01).scrollY(.1,.01), 1900),
      mods1: run(osc(6).modulateRepeat(noise(3),3,3,.1,.1)
        .modulateRepeatX(noise(3),2,.1).modulateRepeatY(noise(3),2,.1)
        .modulateKaleid(noise(3),4).modulateScrollX(noise(3),.1,.01), 2000),
      mods2: run(osc(6).modulateScrollY(noise(3),.1,.01)
        .modulateScale(noise(3),.2,.5).modulatePixelate(noise(3),2,3)
        .modulateRotate(noise(3),.1,0).modulateHue(noise(3),.2), 2050),
      combine2: run(osc(6).add(noise(3),1).sub(noise(3),.5).mult(noise(3),1)
        .diff(noise(3)).blend(shape(4,.3),.5).layer(solid(.1,.2,.3,1))
        .mask(noise(3).add(solid(.2,.2,.2,1),.5)), 2100),
      colors1: run(osc(6).shift(.1,.2,.3,.1).posterize(4,.6).invert(1)
        .contrast(1.2).brightness(.1), 2200),
      shift1: run(osc(6).shift(.1,.2,.3,.1), 2210),
      posterize1: run(osc(6).posterize(4,.6), 2220),
      invert1: run(osc(6).invert(1), 2230),
      contrast1: run(osc(6).contrast(1.2), 2240),
      brightness1: run(osc(6).brightness(.1), 2250),
      luma1: run(osc(6).luma(.5,.2), 2220),
      thresh1: run(osc(6).thresh(.5,.2), 2240),
      colors2: run(osc(6).color(1,.8,.6,1).saturate(1.5).hue(.2).colorama(.01)
        .r(1,.1).g(.9,.1).b(.8,.1).a(1,0).sum(.1), 2250),
      feedback: (() => {
        const node=osc(6).add(prev().scale(1.01),.2);
        node.out(); node._render(scope,2300);
        const first=stats(read());
        const e1=gl.getError();
        node._render(scope,2400);
        const second=stats(read());
        return {error:gl.getError(),e1,min:second.min,max:second.max,
          mean:second.mean,delta:Math.abs(second.mean-first.mean)};
      })(),
      resolution: (() => {
        const width=canvas.width, height=canvas.height;
        setResolution(160,90);
        const rendered=run(solid(.5,.2,.1),2500);
        const dims=[canvas.width,canvas.height];
        setResolution(width,height);
        return {error:rendered.error,...dims};
      })(),
      hush: (() => { hush(); return {cleared:__hydraOutput===null}; })(),
      finalError: gl.getError()
    };
  })()`);
  const check = (name, value, nonUniform) => {
    const ok = value.error === 0 && Number.isFinite(value.mean) && value.mean > 0 &&
      (!nonUniform || value.max > value.min);
    if (!ok) { failures++; console.error(name, value); }
  };
  check('solid', results.solid, false);
  if (Math.abs(results.solid.mean - 332) > 3) {
    failures++; console.error('solid mean', results.solid.mean);
  }
  for (const name of ['osc','chain','combine','binary','scope','coords','mods1','mods2','combine2','colors1','shift1','posterize1','invert1','contrast1','brightness1','luma1','thresh1','colors2'])
    check(name, results[name], true);
  if (results.feedback.error !== 0 || results.feedback.e1 !== 0 ||
      !Number.isFinite(results.feedback.mean) || results.feedback.mean <= 0 ||
      results.feedback.max <= results.feedback.min || results.feedback.delta < 0.1) {
    failures++; console.error('feedback', results.feedback);
  }
  if (results.finalError !== 0) { failures++; console.error('final GL error', results.finalError); }
  if (results.ready !== true) { failures++; console.error('Hydra not ready'); }
  if (results.resolution.error !== 0 || results.resolution[0] !== 160 ||
      results.resolution[1] !== 90) {
    failures++; console.error('resolution', results.resolution);
  }
  if (!results.hush.cleared) { failures++; console.error('hush', results.hush); }

  /* Camera/screen/image sources and their texture upload path. The media
     functions are stubbed with owned canvas streams; no device is touched. */
  const sourceResults = await browser.evaluate(`(async () => {
    const gl=__hydraGL, canvas=document.getElementById('stage');
    const read=()=>{const data=new Uint8Array(canvas.width*canvas.height*4);
      gl.readPixels(0,0,canvas.width,canvas.height,gl.RGBA,gl.UNSIGNED_BYTE,data);return data;};
    const stats=data=>{let min=999,max=-1,sum=0;
      for(let i=0;i<data.length;i+=4){const value=data[i]+data[i+1]+data[i+2];
        if(value<min)min=value;if(value>max)max=value;sum+=value;}
      return {min,max,mean:sum/(data.length/4)};};
    const run=(node,time)=>{node.out();node._render([],time);
      return {error:gl.getError(),...stats(read())};};
    const wait=async source=>{for(let i=0;i<120;i++){source.tick();
      if(source.ready)return true;await new Promise(r=>setTimeout(r,25));}return false;};
    const painted=(a,b)=>{const c=document.createElement('canvas');
      c.width=16;c.height=16;const x=c.getContext('2d');
      const g=x.createLinearGradient(0,0,16,0);g.addColorStop(0,a);g.addColorStop(1,b);
      x.fillStyle=g;x.fillRect(0,0,16,16);return c;};
    await s2.initImage(painted('#f00','#0f0').toDataURL());
    const imageReady=await wait(s2);
    const image=run(src(s2),3000);
    let camera=null,screen=null,stream=null,stubError=null,imageError=s2.error;
    const own=Object.getOwnPropertyDescriptor(navigator,'mediaDevices');
    const capture=()=>{const c=painted('#0f0','#00f');
      return c.captureStream?c.captureStream(10):null;};
    try {
      Object.defineProperty(navigator,'mediaDevices',{configurable:true,
        value:{getUserMedia:async()=>capture(),getDisplayMedia:async()=>capture()}});
    } catch(error) { stubError=String(error&&error.message||error); }
    if(!stubError){
      await s0.initCam();
      if(await wait(s0)) camera=run(src(s0),3100);
      else camera={error:-1,detail:s0.error,min:0,max:0,mean:0};
      await s1.initScreen();
      if(await wait(s1)) screen=run(src(s1),3200);
      else screen={error:-1,detail:s1.error,min:0,max:0,mean:0};
    }
    if(own) Object.defineProperty(navigator,'mediaDevices',own);
    else delete navigator.mediaDevices;
    await s3.initStream(capture());
    if(await wait(s3)) stream=run(src(s3),3300);
    else stream={error:-1,detail:s3.error,min:0,max:0,mean:0};
    s0.clear(); s1.clear(); s2.clear(); s3.clear();
    return {imageReady,image,camera,screen,stream,stubError,imageError};
  })()`);
  const checkSource=(name,value)=>{
    if (!value || value.error!==0 || !Number.isFinite(value.mean) ||
        value.mean<=0 || value.max<=value.min) {
      failures++; console.error(name,value);
    }
  };
  if (!sourceResults.imageReady) { failures++; console.error('image source not ready', sourceResults); }
  checkSource('image source', sourceResults.image);
  if (sourceResults.stubError) {
    failures++; console.error('media stub', sourceResults.stubError);
  } else {
    checkSource('camera source', sourceResults.camera);
    checkSource('screen source', sourceResults.screen);
    checkSource('stream source', sourceResults.stream);
    console.log('hydra sources:', JSON.stringify({
      image: sourceResults.imageReady,
      camera: sourceResults.camera && sourceResults.camera.mean,
      screen: sourceResults.screen && sourceResults.screen.mean,
      stream: sourceResults.stream && sourceResults.stream.mean
    }));
  }

  /* The real page must switch from the 2D scope fallback to the WebGL stage
     and render a user graph after its analysis poll. */
  await browser.page(url + 'page');
  await browser.until('window.Hydra && Hydra.ready === true', 10000);
  await browser.evaluate('osc(6).color(1,.5,.2).rotate(.2).out(); true');
  await browser.until('document.getElementById("hud").textContent.indexOf("WebGL graph") >= 0', 5000);
  const pageResult = await browser.evaluate(`(() => {
    const gl = __hydraGL, canvas = document.getElementById('stage');
    const data = new Uint8Array(canvas.width*canvas.height*4);
    gl.readPixels(0,0,canvas.width,canvas.height,gl.RGBA,gl.UNSIGNED_BYTE,data);
    let min=999,max=-1;
    for (let i=0;i<data.length;i+=4) {
      const value=data[i]+data[i+1]+data[i+2];
      if (value<min) min=value;
      if (value>max) max=value;
    }
    return {min,max,display:getComputedStyle(canvas).display,
      hud:document.getElementById('hud').textContent,error:gl.getError()};
  })()`);
  if (pageResult.display === 'none' || pageResult.max <= pageResult.min ||
      pageResult.error !== 0) {
    failures++;
    console.error('page', pageResult);
  }

  if (failures) {
    console.error(`hydra WebGL smoke failed: ${failures} case(s)`);
    process.exitCode = 1;
  } else {
    console.log('hydra WebGL generators, full effects, feedback, camera/screen/image sources and page scope source ok');
  }
} catch (error) {
  if (/WebGL unavailable/.test(String(error))) {
    console.log('SKIP hydra WebGL: WebGL unavailable');
  } else {
    console.error(String(error && error.stack ? error.stack : error));
    process.exitCode = 1;
  }
} finally {
  if (browser) await browser.close();
  await new Promise(resolve => server.close(resolve));
}
