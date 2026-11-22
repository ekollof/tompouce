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

  let WIDTH = Math.max(1, Math.min(1920, canvas.width || 960));
  let HEIGHT = Math.max(1, Math.min(1080, canvas.height || 540));
  const SCOPE_SIZE = 256;
  const POOL_SIZE = 20;
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
float hydraLuma(vec3 c){ return dot(c,vec3(.2125,.7154,.0721)); }
`;
  const BLIT = `uniform sampler2D u_tex;
void main(){ gl_FragColor=texture2D(u_tex,v_uv); }`;
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
    solid: `uniform float p0,p1,p2,p3;
void main(){ gl_FragColor=vec4(p0,p1,p2,p3); }`,
    noise: `uniform float p0,p1;
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453123); }
float vnoise(vec2 p){ vec2 i=floor(p),f=fract(p); f=f*f*(3.-2.*f);
  return mix(mix(hash(i),hash(i+vec2(1.,0.)),f.x),
             mix(hash(i+vec2(0.,1.)),hash(i+vec2(1.,1.)),f.x),f.y); }
void main(){ float n=vnoise(v_uv*max(.01,p0)+u_time*p1);
  gl_FragColor=vec4(vec3(n),1.); }`,
    voronoi: `uniform float p0,p1,p2;
vec2 hash2(vec2 p){ p=vec2(dot(p,vec2(127.1,311.7)),dot(p,vec2(269.5,183.3)));
  return fract(sin(p)*43758.5453); }
float cells(vec2 x){ vec2 n=floor(x),f=fract(x); float md=8.;
  for(int j=-1;j<=1;j++) for(int i=-1;i<=1;i++){
    vec2 g=vec2(float(i),float(j)); vec2 o=hash2(n+g); vec2 r=g+o-f;
    md=min(md,dot(r,r)); }
  return sqrt(md); }
void main(){ float d=cells(v_uv*max(.01,p0)+u_time*p1);
  gl_FragColor=vec4(vec3(clamp(1.-d,0.,1.)*(1.+p2)),1.); }`,
    gradient: `uniform float p0;
void main(){ gl_FragColor=vec4(vec3(fract(v_uv.y+u_time*p0)),1.); }`,
    src: `uniform sampler2D u_scope;
void main(){ float s=texture2D(u_scope,vec2(v_uv.x,.5)).r*2.-1.;
  float d=abs(v_uv.y-.5-s*.35);
  float line=1.-smoothstep(.004,.02,d);
  gl_FragColor=vec4(vec3(line*.9+.03),1.); }`,
    srcTex: `uniform sampler2D u_tex;
void main(){ gl_FragColor=texture2D(u_tex,fract(v_uv)); }`,
    prev: `uniform sampler2D u_prev;
void main(){ gl_FragColor=texture2D(u_prev,fract(v_uv)); }`
  };

  const UNARY = {
    rotate: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 p=v_uv-.5; float a=p0+u_time*p1;
  vec2 uv=mat2(cos(a),-sin(a),sin(a),cos(a))*p+.5;
  gl_FragColor=texture2D(u_tex,uv); }`,
    scale: `uniform sampler2D u_tex; uniform float p0,p1,p2,p3,p4;
void main(){ vec2 xy=v_uv-vec2(p3,p4);
  xy*=1.0/vec2(max(.0001,abs(p0*p1)),max(.0001,abs(p0*p2)));
  gl_FragColor=texture2D(u_tex,xy+vec2(p3,p4)); }`,
    pixelate: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 n=max(vec2(1.),vec2(p0,p1));
  gl_FragColor=texture2D(u_tex,(floor(v_uv*n)+.5)/n); }`,
    repeat: `uniform sampler2D u_tex; uniform float p0,p1,p2,p3;
