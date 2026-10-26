// Report tool for the z128 viewer: drag a rectangle over a problem, add a note, and
// POST it to /api/report (dirge-play backend). Each report carries two PNGs (plain and
// with the rectangle), the camera (+ the ?cam= string that restores it), and picks:
// for 9 points in the rectangle, an ID render (which draw/triangle the GPU actually
// drew there) and a CPU ray cast listing every surface along the ray, so coplanar
// competitors (z-fighting) show up as several hits at almost the same distance.
import * as THREE from 'three';

const PICK_VS = `
precision highp float;
precision highp int;      // ES 3.0 fragment ints default to mediump (16 bits): tri ids need 24
in vec3 position;
in vec2 uv;
in vec4 color;
uniform mat4 modelViewMatrix, projectionMatrix;
out vec2 vUV;
out vec4 vCol;
flat out uint vTri;
void main() {
  vUV = uv; vCol = color;
  vTri = uint(gl_VertexID) / 3u;     // drawArrays(first, count): gl_VertexID is the mesh vertex index
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}`;

const PICK_FS = `
precision highp float;
precision highp int;      // ES 3.0 fragment ints default to mediump (16 bits): tri ids need 24
in vec2 vUV;
in vec4 vCol;
flat in uint vTri;
uniform sampler2D uTex;
uniform bool uATest;
uniform float uARef;
uniform uint uDraw;
out vec4 fragColor;
void main() {
  if (uATest && min(texture(uTex, vUV).a * vCol.a * 2.0, 1.0) < uARef) discard;
  uint d = uDraw + 1u;
  fragColor = vec4(float(d & 255u), float((vTri >> 16) & 255u),
                   float((vTri >> 8) & 255u), float(vTri & 255u)) / 255.0;
}`;

// the panel: the tool's own DOM, built here so index.html stays small
function el(tag, attrs = {}, text) {
  const e = document.createElement(tag);
  Object.assign(e, attrs);
  if (text) e.textContent = text;
  return e;
}

