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
  for (const name of ['osc','chain','combine','binary','scope'])
    check(name, results[name], true);
  if (results.finalError !== 0) { failures++; console.error('final GL error', results.finalError); }
  if (results.ready !== true) { failures++; console.error('Hydra not ready'); }

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
    console.log('hydra WebGL generators, effects, ping-pong chains and page scope source ok');
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