void main(){ vec2 st=v_uv*vec2(p0,p1);
  st.x+=step(1.,mod(st.y,2.0))*p2;
  st.y+=step(1.,mod(st.x,2.0))*p3;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    repeatX: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 st=v_uv*vec2(p0,1.);
  st.y+=step(1.,mod(st.x,2.0))*p1;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    repeatY: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec2 st=v_uv*vec2(1.,p0);
  st.x+=step(1.,mod(st.y,2.0))*p1;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    kaleid: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec2 st=v_uv-.5; float r=length(st); float a=atan(st.y,st.x);
  float n=max(1.,floor(p0)); float pi=6.2831853;
  a=mod(a,pi/n); a=abs(a-pi/n/2.);
  gl_FragColor=texture2D(u_tex,r*vec2(cos(a),sin(a))+.5); }`,
    scroll: `uniform sampler2D u_tex; uniform float p0,p1,p2,p3;
void main(){ gl_FragColor=texture2D(u_tex,fract(v_uv+vec2(p0+u_time*p2,p1+u_time*p3))); }`,
    scrollX: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ gl_FragColor=texture2D(u_tex,fract(vec2(v_uv.x+p0+u_time*p1,v_uv.y))); }`,
    scrollY: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ gl_FragColor=texture2D(u_tex,fract(vec2(v_uv.x,v_uv.y+p0+u_time*p1))); }`,
    posterize: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec4 c2=pow(texture2D(u_tex,v_uv),vec4(p1));
  c2*=vec4(max(1.,floor(p0))); c2=floor(c2);
  c2/=vec4(max(1.,floor(p0))); c2=pow(c2,vec4(1./max(.0001,p1)));
  gl_FragColor=vec4(c2.rgb,texture2D(u_tex,v_uv).a); }`,
    shift: `uniform sampler2D u_tex; uniform float p0,p1,p2,p3;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  c=vec4(fract(c.r+p0),fract(c.g+p1),fract(c.b+p2),fract(c.a+p3));
  gl_FragColor=c; }`,
    color: `uniform sampler2D u_tex; uniform float p0,p1,p2,p3;
void main(){ vec4 c=vec4(p0,p1,p2,p3); vec4 pos=step(0.,c);
  gl_FragColor=mix((1.-texture2D(u_tex,v_uv))*abs(c),c*texture2D(u_tex,v_uv),pos); }`,
    colorama: `uniform sampler2D u_tex; uniform float p0;
${HSV}
void main(){ vec4 c=texture2D(u_tex,v_uv);
  vec3 h=rgb2hsv(clamp(c.rgb,0.,1.)); h+=vec3(p0);
  gl_FragColor=vec4(fract(hsv2rgb(h)),c.a); }`,
    hue: `uniform sampler2D u_tex; uniform float p0;
${HSV}
void main(){ vec4 c=texture2D(u_tex,v_uv);
  vec3 h=rgb2hsv(clamp(c.rgb,0.,1.)); h.x+=p0;
  gl_FragColor=vec4(hsv2rgb(h),c.a); }`,
    saturate: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  vec3 intensity=vec3(hydraLuma(c.rgb));
  gl_FragColor=vec4(mix(intensity,c.rgb,p0),c.a); }`,
    brightness: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv); c.rgb+=vec3(p0); gl_FragColor=c; }`,
    contrast: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=(texture2D(u_tex,v_uv)-.5)*p0+.5;
  gl_FragColor=vec4(c.rgb,texture2D(u_tex,v_uv).a); }`,
    invert: `uniform sampler2D u_tex; uniform float p0;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  gl_FragColor=vec4((1.-c.rgb)*p0+c.rgb*(1.-p0),c.a); }`,
    luma: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  float a=smoothstep(p0-(p1+.0000001),p0+(p1+.0000001),hydraLuma(c.rgb));
  gl_FragColor=vec4(c.rgb*a,a); }`,
    thresh: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ vec4 c=texture2D(u_tex,v_uv);
  float a=smoothstep(p0-(p1+.0000001),p0+(p1+.0000001),hydraLuma(c.rgb));
  gl_FragColor=vec4(vec3(a),c.a); }`,
    r: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ gl_FragColor=vec4(texture2D(u_tex,v_uv).r*p0+p1); }`,
    g: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ gl_FragColor=vec4(texture2D(u_tex,v_uv).g*p0+p1); }`,
    b: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ gl_FragColor=vec4(texture2D(u_tex,v_uv).b*p0+p1); }`,
    a: `uniform sampler2D u_tex; uniform float p0,p1;
void main(){ gl_FragColor=vec4(texture2D(u_tex,v_uv).a*p0+p1); }`,
    sum: `uniform sampler2D u_tex; uniform float p0,p1,p2,p3;
void main(){ vec4 c=texture2D(u_tex,v_uv)*vec4(p0,p1,p2,p3);
  gl_FragColor=vec4(vec3(c.r+c.g+c.b+c.a),1.); }`
  };

  const BINARY = {
    modulate: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec2 uv=v_uv+texture2D(u_tex2,v_uv).xy*p0;
  gl_FragColor=texture2D(u_tex,uv); }`,
    modulateRepeat: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1,p2,p3;
void main(){ vec2 st=v_uv*vec2(p0,p1); vec4 m=texture2D(u_tex2,v_uv);
  st.x+=step(1.,mod(st.y,2.0))+m.r*p2;
  st.y+=step(1.,mod(st.x,2.0))+m.g*p3;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    modulateRepeatX: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1;
void main(){ vec2 st=v_uv*vec2(p0,1.); vec4 m=texture2D(u_tex2,v_uv);
  st.y+=step(1.,mod(st.x,2.0))+m.r*p1;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    modulateRepeatY: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1;
void main(){ vec2 st=v_uv*vec2(1.,p0); vec4 m=texture2D(u_tex2,v_uv);
  st.x+=step(1.,mod(st.y,2.0))+m.r*p1;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    modulateKaleid: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec2 st=v_uv-.5; float r=length(st); float a=atan(st.y,st.x);
  float n=max(1.,floor(p0)); float pi=6.2831853;
  a=mod(a,pi/n); a=abs(a-pi/n/2.);
  r+=texture2D(u_tex2,v_uv).r;
  gl_FragColor=texture2D(u_tex,r*vec2(cos(a),sin(a))+.5); }`,
    modulateScrollX: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1;