export function initReport(ctx) {
  // ctx: { renderer, camera, root, scene, attrs (mesh id -> Float32Array), view(), setFrozen(bool), toast(msg),
  //        extraMeta() (optional: extra top-level meta.json fields, e.g. the play page's player state) }
  // Only root children with userData.draw are pickable; draws without attrs are skipped by the ray cast.
  const { renderer, camera, root, scene } = ctx;
  const canvas = renderer.domElement;

  // ---- pick scene: one ID mesh per draw (alpha-tested draws use the depth-writing pass test)
  let pickRoot = null;
  function buildPickRoot() {
    pickRoot = new THREE.Scene();
    for (const m of root.children) {
      const { draw, mode } = m.userData;
      if (!Number.isInteger(draw)) continue;     // not a room draw (e.g. the play page's skinned Vincent)
      if (mode === 2) continue;                 // blended remainder: never writes depth
      const mat = new THREE.RawShaderMaterial({
        glslVersion: THREE.GLSL3, vertexShader: PICK_VS, fragmentShader: PICK_FS,
        side: THREE.DoubleSide, depthFunc: THREE.LessDepth, blending: THREE.NoBlending,
        uniforms: {
          uTex: { value: m.material.uniforms.uTex.value },
          uATest: { value: mode === 1 }, uARef: { value: scene.draws[draw].aref },
          uDraw: { value: draw },
        },
      });
      if (m.material.polygonOffset) {          // keep the same depth tie-break as the screen
        mat.polygonOffset = true;
        mat.polygonOffsetFactor = m.material.polygonOffsetFactor;
        mat.polygonOffsetUnits = m.material.polygonOffsetUnits;
      }
      const pm = new THREE.Mesh(m.geometry, mat);
      pm.frustumCulled = false; pm.matrixAutoUpdate = false;
      pickRoot.add(pm);
    }
  }

  function bufferSize() {
    return new THREE.Vector2(canvas.width, canvas.height);   // drawing-buffer pixels
  }

  // render `what` into an offscreen RGBA8 target at canvas size; return {rt, w, h}
  function renderToTarget(what, clear) {
    const { x: w, y: h } = bufferSize();
    const rt = new THREE.WebGLRenderTarget(w, h, { depthBuffer: true, type: THREE.UnsignedByteType });
    const oldColor = renderer.getClearColor(new THREE.Color()), oldAlpha = renderer.getClearAlpha();
    if (clear) renderer.setClearColor(new THREE.Color(0, 0, 0), 0);
    renderer.setRenderTarget(rt);
    renderer.clear();
    renderer.render(what, camera);
    renderer.setRenderTarget(null);
    renderer.setClearColor(oldColor, oldAlpha);
    return { rt, w, h };
  }

  // the frame as an offscreen 2D canvas (rows flipped to top-first)
  function snapshot() {
    const { rt, w, h } = renderToTarget(root, false);
    const px = new Uint8Array(w * h * 4);
    renderer.readRenderTargetPixels(rt, 0, 0, w, h, px);
    rt.dispose();
    const c = el('canvas', { width: w, height: h });
    const g = c.getContext('2d');
    const img = g.createImageData(w, h);
    for (let y = 0; y < h; y++) {
      const src = (h - 1 - y) * w * 4;
      img.data.set(px.subarray(src, src + w * 4), y * w * 4);
      for (let x = 0; x < w; x++) img.data[y * w * 4 + x * 4 + 3] = 255;
    }
    g.putImageData(img, 0, 0);
    return c;
  }

  function gpuPick(points) {
    if (!pickRoot) buildPickRoot();
    const { rt, h } = renderToTarget(pickRoot, true);
    const px = new Uint8Array(4);
    const out = points.map(([x, y]) => {
      renderer.readRenderTargetPixels(rt, Math.round(x), h - 1 - Math.round(y), 1, 1, px);
      if (px[0] === 0) return null;
      const draw = px[0] - 1, d = scene.draws[draw];
      return { draw, name: d.name, mesh: d.mesh, tex: d.tex, kind: d.kind,
               tri: (px[1] << 16) | (px[2] << 8) | px[3] };
    });
    rt.dispose();
    return out;
  }

  // ---- CPU ray cast over every draw (no alpha test: cut-out texels still count as hits)
  function rayFor(x, y) {
    const { x: w, y: h } = bufferSize();
    const ndc = new THREE.Vector3(x / w * 2 - 1, 1 - y / h * 2, -1);
    const o = camera.position.clone();
    const p = ndc.unproject(camera);
    return { o, d: p.sub(o).normalize() };
  }

  function rayCast(o, d, maxHits = 8) {
    const hits = [];
    const e1 = new THREE.Vector3(), e2 = new THREE.Vector3(), pv = new THREE.Vector3(),
          tv = new THREE.Vector3(), qv = new THREE.Vector3(), a = new THREE.Vector3();
    scene.draws.forEach((dr, di) => {
      const f = ctx.attrs[dr.mesh];
      if (!f) return;                           // draw not loaded on this page
      const t0 = dr.first / 3, t1 = (dr.first + dr.count) / 3;
      for (let t = t0; t < t1; t++) {
        const b = t * 36;
        a.set(f[b], f[b + 1], f[b + 2]);
        e1.set(f[b + 12] - a.x, f[b + 13] - a.y, f[b + 14] - a.z);
        e2.set(f[b + 24] - a.x, f[b + 25] - a.y, f[b + 26] - a.z);
        pv.crossVectors(d, e2);
        const det = e1.dot(pv);
        if (Math.abs(det) < 1e-9) continue;
        const inv = 1 / det;
        tv.subVectors(o, a);
        const u = tv.dot(pv) * inv;
        if (u < 0 || u > 1) continue;
        qv.crossVectors(tv, e1);
        const v = d.dot(qv) * inv;
        if (v < 0 || u + v > 1) continue;
        const dist = e2.dot(qv) * inv;
        if (dist <= camera.near) continue;
        const w0 = 1 - u - v, at = k => w0 * f[b + k] + u * f[b + 12 + k] + v * f[b + 24 + k];
        const uvx = at(6), uvy = at(7);
        const tx = dr.tex ? scene.textures[dr.tex] : null;
        hits.push({
          draw: di, name: dr.name, tex: dr.tex, aref: dr.aref, tri: t, dist: +dist.toFixed(4),
          uv: [+uvx.toFixed(4), +uvy.toFixed(4)],
          texel: tx ? [Math.floor(((uvx % 1) + 1) % 1 * tx.w), Math.floor(((uvy % 1) + 1) % 1 * tx.h)] : null,
          vcol: [0, 1, 2, 3].map(k => Math.round(at(8 + k) * 255)),
          normal: (() => { const n = new THREE.Vector3().crossVectors(e1, e2).normalize();
                           return [+n.x.toFixed(4), +n.y.toFixed(4), +n.z.toFixed(4)]; })(),
        });
      }
    });
    hits.sort((p, q) => p.dist - q.dist);
    const first = hits.length ? hits[0].dist : 0;
    // depth-buffer step at that distance (24-bit, this near/far): coplanar if closer than a few steps
    const zstep = first * first / (camera.near * 16777216) * (1 - camera.near / camera.far);
    for (const h of hits) h.gap = +(h.dist - first).toFixed(4);
    const coplanar = hits.filter(h => h.gap <= Math.max(8 * zstep, 0.05));
    return { hits: hits.slice(0, maxHits), nHits: hits.length, zstep: +zstep.toPrecision(3),
             coplanar: coplanar.length > 1 ? coplanar.map(h => `${h.name}#${h.tri}`) : [] };
  }

  // ---- UI
  const overlay = el('div', { id: 'rep-overlay' });
  const box = el('div', { id: 'rep-box' });
  const form = el('div', { id: 'rep-form' });
  const hint = el('div', { id: 'rep-hint' }, 'Drag a rectangle over the problem.  Esc cancels.');
  const note = el('textarea', { rows: 3, placeholder: 'note (optional): what looks wrong?' });
  const send = el('button', { type: 'button' }, 'Submit report');
  const cancel = el('button', { type: 'button' }, 'Cancel');
  const redo = el('button', { type: 'button' }, 'Redraw');
  const status = el('div', { id: 'rep-status' });
  form.append(note, el('div', { className: 'row' }), status);
  form.querySelector('.row').append(send, redo, cancel);
  overlay.append(box, hint, form);
  document.body.append(overlay);

  let active = false, start = null, rect = null, busy = false;

  function open() {
    if (active) return;
    if (document.pointerLockElement) document.exitPointerLock();
    active = true; rect = null; start = null;
    ctx.setFrozen(true);
    overlay.style.display = 'block'; box.style.display = 'none'; form.style.display = 'none';
    hint.style.display = 'block'; status.textContent = ''; note.value = '';
  }
  function close() {
    active = false; overlay.style.display = 'none';
    ctx.setFrozen(false);
  }
  ctx.openReport = open;

  function cssRect(a, b) {
    return { x: Math.min(a[0], b[0]), y: Math.min(a[1], b[1]),
             w: Math.abs(a[0] - b[0]), h: Math.abs(a[1] - b[1]) };
  }
  function drawBox(r) {
    Object.assign(box.style, { display: 'block', left: r.x + 'px', top: r.y + 'px',
                               width: r.w + 'px', height: r.h + 'px' });
  }
  overlay.addEventListener('pointerdown', e => {
    if (e.target !== overlay && e.target !== box) return;
    start = [e.clientX, e.clientY]; rect = null;
    form.style.display = 'none'; hint.style.display = 'none';
    overlay.setPointerCapture(e.pointerId);
  });
  overlay.addEventListener('pointermove', e => {
    if (start) drawBox(cssRect(start, [e.clientX, e.clientY]));
  });
  overlay.addEventListener('pointerup', e => {
    if (!start) return;
    let r = cssRect(start, [e.clientX, e.clientY]);
    start = null;
    if (r.w < 4 || r.h < 4) r = { x: e.clientX - 20, y: e.clientY - 20, w: 40, h: 40 };  // a click = small box
    rect = r; drawBox(r);
    const fx = Math.min(r.x, innerWidth - 340), fy = r.y + r.h + 140 < innerHeight ? r.y + r.h + 8 : Math.max(r.y - 130, 8);
    Object.assign(form.style, { display: 'block', left: Math.max(fx, 8) + 'px', top: fy + 'px' });
    note.focus();
  });
  redo.onclick = () => { rect = null; box.style.display = 'none'; form.style.display = 'none'; hint.style.display = 'block'; };
  cancel.onclick = close;
  addEventListener('keydown', e => {
    if (e.code === 'Escape' && active && !busy) { close(); e.preventDefault(); }
    if (e.code === 'Enter' && active && rect && (e.metaKey || e.ctrlKey)) send.click();
  });

  function toBlobB64(c) {
    return new Promise((ok, bad) => c.toBlob(b => {
      if (!b) return bad(new Error('PNG encode failed'));
      const fr = new FileReader();
      fr.onload = () => ok(String(fr.result).split(',')[1]);
      fr.onerror = () => bad(fr.error);
      fr.readAsDataURL(b);
    }, 'image/png'));
  }

  send.onclick = async () => {
    if (!rect || busy) return;
    busy = true; send.disabled = true; status.textContent = 'capturing…';
    try {
      const sx = canvas.width / canvas.clientWidth, sy = canvas.height / canvas.clientHeight;
      const cr = canvas.getBoundingClientRect();
      const px = { x: Math.round((rect.x - cr.left) * sx), y: Math.round((rect.y - cr.top) * sy),
                   w: Math.round(rect.w * sx), h: Math.round(rect.h * sy) };
      const W = canvas.width, H = canvas.height;
      const norm = { x: +(px.x / W).toFixed(5), y: +(px.y / H).toFixed(5),
                     w: +(px.w / W).toFixed(5), h: +(px.h / H).toFixed(5) };

      const plain = snapshot();
      const marked = el('canvas', { width: W, height: H });
      const g = marked.getContext('2d');
      g.drawImage(plain, 0, 0);
      g.lineWidth = Math.max(2, Math.round(2 * sx));
      g.strokeStyle = '#ff2d55'; g.strokeRect(px.x, px.y, px.w, px.h);

      // centre first, then a 3x3 grid inside the rectangle
      const pts = [[px.x + px.w / 2, px.y + px.h / 2]];
      for (const fy of [1 / 6, 1 / 2, 5 / 6]) for (const fx of [1 / 6, 1 / 2, 5 / 6])
        if (!(fx === 1 / 2 && fy === 1 / 2)) pts.push([px.x + px.w * fx, px.y + px.h * fy]);
      const clampPt = ([x, y]) => [Math.min(Math.max(x, 0), W - 1), Math.min(Math.max(y, 0), H - 1)];
      const ptsC = pts.map(clampPt);
      const gpu = gpuPick(ptsC);
      const picks = ptsC.map((p, i) => {
        const { o, d } = rayFor(p[0] + 0.5, p[1] + 0.5);
        return { px: p.map(v => Math.round(v)), norm: [+(p[0] / W).toFixed(4), +(p[1] / H).toFixed(4)],
                 gpu: gpu[i], ray: rayCast(o, d) };
      });

      const gl = renderer.getContext();
      const dbg = gl.getExtension('WEBGL_debug_renderer_info');
      const meta = {
        version: 1,
        time: new Date().toISOString(),
        url: location.href,
        note: note.value.slice(0, 4000),
        ua: navigator.userAgent,
        camera: ctx.view(),
        rect: { px, norm },
        canvas: { w: W, h: H, css_w: canvas.clientWidth, css_h: canvas.clientHeight,
                  dpr: devicePixelRatio, pixel_ratio: renderer.getPixelRatio() },
        webgl: { renderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
                 vendor: dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
                 depth_bits: gl.getParameter(gl.DEPTH_BITS), max_texture: gl.getParameter(gl.MAX_TEXTURE_SIZE),
                 antialias: gl.getContextAttributes().antialias },
        picks,
        ...(ctx.extraMeta ? ctx.extraMeta() : {}),
      };
      status.textContent = 'uploading…';
      const body = JSON.stringify({ meta, marked_png: await toBlobB64(marked), plain_png: await toBlobB64(plain) });
      const r = await fetch('api/report', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body });
      const j = await r.json().catch(() => ({}));
      if (!r.ok || !j.id) throw new Error(j.error || `HTTP ${r.status}`);
      close();
      ctx.toast(`Report saved: ${j.id}`, 8000);
      window.__lastReport = { id: j.id, meta };
    } catch (err) {
      status.textContent = 'failed: ' + err.message;
      console.error(err);
    } finally {
      busy = false; send.disabled = false;
    }
  };

  // exposed for headless checks. __idBuffer: draw+1 in bits 24-31, triangle in 0-23,
  // with an optional near-plane scale and reversed draw order (z-fight detector).
  window.__idBuffer = ({ nearScale = 1, reverse = false } = {}) => {
    if (!pickRoot) buildPickRoot();
    const n0 = camera.near;
    camera.near = n0 * nearScale; camera.updateProjectionMatrix();
    if (reverse) pickRoot.children.reverse();
    const { rt, w, h } = renderToTarget(pickRoot, true);
    if (reverse) pickRoot.children.reverse();
    camera.near = n0; camera.updateProjectionMatrix();
    const px = new Uint8Array(w * h * 4);
    renderer.readRenderTargetPixels(rt, 0, 0, w, h, px);
    rt.dispose();
    const ids = new Uint32Array(w * h);
    for (let i = 0; i < w * h; i++)
      ids[i] = ((px[4 * i] << 24) | (px[4 * i + 1] << 16) | (px[4 * i + 2] << 8) | px[4 * i + 3]) >>> 0;
    return { ids, w, h };   // rows bottom-first (GL order)
  };
  window.__colorBuffer = ({ nearScale = 1 } = {}) => {
    const n0 = camera.near;
    camera.near = n0 * nearScale; camera.updateProjectionMatrix();
    const { rt, w, h } = renderToTarget(root, false);
    camera.near = n0; camera.updateProjectionMatrix();
    const px = new Uint8Array(w * h * 4);
    renderer.readRenderTargetPixels(rt, 0, 0, w, h, px);
    rt.dispose();
    return { px, w, h };
  };
  window.__pick = (x, y) => ({ gpu: gpuPick([[x, y]])[0], ray: rayCast(...Object.values(rayFor(x + .5, y + .5))) });
  return { open, close, isActive: () => active };
}
