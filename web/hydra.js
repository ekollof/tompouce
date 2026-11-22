/* Bounded Hydra-compatible visual backend for the /hydra page.
   Generators and effects compile to GLSL; effect chains render through a
   bounded pool of ping-pong framebuffers. No network or engine control is
   involved: the page supplies a scope array and a timestamp. */
(function (global) {
  'use strict';
  const canvas = global.__HYDRA_CANVAS ||
    (global.document && document.getElementById('stage'));
  if (!canvas) return;
  let gl = null;
  try {
    gl = canvas.getContext('webgl2', {alpha:false, antialias:false, preserveDrawingBuffer:true}) ||
      canvas.getContext('webgl', {alpha:false, antialias:false, preserveDrawingBuffer:true});
  } catch (error) { gl = null; }
  const api = {ready:false, error:null};
  if (!gl) {
    api.error = 'WebGL unavailable';
    global.Hydra = api;
    return;
  }
  global.__hydraGL = gl;

  const WIDTH = Math.max(1, Math.min(1920, canvas.width || 960));
  const HEIGHT = Math.max(1, Math.min(1080, canvas.height || 540));
  const SCOPE_SIZE = 256;
  const POOL_SIZE = 12;
  const MAX_DEPTH = 32;

  function number(value, fallback) {
    const n = Number(value);
    return Number.isFinite(n) ? n : fallback;
  }
  function clamp(value, low, high) { return Math.min(high, Math.max(low, value)); }

  const VERTEX = `attribute vec2 a_pos;
varying vec2 v_uv;
void main(){ v_uv=a_pos*.5+.5; gl_Position=vec4(a_pos,0.,1.); }`;
  const HEADER = `precision highp float;
varying vec2 v_uv;
uniform float u_time;
uniform vec2 u_resolution;
`;
  const HSV = `vec3 rgb2hsv(vec3 c){
  vec4 K=vec4(0.,-.3333333,.6666667,1.);
  vec4 p=mix(vec4(c.bg,K.wz),vec4(c.gb,K.xy),step(c.b,c.g));
  vec4 q=mix(vec4(p.xyw,c.r),vec4(c.r,p.yzx),step(p.x,c.r));
  float d=q.x-min(q.w,q.y);
  float e=1.0e-10;
  return vec3(abs(q.z+(q.w-q.y)/(6.*d+e)),d/(q.x+e),q.x);
}
vec3 hsv2rgb(vec3 c){
  vec4 K=vec4(1.,.6666667,.3333333,3.);
  vec3 p=abs(fract(c.xxx+K.xyz)*6.-K.www);
  return c.z*mix(K.xxx,clamp(p-K.xxx,0.,1.),c.y);
}`;

  const GENERATORS = {
    osc: `uniform float p0,p1,p2;
void main(){ float v=sin((v_uv.x*p0+u_time*p1+p2)*3.141592653589793);
  gl_FragColor=vec4(vec3(v),1.); }`,
    shape: `uniform float p0,p1,p2;
void main(){ vec2 p=v_uv*2.-1.; float a=atan(p.y,p.x);
  float n=max(3.,floor(p0)); float seg=6.2831853/n;
  float d=cos(floor(.5+a/seg)*seg-a)*length(p);
  float r=max(.001,p1); float sm=max(.0005,p2);
  gl_FragColor=vec4(vec3(1.-smoothstep(r-sm,r+sm,d)),1.); }`,
    solid: `uniform float p0,p1,p2;
void main(){ gl_FragColor=vec4(p0,p1,p2,1.); }`,
    noise: `uniform float p0,p1;
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453123); }
float vnoise(vec2 p){ vec2 i=floor(p),f=fract(p); f=f*f*(3.-2.*f);
  return mix(mix(hash(i),hash(i+vec2(1.,0.)),f.x),
             mix(hash(i+vec2(0.,1.)),hash(i+vec2(1.,1.)),f.x),f.y); }
void main(){ float n=vnoise(v_uv*max(.01,p0)+u_time*p1);
  gl_FragColor=vec4(vec3(n),1.); }`,
    voronoi: `uniform float p0,p1;
vec2 hash2(vec2 p){ p=vec2(dot(p,vec2(127.1,311.7)),dot(p,vec2(269.5,183.3)));
  return fract(sin(p)*43758.5453); }
float cells(vec2 x){ vec2 n=floor(x),f=fract(x); float md=8.;
  for(int j=-1;j<=1;j++) for(int i=-1;i<=1;i++){
    vec2 g=vec2(float(i),float(j)); vec2 o=hash2(n+g); vec2 r=g+o-f;
    md=min(md,dot(r,r)); }
  return sqrt(md); }
void main(){ float d=cells(v_uv*max(.01,p0)+u_time*p1);
  gl_FragColor=vec4(vec3(clamp(1.-d,0.,1.)),1.); }`,
    gradient: `uniform float p0;
void main(){ gl_FragColor=vec4(vec3(fract(v_uv.y+u_time*p0)),1.); }`,
    src: `uniform sampler2D u_scope;
void main(){ float s=texture2D(u_scope,vec2(v_uv.x,.5)).r*2.-1.;
  float d=abs(v_uv.y-.5-s*.35);
  float line=1.-smoothstep(.004,.02,d);
  gl_FragColor=vec4(vec3(line*.9+.03),1.); }`
  };

  const UNARY = {
    rotate: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 p=v_uv-.5; float a=p0*6.2831853+u_time*p1;
  vec2 uv=vec2(cos(a)*p.x-sin(a)*p.y,sin(a)*p.x+cos(a)*p.y)+.5;
  gl_FragColor=texture2D(u_tex,uv); }`,
    scale: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 s=vec2(max(.001,abs(p0)),max(.001,abs(p1)));
  vec2 uv=(v_uv-.5)/s+.5; gl_FragColor=texture2D(u_tex,uv); }`,
    repeat: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 n=vec2(max(1.,floor(p0)),max(1.,floor(p1)));
  gl_FragColor=texture2D(u_tex,fract(v_uv*n)); }`,
    repeatX: `uniform sampler2D u_tex; uniform float p0;
void main(){ float n=max(1.,floor(p0));
  gl_FragColor=texture2D(u_tex,vec2(fract(v_uv.x*n),v_uv.y)); }`,
    repeatY: `uniform sampler2D u_tex; uniform float p0;
void main(){ float n=max(1.,floor(p0));
  gl_FragColor=texture2D(u_tex,vec2(v_uv.x,fract(v_uv.y*n))); }`,
    kaleid: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec2 p=v_uv-.5; float r=length(p); float a=atan(p.y,p.x);
  float n=max(1.,floor(p0)); float seg=6.2831853/n;
  a=mod(a,seg); a=abs(a-seg*.5);
  gl_FragColor=texture2D(u_tex,vec2(cos(a),sin(a))*r+.5); }`,
    pixelate: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 n=vec2(max(1.,floor(p0)),max(1.,floor(p1)));
  gl_FragColor=texture2D(u_tex,(floor(v_uv*n)+.5)/n); }`,
    posterize: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ float n=max(1.,floor(p0)); vec4 c=texture2D(u_tex,v_uv);
  c.rgb=pow(floor(c.rgb*n)/n,vec3(max(.001,p1))); gl_FragColor=c; }`,
    color: `uniform sampler2D u_tex; uniform float p0,p1,p2;
void main(){ vec4 c=texture2D(u_tex,v_uv); c.rgb*=vec3(p0,p1,p2); gl_FragColor=c; }`,
    colorama: `uniform sampler2D u_tex; uniform float p0;
${HSV}
void main(){ vec4 c=texture2D(u_tex,v_uv);
  vec3 h=rgb2hsv(clamp(c.rgb,0.,1.));
  h.x=fract(h.x+u_time*p0);
  gl_FragColor=vec4(hsv2rgb(h),c.a); }`,
    hue: `uniform sampler2D u_tex; uniform float p0;
${HSV}
void main(){ vec4 c=texture2D(u_tex,v_uv);
  vec3 h=rgb2hsv(clamp(c.rgb,0.,1.));
  h.x=fract(h.x+p0);
  gl_FragColor=vec4(hsv2rgb(h),c.a); }`,
    saturate: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  float l=dot(c.rgb,vec3(.299,.587,.114));
  gl_FragColor=vec4(mix(vec3(l),c.rgb,p0),c.a); }`,
    brightness: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv); c.rgb*=p0; gl_FragColor=c; }`,
    contrast: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  c.rgb=(c.rgb-.5)*p0+.5; gl_FragColor=c; }`,
    invert: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  c.rgb=mix(c.rgb,1.-c.rgb,clamp(p0,0.,1.)); gl_FragColor=c; }`,
    luma: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  float l=dot(c.rgb,vec3(.299,.587,.114));
  gl_FragColor=vec4(mix(c.rgb,vec3(l),clamp(p0,0.,1.)),c.a); }`
  };

  const BINARY = {
    modulate: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec2 uv=v_uv; uv.x+=(texture2D(u_tex2,v_uv).r-.5)*p0;
  gl_FragColor=texture2D(u_tex,uv); }`,
    blend: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=mix(a,b,clamp(p0,0.,1.)); }`,
    add: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=vec4(a.rgb+b.rgb*p0,a.a); }`,
    sub: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=vec4(a.rgb-b.rgb*p0,a.a); }`,
    mult: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=vec4(a.rgb*b.rgb*p0,a.a); }`,
    diff: `uniform sampler2D u_tex,u_tex2;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=vec4(abs(a.rgb-b.rgb),a.a); }`,
    layer: `uniform sampler2D u_tex,u_tex2;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=max(a,b); }`
  };

  function compile(type, source) {
    const shader=gl.createShader(type);
    gl.shaderSource(shader,source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader,gl.COMPILE_STATUS)) {
      const log=gl.getShaderInfoLog(shader)||'shader compile failed';
      gl.deleteShader(shader);
      throw new Error(log);
    }
    return shader;
  }
  function createProgram(key, fragment) {
    const program=gl.createProgram();
    const vertex=compile(gl.VERTEX_SHADER,VERTEX);
    const pixel=compile(gl.FRAGMENT_SHADER,HEADER+fragment);
    gl.attachShader(program,vertex); gl.attachShader(program,pixel);
    gl.linkProgram(program);
    gl.deleteShader(vertex); gl.deleteShader(pixel);
    if (!gl.getProgramParameter(program,gl.LINK_STATUS)) {
      const log=gl.getProgramInfoLog(program)||'program link failed';
      gl.deleteProgram(program);
      throw new Error(key+': '+log);
    }
    const uniforms=[];
    for (let i=0;i<4;i++) uniforms.push(gl.getUniformLocation(program,'p'+i));
    return {
      program,
      attrib: gl.getAttribLocation(program,'a_pos'),
      time: gl.getUniformLocation(program,'u_time'),
      resolution: gl.getUniformLocation(program,'u_resolution'),
      texture: gl.getUniformLocation(program,'u_tex'),
      texture2: gl.getUniformLocation(program,'u_tex2'),
      scope: gl.getUniformLocation(program,'u_scope'),
      uniforms
    };
  }
  const programs=new Map();
  function generatorProgram(kind) {
    const key='gen:'+kind;
    if (!programs.has(key)) programs.set(key,createProgram(key,GENERATORS[kind]));
    return programs.get(key);
  }
  function unaryProgram(kind) {
    const key='unary:'+kind;
    if (!programs.has(key)) programs.set(key,createProgram(key,UNARY[kind]));
    return programs.get(key);
  }
  function binaryProgram(kind) {
    const key='binary:'+kind;
    if (!programs.has(key)) programs.set(key,createProgram(key,BINARY[kind]));
    return programs.get(key);
  }

  function createTarget() {
    const texture=gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D,texture);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,WIDTH,HEIGHT,0,gl.RGBA,gl.UNSIGNED_BYTE,null);
    const framebuffer=gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER,framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,texture,0);
    const complete=gl.checkFramebufferStatus(gl.FRAMEBUFFER)===gl.FRAMEBUFFER_COMPLETE;
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);
    if (!complete) throw new Error('WebGL framebuffer incomplete');
    return {texture,framebuffer};
  }

  const pool=[];
  for (let i=0;i<POOL_SIZE;i++) pool.push(createTarget());
  function acquire() {
    if (!pool.length) throw new Error('Hydra graph too deep');
    return pool.pop();
  }
  function release(target) { pool.push(target); }

  const quad=gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER,quad);
  gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),gl.STATIC_DRAW);

  const scopeTexture=gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D,scopeTexture);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);
  gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,SCOPE_SIZE,1,0,gl.RGBA,gl.UNSIGNED_BYTE,null);
  const scopeBytes=new Uint8Array(SCOPE_SIZE*4);
  function updateScope(scope) {
    const values=Array.isArray(scope)?scope:[];
    for (let i=0;i<SCOPE_SIZE;i++) {
      const source=values.length?values[Math.min(values.length-1,Math.floor(i*values.length/SCOPE_SIZE))]:0;
      const value=Math.round((clamp(Number(source)||0,-1,1)*.5+.5)*255);
      scopeBytes[i*4]=value; scopeBytes[i*4+1]=value; scopeBytes[i*4+2]=value;
      scopeBytes[i*4+3]=255;
    }
    gl.bindTexture(gl.TEXTURE_2D,scopeTexture);
    gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,SCOPE_SIZE,1,0,gl.RGBA,gl.UNSIGNED_BYTE,scopeBytes);
  }

  let renderTime=0;
  function draw(info,target,texture,texture2,params) {
    gl.bindFramebuffer(gl.FRAMEBUFFER,target?target.framebuffer:null);
    gl.viewport(0,0,WIDTH,HEIGHT);
    gl.useProgram(info.program);
    if (info.time) gl.uniform1f(info.time,renderTime);
    if (info.resolution) gl.uniform2f(info.resolution,WIDTH,HEIGHT);
    for (let i=0;i<params.length && i<4;i++)
      if (info.uniforms[i]) gl.uniform1f(info.uniforms[i],params[i]);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D,texture||scopeTexture);
    if (info.texture) gl.uniform1i(info.texture,0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D,texture2||scopeTexture);
    if (info.texture2) gl.uniform1i(info.texture2,1);
    if (info.scope) gl.uniform1i(info.scope,0);
    gl.bindBuffer(gl.ARRAY_BUFFER,quad);
    gl.enableVertexAttribArray(info.attrib);
    gl.vertexAttribPointer(info.attrib,2,gl.FLOAT,false,0,0);
    gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
  }

  function drawNode(node,target) {
    if (!node) throw new Error('Hydra graph is empty');
    const generator=GENERATORS[node.kind];
    if (generator) {
      draw(generatorProgram(node.kind),target,node.kind==='src'?scopeTexture:null,null,node.params);
      return;
    }
    if (UNARY[node.kind]) {
      const source=acquire();
      drawNode(node.inputs[0],source);
      draw(unaryProgram(node.kind),target,source.texture,null,node.params);
      release(source);
      return;
    }
    if (BINARY[node.kind]) {
      const left=acquire(), right=acquire();
      drawNode(node.inputs[0],left);
      drawNode(node.inputs[1],right);
      draw(binaryProgram(node.kind),target,left.texture,right.texture,node.params);
      release(left); release(right);
      return;
    }
    throw new Error('Unknown Hydra node: '+node.kind);
  }

  const proto={
    rotate(angle,speed) { return make('rotate',[number(angle,0),number(speed,0)],[this]); },
    scale(x,y) { return make('scale',[number(x,1),number(y===undefined?x:y,1)],[this]); },
    repeat(x,y) { return make('repeat',[number(x,2),number(y===undefined?x:y,2)],[this]); },
    repeatX(count) { return make('repeatX',[number(count,2)],[this]); },
    repeatY(count) { return make('repeatY',[number(count,2)],[this]); },
    kaleid(count) { return make('kaleid',[number(count,4)],[this]); },
    pixelate(x,y) { return make('pixelate',[number(x,20),number(y===undefined?x:y,20)],[this]); },
    posterize(count,gamma) { return make('posterize',[number(count,4),number(gamma,.6)],[this]); },
    color(r,g,b) { return make('color',[number(r,1),number(g,1),number(b,1)],[this]); },
    colorama(amount) { return make('colorama',[number(amount,.005)],[this]); },
    hue(angle) { return make('hue',[number(angle,0)],[this]); },
    saturate(amount) { return make('saturate',[number(amount,1)],[this]); },
    brightness(amount) { return make('brightness',[number(amount,1)],[this]); },
    contrast(amount) { return make('contrast',[number(amount,1)],[this]); },
    invert(amount) { return make('invert',[number(amount,1)],[this]); },
    luma(amount) { return make('luma',[number(amount,1)],[this]); },
    modulate(other,amount) { return make('modulate',[number(amount,.1)],[this,other]); },
    blend(other,amount) { return make('blend',[number(amount,.5)],[this,other]); },
    add(other,amount) { return make('add',[number(amount,.5)],[this,other]); },
    sub(other,amount) { return make('sub',[number(amount,.5)],[this,other]); },
    mult(other,amount) { return make('mult',[number(amount,.5)],[this,other]); },
    diff(other) { return make('diff',[0],[this,other]); },
    layer(other) { return make('layer',[0],[this,other]); },
    out() { global.__hydraOutput=this; return this; }
  };

  function make(kind,params,inputs) {
    for (const input of inputs||[])
      if (!input || typeof input.kind!=='string') throw new Error('Invalid Hydra input');
    const node=Object.create(proto);
    node.kind=kind; node.params=params; node.inputs=inputs||[];
    node.depth=1;
    for (const input of node.inputs) node.depth=Math.max(node.depth,input.depth+1);
    if (node.depth>MAX_DEPTH) throw new Error('Hydra chain too deep');
    return node;
  }

  proto._render=function(scope,timeMs) {
    renderTime=Number.isFinite(timeMs)?timeMs*.001:(global.performance?performance.now()*.001:0);
    updateScope(scope);
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);
    gl.viewport(0,0,WIDTH,HEIGHT);
    gl.clearColor(.035,.043,.07,1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    drawNode(this,null);
  };

  const methods={
    osc(freq,sync,offset) { return make('osc',[number(freq,60),number(sync,.1),number(offset,0)],[]); },
    shape(sides,radius,smoothing) { return make('shape',[number(sides,3),number(radius,.3),number(smoothing,.01)],[]); },
    solid(r,g,b) { return make('solid',[number(r,1),number(g,1),number(b,1)],[]); },
    noise(scale,offset) { return make('noise',[number(scale,3),number(offset,.1)],[]); },
    voronoi(scale,speed) { return make('voronoi',[number(scale,5),number(speed,.3)],[]); },
    gradient(speed) { return make('gradient',[number(speed,0)],[]); },
    src(o) { return o && typeof o.kind==='string'?o:make('src',[],[]); }
  };
  Object.assign(api,methods);
  api.ready=true;
  global.Hydra=api;
  Object.assign(global,methods);
})(typeof globalThis!=='undefined'?globalThis:this);