void main(){ vec2 st=v_uv; st.x+=texture2D(u_tex2,v_uv).r*p0+u_time*p1;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    modulateScrollY: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1;
void main(){ vec2 st=v_uv; st.y+=texture2D(u_tex2,v_uv).r*p0+u_time*p1;
  gl_FragColor=texture2D(u_tex,fract(st)); }`,
    modulateScale: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1;
void main(){ vec2 m=texture2D(u_tex2,v_uv).rg;
  vec2 xy=v_uv-.5; xy*=1.0/vec2(p1+p0*m.x,p1+p0*m.y);
  gl_FragColor=texture2D(u_tex,xy+.5); }`,
    modulatePixelate: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1;
void main(){ vec2 m=texture2D(u_tex2,v_uv).rg;
  vec2 n=max(vec2(1.),vec2(p1+m.x*p0,p1+m.y*p0));
  gl_FragColor=texture2D(u_tex,(floor(v_uv*n)+.5)/n); }`,
    modulateRotate: `uniform sampler2D u_tex,u_tex2; uniform float p0,p1;
void main(){ vec2 xy=v_uv-.5; float a=p1+texture2D(u_tex2,v_uv).r*p0;
  xy=mat2(cos(a),-sin(a),sin(a),cos(a))*xy;
  gl_FragColor=texture2D(u_tex,xy+.5); }`,
    modulateHue: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 m=texture2D(u_tex2,v_uv);
  vec2 uv=v_uv+vec2(m.g-m.r,m.b-m.g)*p0/u_resolution;
  gl_FragColor=texture2D(u_tex,uv); }`,
    add: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=(a+b)*p0+a*(1.-p0); }`,
    sub: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=(a-b)*p0+a*(1.-p0); }`,
    layer: `uniform sampler2D u_tex,u_tex2;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=vec4(mix(a.rgb,b.rgb,b.a),clamp(a.a+b.a,0.,1.)); }`,
    blend: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=a*(1.-p0)+b*p0; }`,
    mult: `uniform sampler2D u_tex,u_tex2; uniform float p0;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=a*(1.-p0)+(a*b)*p0; }`,
    diff: `uniform sampler2D u_tex,u_tex2;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  gl_FragColor=vec4(abs(a.rgb-b.rgb),max(a.a,b.a)); }`,
    mask: `uniform sampler2D u_tex,u_tex2;
