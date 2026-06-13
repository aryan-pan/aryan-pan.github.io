/*
 * rain.js — photoreal rain-on-glass backdrop (WebGL2 fragment shader).
 *
 * Renders a chosen scene behind a steamed-up window with droplets that refract
 * the background through their surface normals (BigWings "Heartfelt" technique).
 * Adds: cursor fog-wipe, mouse/scroll parallax, and a scene switcher that swaps
 * the backdrop (forest / city / mountains / aurora) with per-scene tuning.
 *
 * Honors prefers-reduced-motion (single still frame) and pauses when hidden.
 */
(function () {
  'use strict';

  var canvas = document.getElementById('rain-canvas');
  if (!canvas) return;

  var gl = canvas.getContext('webgl2', { antialias: false, alpha: false, powerPreference: 'high-performance' });
  if (!gl) { canvas.style.display = 'none'; document.documentElement.classList.add('no-rain'); return; }

  var reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  // default look (tuned in the prototype): fogged glass
  var DEF = { rain: 0.45, fog: 0.33, maxBlur: 3.3, refr: 0.10 };

  // line-art scene icons (currentColor)
  var ICONS = {
    forest: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3 7 11h3l-4 6h12l-4-6h3z"/><path d="M12 17v4"/></svg>',
    city: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M3 21V10h5v11M8 21V4h6v17M14 21v-8h7v8"/><path d="M3 21h18"/></svg>',
    mountains: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M3 19 9 8l4 7 2-3 6 7z"/></svg>',
    aurora: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4 14c3-8 6-8 8-2s5 4 8-2"/><path d="M5 18c3-7 6-6 8-1"/></svg>'
  };

  // scenes from the canvas data-scenes attribute (fallback to data-bg)
  var scenes;
  try { scenes = JSON.parse(canvas.getAttribute('data-scenes')); } catch (e) { scenes = null; }
  if (!scenes || !scenes.length) {
    scenes = [{ name: 'Backdrop', src: canvas.getAttribute('data-bg') || '/assets/images/foggy_forest.jpg' }];
  }
  var active = 0;
  try { var saved = parseInt(localStorage.getItem('rain-scene'), 10); if (saved >= 0 && saved < scenes.length) active = saved; } catch (e) {}

  var VERT = '#version 300 es\n' +
    'layout(location=0) in vec2 a_pos;\n' +
    'void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); }';

  var FRAG = '#version 300 es\n' +
    'precision highp float;\n' +
    'uniform vec2 u_resolution;\n' +
    'uniform vec2 u_texResolution;\n' +
    'uniform float u_time;\n' +
    'uniform sampler2D u_tex;\n' +
    'uniform float u_rain, u_fog, u_maxBlur, u_refr, u_reduced;\n' +
    'uniform vec2 u_mouse;\n' +
    'uniform float u_wipe;\n' +
    'uniform vec2 u_parallax;\n' +
    'out vec4 outColor;\n' +
    '#define S(a,b,t) smoothstep(a,b,t)\n' +
    'vec3 N13(float p){vec3 p3=fract(vec3(p)*vec3(.1031,.11369,.13787));p3+=dot(p3,p3.yzx+19.19);return fract(vec3((p3.x+p3.y)*p3.z,(p3.x+p3.z)*p3.y,(p3.y+p3.z)*p3.x));}\n' +
    'float N(float t){return fract(sin(t*12345.564)*7658.76);}\n' +
    'float Saw(float b,float t){return S(0.,b,t)*S(1.,b,t);}\n' +
    'float hash21(vec2 p){p=fract(p*vec2(123.34,456.21));p+=dot(p,p+45.32);return fract(p.x*p.y);}\n' +
    'float vnoise(vec2 p){vec2 i=floor(p),f=fract(p);float a=hash21(i),b=hash21(i+vec2(1,0)),c=hash21(i+vec2(0,1)),d=hash21(i+vec2(1,1));vec2 u=f*f*(3.-2.*f);return mix(mix(a,b,u.x),mix(c,d,u.x),u.y);}\n' +
    'float fbm(vec2 p){float s=0.,a=.5;for(int i=0;i<4;i++){s+=a*vnoise(p);p*=2.;a*=.5;}return s;}\n' +
    'vec2 DropLayer2(vec2 uv,float t){vec2 UV=uv;uv.y+=t*0.75;vec2 a=vec2(6.,1.);vec2 grid=a*2.;vec2 id=floor(uv*grid);float colShift=N(id.x);uv.y+=colShift;id=floor(uv*grid);vec3 n=N13(id.x*35.2+id.y*2376.1);vec2 st=fract(uv*grid)-vec2(.5,0);float x=n.x-.5;float y=UV.y*20.;float wiggle=sin(y+sin(y))+0.5*sin(y*2.3+n.z*6.2831);x+=wiggle*(.5-abs(x))*(n.z-.5)*1.5;x*=.7;float ti=fract(t+n.z);y=(Saw(.85,ti)-.5)*.9+.5;vec2 p=vec2(x,y);float d=length((st-p)*a.yx);float mainDrop=S(.4,.0,d);float r=sqrt(S(1.,y,st.y));float cd=abs(st.x-x);float trail=S(.23*r,.15*r*r,cd);float trailFront=S(-.02,.02,st.y-y);trail*=trailFront*r*r;y=UV.y;float trail2=S(.2*r,.0,cd);float droplets=max(0.,(sin(y*(1.-y)*120.)-st.y))*trail2*trailFront*n.z;y=fract(y*10.)+(st.y-.5);float dd=length(st-vec2(x,y));droplets=S(.3,0.,dd);float m=mainDrop+droplets*r*trailFront;return vec2(m,trail);}\n' +
    'float StaticDrops(vec2 uv,float t){uv*=40.;vec2 id=floor(uv);uv=fract(uv)-.5;vec3 n=N13(id.x*107.45+id.y*3543.654);vec2 p=(n.xy-.5)*.7;float d=length(uv-p);float fade=Saw(.025,fract(t+n.z));float c=S(.3,0.,d)*fract(n.z*10.)*fade;return c;}\n' +
    'vec2 Drops(vec2 uv,float t,float l0,float l1,float l2){float s=StaticDrops(uv,t)*l0;vec2 m1=DropLayer2(uv,t)*l1;vec2 m2=DropLayer2(uv*1.85,t)*l2;float c=s+m1.x+m2.x;c=S(.3,1.,c);return vec2(c,max(m1.y*l0,m2.y*l1));}\n' +
    'vec2 coverUV(vec2 uv,vec2 res,vec2 texRes){float sa=res.x/res.y,ta=texRes.x/texRes.y;vec2 sc=sa>ta?vec2(1.0,ta/sa):vec2(sa/ta,1.0);return (uv-.5)*sc+.5;}\n' +
    'void main(){\n' +
    '  vec2 fragCoord=gl_FragCoord.xy;\n' +
    '  vec2 uv=(fragCoord-.5*u_resolution)/u_resolution.y;\n' +
    '  vec2 UV=fragCoord/u_resolution;\n' +
    '  float T=u_time;\n' +
    '  float t=T*.2;\n' +
    '  float rainAmount=u_rain;\n' +
    '  float maxBlur=u_maxBlur;\n' +
    '  float minBlur=2.6;\n' +
    '  float zoom=-cos(T*.05)*(1.0-u_reduced);\n' +
    '  uv*=.85+zoom*.04;\n' +
    '  float staticDrops=S(-.5,1.,rainAmount)*2.;\n' +
    '  float layer1=S(.25,.75,rainAmount);\n' +
    '  float layer2=S(.0,.5,rainAmount);\n' +
    '  vec2 c=Drops(uv,t,staticDrops,layer1,layer2);\n' +
    '  vec2 e=vec2(.0025,0.);\n' +
    '  float cx=Drops(uv+e.xy,t,staticDrops,layer1,layer2).x;\n' +
    '  float cy=Drops(uv+e.yx,t,staticDrops,layer1,layer2).x;\n' +
    '  vec2 n=vec2(cx-c.x,cy-c.x);\n' +
    '  float focus=mix(maxBlur-c.y*maxBlur,minBlur,S(.1,.2,c.x));\n' +
    '  vec2 mp=u_mouse; mp.x*=u_resolution.x/u_resolution.y; vec2 fp=UV; fp.x*=u_resolution.x/u_resolution.y;\n' +
    '  float wipe=S(0.22,0.0,length(fp-mp))*u_wipe;\n' +
    '  focus=mix(focus,minBlur,wipe);\n' +
    '  vec2 bgUV=coverUV(UV,u_resolution,u_texResolution)+u_parallax;\n' +
    '  vec3 col=textureLod(u_tex,bgUV+n*u_refr,focus).rgb;\n' +
    '  float clarity=clamp(c.x*1.6+c.y*1.2,0.,1.);\n' +
    '  clarity=max(clarity,wipe);\n' +
    '  float drift=u_reduced>.5?0.0:T*0.03;\n' +
    '  float steamVar=fbm(uv*3.0+vec2(0.0,-drift));\n' +
    '  vec3 fogCol=vec3(0.60,0.65,0.66);\n' +
    '  float steam=(1.0-clarity)*u_fog*(0.55+0.7*steamVar);\n' +
    '  col=mix(col,fogCol,clamp(steam,0.0,1.0));\n' +
    '  col=mix(col,col*vec3(0.92,0.98,1.04),(1.0-clarity)*0.5);\n' +
    '  float drop=clamp(c.x,0.0,1.0);\n' +
    '  float rim=S(0.04,0.22,drop)*(1.0-S(0.22,0.55,drop));\n' +
    '  col*=1.0-rim*0.18;\n' +
    '  float body=S(0.25,0.95,drop);\n' +
    '  col+=vec3(0.85,0.92,1.0)*body*0.045;\n' +
    '  float hi=S(0.55,1.0,drop)*clamp(0.5-n.y*12.0-n.x*12.0,0.0,1.0);\n' +
    '  col+=vec3(1.0)*hi*0.18;\n' +
    '  vec2 q=UV-.5; col*=1.0-dot(q,q)*0.85; col*=0.93;\n' +
    '  outColor=vec4(col,1.0);\n' +
    '}';

  function compile(type, src) {
    var s = gl.createShader(type);
    gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) console.error(gl.getShaderInfoLog(s));
    return s;
  }
  var program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER, VERT));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, FRAG));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) { console.error(gl.getProgramInfoLog(program)); canvas.style.display = 'none'; return; }
  gl.useProgram(program);

  var vao = gl.createVertexArray();
  gl.bindVertexArray(vao);
  var buf = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

  function U(n) { return gl.getUniformLocation(program, n); }
  var u = {
    res: U('u_resolution'), texRes: U('u_texResolution'), time: U('u_time'), tex: U('u_tex'),
    rain: U('u_rain'), fog: U('u_fog'), maxBlur: U('u_maxBlur'), refr: U('u_refr'), reduced: U('u_reduced'),
    mouse: U('u_mouse'), wipe: U('u_wipe'), parallax: U('u_parallax')
  };
  gl.uniform1f(u.reduced, reduced ? 1 : 0);

  var tex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, tex);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([30, 40, 35, 255]));
  var texRes = [1, 1];

  function applySceneConfig(sc) {
    gl.useProgram(program);
    gl.uniform1f(u.rain, sc.rain != null ? sc.rain : DEF.rain);
    gl.uniform1f(u.fog, sc.fog != null ? sc.fog : DEF.fog);
    gl.uniform1f(u.maxBlur, sc.blur != null ? sc.blur : DEF.maxBlur);
    gl.uniform1f(u.refr, sc.refr != null ? sc.refr : DEF.refr);
  }

  function loadScene(i, animate) {
    i = ((i % scenes.length) + scenes.length) % scenes.length;
    active = i;
    try { localStorage.setItem('rain-scene', i); } catch (e) {}
    updateSwitcher();
    applySceneConfig(scenes[i]);
    if (animate) { canvas.style.transition = 'opacity .4s ease'; canvas.style.opacity = '0.2'; }
    var im = new Image();
    im.onload = function () {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, im);
      gl.generateMipmap(gl.TEXTURE_2D);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      texRes = [im.naturalWidth, im.naturalHeight];
      canvas.style.opacity = '1';
    };
    im.onerror = function () { canvas.style.opacity = '1'; };
    im.src = scenes[i].src;
  }

  /* ---- scene switcher UI ---- */
  var switcher = null, btns = [];
  function buildSwitcher() {
    if (scenes.length < 2) return;
    switcher = document.createElement('div');
    switcher.className = 'scene-switcher glass';
    switcher.setAttribute('role', 'group');
    switcher.setAttribute('aria-label', 'Backdrop scene');
    scenes.forEach(function (sc, i) {
      var b = document.createElement('button');
      b.type = 'button';
      b.className = 'scene-btn';
      if (ICONS[sc.icon]) { b.innerHTML = ICONS[sc.icon]; } else { b.textContent = sc.name; }
      b.title = sc.name;
      b.setAttribute('aria-label', sc.name);
      b.setAttribute('aria-pressed', 'false');
      b.addEventListener('click', function () { if (i !== active) loadScene(i, true); });
      switcher.appendChild(b);
      btns.push(b);
    });
    document.body.appendChild(switcher);
  }
  function updateSwitcher() {
    btns.forEach(function (b, i) {
      var on = i === active;
      b.classList.toggle('on', on);
      b.setAttribute('aria-pressed', on ? 'true' : 'false');
    });
  }
  buildSwitcher();

  /* ---- interaction state ---- */
  var mouse = { x: 0.5, y: 0.5, tx: 0.5, ty: 0.5, wipe: 0, twipe: 0 };
  var px = 0, py = 0, tpx = 0, tpy = 0, scroll = 0;
  window.addEventListener('pointermove', function (ev) {
    mouse.tx = ev.clientX / window.innerWidth;
    mouse.ty = 1 - ev.clientY / window.innerHeight;
    mouse.twipe = 1;
    tpx = (ev.clientX / window.innerWidth - 0.5) * 2;
    tpy = (ev.clientY / window.innerHeight - 0.5) * 2;
  }, { passive: true });
  window.addEventListener('pointerleave', function () { mouse.twipe = 0; });
  window.addEventListener('scroll', function () { scroll = window.scrollY; }, { passive: true });

  var DPR_CAP = 2;
  function resize() {
    var dpr = Math.min(window.devicePixelRatio || 1, DPR_CAP);
    var w = Math.floor(window.innerWidth * dpr), h = Math.floor(window.innerHeight * dpr);
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; gl.viewport(0, 0, w, h); }
  }
  window.addEventListener('resize', resize);
  resize();

  var start = performance.now(), elapsed = 0, running = true;
  document.addEventListener('visibilitychange', function () {
    running = !document.hidden;
    if (running && !reduced) { start = performance.now() - elapsed * 1000; requestAnimationFrame(frame); }
  });

  function frame(now) {
    if (!running) return;
    resize();
    elapsed = reduced ? 7.0 : (now - start) / 1000;
    mouse.x += (mouse.tx - mouse.x) * 0.12;
    mouse.y += (mouse.ty - mouse.y) * 0.12;
    mouse.wipe += (mouse.twipe - mouse.wipe) * 0.06;
    px += (tpx - px) * 0.06; py += (tpy - py) * 0.06;
    var scrollOff = Math.min(scroll / Math.max(window.innerHeight, 1), 2);

    gl.uniform2f(u.res, canvas.width, canvas.height);
    gl.uniform2f(u.texRes, texRes[0], texRes[1]);
    gl.uniform1f(u.time, elapsed);
    gl.uniform1i(u.tex, 0);
    gl.uniform2f(u.mouse, mouse.x, mouse.y);
    gl.uniform1f(u.wipe, mouse.wipe);
    gl.uniform2f(u.parallax, px * 0.012, -py * 0.012 - scrollOff * 0.03);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    if (!reduced) requestAnimationFrame(frame);
  }

  loadScene(active, false);
  requestAnimationFrame(frame);
})();