void main(){ vec4 a=texture2D(u_tex,v_uv),b=texture2D(u_tex2,v_uv);
  float l=hydraLuma(b.rgb); gl_FragColor=vec4(a.rgb*l,a.a*l); }`
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
    for (let i=0;i<8;i++) uniforms.push(gl.getUniformLocation(program,'p'+i));
    return {
      program,
      attrib: gl.getAttribLocation(program,'a_pos'),
      time: gl.getUniformLocation(program,'u_time'),
      resolution: gl.getUniformLocation(program,'u_resolution'),
      texture: gl.getUniformLocation(program,'u_tex'),
      texture2: gl.getUniformLocation(program,'u_tex2'),
      scope: gl.getUniformLocation(program,'u_scope'),
      prev: gl.getUniformLocation(program,'u_prev'),
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
  function blitProgram() {
    if (!programs.has('blit')) programs.set('blit',createProgram('blit',BLIT));
    return programs.get('blit');
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
  let feedbackTarget=createTarget();
  let feedbackTexture=feedbackTarget.texture;

  function destroyTarget(target) {
    if (!target) return;
    gl.deleteFramebuffer(target.framebuffer);
    gl.deleteTexture(target.texture);
  }
  function setResolution(width,height) {
    const w=Math.max(1,Math.min(1920,Math.round(number(width,WIDTH))));
    const h=Math.max(1,Math.min(1080,Math.round(number(height,HEIGHT))));
    if (w===WIDTH && h===HEIGHT) return;
    WIDTH=w; HEIGHT=h;
    canvas.width=w; canvas.height=h;
    for (const target of pool) destroyTarget(target);
    pool.length=0;
    for (let i=0;i<POOL_SIZE;i++) pool.push(createTarget());
    destroyTarget(feedbackTarget);
    feedbackTarget=createTarget();
    feedbackTexture=feedbackTarget.texture;
  }
  function hush() {
    global.__hydraOutput=null;
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);
    gl.viewport(0,0,WIDTH,HEIGHT);
    gl.clearColor(.035,.043,.07,1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindFramebuffer(gl.FRAMEBUFFER,feedbackTarget.framebuffer);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);
    return api;
  }
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

  /* Four bounded external sources (s0..s3), matching Hydra's camera/screen/
     image API. Video textures are uploaded only when the element is ready;
     a failed permission request leaves the source black instead of breaking
     the render loop. */
  const SOURCE_MAX=4096, SOURCE_URL_MAX=2048;
  const sources=[];
  function sourceBlank(texture) {
    gl.bindTexture(gl.TEXTURE_2D,texture);
    gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,
      new Uint8Array([0,0,0,255]));
  }
  function makeSource(label) {
    const texture=gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D,texture);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
    sourceBlank(texture);
    const source={kind:'source',name:label,texture,element:null,stream:null,
      width:1,height:1,ready:false,dynamic:false,error:null,params:null};
    source.clear=function() {
      if (source.stream && source.stream.getTracks)
        for (const track of source.stream.getTracks()) track.stop();
      if (source.element && source.element.srcObject) source.element.srcObject=null;
      if (source.element && source.element.tagName==='VIDEO') {
        try { source.element.pause(); } catch (error) {}
      }
      source.element=null; source.stream=null; source.width=source.height=1;
      source.ready=false; source.dynamic=false;
      sourceBlank(source.texture);
      return source;
    };
    source.attach=function(stream) {
      if (!stream || !stream.getTracks) {
        source.error='media stream unavailable';
        return Promise.resolve(source);
      }
      if (source.stream && source.stream.getTracks)
        for (const track of source.stream.getTracks()) track.stop();
      const video=global.document.createElement('video');
      video.muted=true; video.autoplay=true; video.playsInline=true;
      video.srcObject=stream;
      source.element=video; source.stream=stream; source.dynamic=true;
      source.ready=false; source.error=null;
      const played=video.play ? video.play() : Promise.resolve();
      return Promise.resolve(played).catch(()=>{}).then(()=>source);
    };
    source.initCam=function(index,params) {
      source.params=params||null;
      const media=global.navigator && global.navigator.mediaDevices;
      if (!media || !media.getUserMedia) {
        source.error='camera unavailable';
        return Promise.resolve(source);
      }
      return media.getUserMedia({video:true})
        .then(stream=>source.attach(stream))
        .catch(error=>{source.error=String(error&&error.message||error);return source;});
    };
    source.initScreen=function(index,params) {
      source.params=params||null;
      const media=global.navigator && global.navigator.mediaDevices;
      if (!media || !media.getDisplayMedia) {
        source.error='screen capture unavailable';
        return Promise.resolve(source);
      }
      return media.getDisplayMedia({video:true})
        .then(stream=>source.attach(stream))
        .catch(error=>{source.error=String(error&&error.message||error);return source;});
    };
    source.initStream=function(stream,params) {
      source.params=params||null;
      return source.attach(stream);
    };
    source.initImage=function(url,params) {
      source.params=params||null;
      const text=String(url||'').slice(0,SOURCE_URL_MAX);
      if (!text || !global.Image) {
        source.error='image unavailable';
        return Promise.resolve(source);
      }
      const image=new global.Image();
      image.crossOrigin='anonymous';
      image.onload=()=>{source.element=image;source.dynamic=false;source.ready=false;source.error=null;};
      image.onerror=()=>{source.error='image load failed';};
      image.src=text;
      return Promise.resolve(source);
    };
    source.initVideo=function(url,params) {
      source.params=params||null;
      const text=String(url||'').slice(0,SOURCE_URL_MAX);
      const video=global.document.createElement('video');
      video.crossOrigin='anonymous'; video.autoplay=true; video.loop=true; video.muted=true;
      video.playsInline=true;
      video.onloadeddata=()=>{source.element=video;source.dynamic=true;source.ready=false;source.error=null;
        if (video.play) Promise.resolve(video.play()).catch(()=>{});};
      video.onerror=()=>{source.error='video load failed';};
      video.src=text;
      return Promise.resolve(source);
    };
    source.tick=function() {
      const element=source.element;
      if (!element) return;
      const width=element.videoWidth||element.naturalWidth||element.width||0;
      const height=element.videoHeight||element.naturalHeight||element.height||0;
      if (!width || !height || width>SOURCE_MAX || height>SOURCE_MAX) return;
      if (source.dynamic && element.readyState!==undefined && element.readyState<2) return;
      try {
        gl.bindTexture(gl.TEXTURE_2D,source.texture);
        gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,element);
        source.width=width; source.height=height; source.ready=true;
      } catch (error) {
        source.error=String(error&&error.message||error);
      }
    };
    sources.push(source);
    return source;
  }
  const s0=makeSource('s0'), s1=makeSource('s1');
  const s2=makeSource('s2'), s3=makeSource('s3');

  let renderTime=0;
  function draw(info,target,texture,texture2,params,prev) {
    gl.bindFramebuffer(gl.FRAMEBUFFER,target?target.framebuffer:null);
    gl.viewport(0,0,WIDTH,HEIGHT);
    gl.useProgram(info.program);
    if (info.time) gl.uniform1f(info.time,renderTime);
    if (info.resolution) gl.uniform2f(info.resolution,WIDTH,HEIGHT);
    for (let i=0;i<params.length && i<8;i++)
      if (info.uniforms[i]) gl.uniform1f(info.uniforms[i],params[i]);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D,texture||scopeTexture);
    if (info.texture) gl.uniform1i(info.texture,0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D,texture2||scopeTexture);
    if (info.texture2) gl.uniform1i(info.texture2,1);
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D,prev||feedbackTexture||scopeTexture);
    if (info.prev) gl.uniform1i(info.prev,2);
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
      const texture=node.kind==='src'
        ? (node.source && node.source.texture ? node.source.texture : scopeTexture)
        : node.kind==='srcTex'
          ? (node.source && node.source.texture ? node.source.texture : scopeTexture)
          : node.kind==='prev'?feedbackTexture:null;
      draw(generatorProgram(node.kind),target,texture,null,node.params);
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
    rotate(angle,speed) { return make('rotate',[number(angle,10),number(speed,0)],[this]); },
    scale(amount,xMult,yMult,offsetX,offsetY) {
      return make('scale',[number(amount,1.5),number(xMult,1),number(yMult,1),
        number(offsetX,.5),number(offsetY,.5)],[this]);
    },
    pixelate(x,y) { return make('pixelate',[number(x,20),number(y===undefined?x:y,20)],[this]); },
    posterize(bins,gamma) { return make('posterize',[number(bins,3),number(gamma,.6)],[this]); },
    shift(r,g,b,a) { return make('shift',[number(r,.5),number(g,0),number(b,0),number(a,0)],[this]); },
    repeat(x,y,offsetX,offsetY) {
      return make('repeat',[number(x,3),number(y===undefined?x:y,3),
        number(offsetX,0),number(offsetY,0)],[this]);
    },
    repeatX(reps,offset) { return make('repeatX',[number(reps,3),number(offset,0)],[this]); },
    repeatY(reps,offset) { return make('repeatY',[number(reps,3),number(offset,0)],[this]); },
    kaleid(count) { return make('kaleid',[number(count,4)],[this]); },
    scroll(x,y,speedX,speedY) {
      return make('scroll',[number(x,.5),number(y,.5),
        number(speedX,0),number(speedY,0)],[this]);
    },
    scrollX(x,speed) { return make('scrollX',[number(x,.5),number(speed,0)],[this]); },
    scrollY(y,speed) { return make('scrollY',[number(y,.5),number(speed,0)],[this]); },
    modulate(other,amount) { return make('modulate',[number(amount,.1)],[this,other]); },
    modulateRepeat(other,repeatX,repeatY,offsetX,offsetY) {
      return make('modulateRepeat',[number(repeatX,3),number(repeatY,3),
        number(offsetX,.5),number(offsetY,.5)],[this,other]);
    },
    modulateRepeatX(other,reps,offset) {
      return make('modulateRepeatX',[number(reps,3),number(offset,.5)],[this,other]);
    },
    modulateRepeatY(other,reps,offset) {
      return make('modulateRepeatY',[number(reps,3),number(offset,.5)],[this,other]);
    },
    modulateKaleid(other,count) {
      return make('modulateKaleid',[number(count,4)],[this,other]);
    },
    modulateScrollX(other,x,speed) {
      return make('modulateScrollX',[number(x,.5),number(speed,0)],[this,other]);
    },
    modulateScrollY(other,y,speed) {
      return make('modulateScrollY',[number(y,.5),number(speed,0)],[this,other]);
    },
    modulateScale(other,multiple,offset) {
      return make('modulateScale',[number(multiple,1),number(offset,1)],[this,other]);
    },
    modulatePixelate(other,multiple,offset) {
      return make('modulatePixelate',[number(multiple,10),number(offset,3)],[this,other]);
    },
    modulateRotate(other,multiple,offset) {
      return make('modulateRotate',[number(multiple,1),number(offset,0)],[this,other]);
    },
    modulateHue(other,amount) {
      return make('modulateHue',[number(amount,1)],[this,other]);
    },
    add(other,amount) { return make('add',[number(amount,1)],[this,other]); },
    sub(other,amount) { return make('sub',[number(amount,1)],[this,other]); },
    layer(other) { return make('layer',[0],[this,other]); },
    blend(other,amount) { return make('blend',[number(amount,.5)],[this,other]); },
    mult(other,amount) { return make('mult',[number(amount,1)],[this,other]); },
    diff(other) { return make('diff',[0],[this,other]); },
    mask(other) { return make('mask',[0],[this,other]); },
    invert(amount) { return make('invert',[number(amount,1)],[this]); },
    contrast(amount) { return make('contrast',[number(amount,1.6)],[this]); },
    brightness(amount) { return make('brightness',[number(amount,.4)],[this]); },
    luma(threshold,tolerance) {
      return make('luma',[number(threshold,.5),number(tolerance,.1)],[this]);
    },
    thresh(threshold,tolerance) {
      return make('thresh',[number(threshold,.5),number(tolerance,.04)],[this]);
    },
    color(r,g,b,a) {
      return make('color',[number(r,1),number(g,1),number(b,1),number(a,1)],[this]);
    },
    saturate(amount) { return make('saturate',[number(amount,2)],[this]); },
    hue(angle) { return make('hue',[number(angle,.4)],[this]); },
    colorama(amount) { return make('colorama',[number(amount,.005)],[this]); },
    r(scale,offset) { return make('r',[number(scale,1),number(offset,0)],[this]); },
    g(scale,offset) { return make('g',[number(scale,1),number(offset,0)],[this]); },
    b(scale,offset) { return make('b',[number(scale,1),number(offset,0)],[this]); },
    a(scale,offset) { return make('a',[number(scale,1),number(offset,0)],[this]); },
    sum(scale) {
      const values=Array.isArray(scale)?scale:[scale,scale,scale,scale];
      return make('sum',[number(values[0],1),number(values[1],1),
        number(values[2],1),number(values[3],1)],[this]);
    },
    out() { global.__hydraOutput=this; return this; }
  };

  function make(kind,params,inputs) {
    const normalized=[];
    for (const input of inputs||[]) {
      if (input && input.kind==='source') {
        const node=Object.create(proto);
        node.kind='srcTex'; node.params=[]; node.inputs=[]; node.depth=1;
        node.source=input; normalized.push(node);
      } else normalized.push(input);
    }
    for (const input of normalized)
      if (!input || typeof input.kind!=='string') throw new Error('Invalid Hydra input');
    const node=Object.create(proto);
    node.kind=kind; node.params=params; node.inputs=normalized;
    node.depth=1;
    for (const input of node.inputs) node.depth=Math.max(node.depth,input.depth+1);
    if (node.depth>MAX_DEPTH) throw new Error('Hydra chain too deep');
    return node;
  }

  proto._render=function(scope,timeMs) {
    renderTime=Number.isFinite(timeMs)?timeMs*.001:(global.performance?performance.now()*.001:0);
    for (const source of sources) source.tick();
    updateScope(scope);
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);
    gl.viewport(0,0,WIDTH,HEIGHT);
    gl.clearColor(.035,.043,.07,1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const output=acquire();
    drawNode(this,output);
    draw(blitProgram(),feedbackTarget,output.texture,null,[]);
    draw(blitProgram(),null,output.texture,null,[]);
    release(output);
  };

  const methods={
    osc(freq,sync,offset) { return make('osc',[number(freq,60),number(sync,.1),number(offset,0)],[]); },
    shape(sides,radius,smoothing) { return make('shape',[number(sides,3),number(radius,.3),number(smoothing,.01)],[]); },
    solid(r,g,b,a) { return make('solid',[number(r,1),number(g,1),number(b,1),number(a,1)],[]); },
    noise(scale,offset) { return make('noise',[number(scale,3),number(offset,.1)],[]); },
    voronoi(scale,speed,blending) { return make('voronoi',[number(scale,5),number(speed,.3),number(blending,.3)],[]); },
    gradient(speed) { return make('gradient',[number(speed,0)],[]); },
    prev() { return make('prev',[],[]); },
    src(o) {
      if (o && o.kind==='source') {
        const node=make('srcTex',[],[]);
        node.source=o;
        return node;
      }
      return o && typeof o.kind==='string'?o:make('src',[],[]);
    }
  };
  Object.assign(api,methods);
  Object.assign(api,{s0,s1,s2,s3,setResolution,hush});
  global.s0=s0; global.s1=s1; global.s2=s2; global.s3=s3;
  global.setResolution=setResolution; global.hush=hush;
  api.ready=true;
  global.Hydra=api;
  Object.assign(global,methods);
})(typeof globalThis!=='undefined'?globalThis:this);
