// PS2 display output stage (PCRTC merge), as the game sets it every frame.
//
// FUN_0040ce28 (the per-frame flip) calls FUN_0040d228(env, buf, 1, 1), which writes:
//   PMODE    = env.pmode | 0x8021      -> EN1=EN2=1, MMOD=1 (fixed alpha), ALP=0x80, SLBG=0
//   DISPFB1  = DISPFB2 = buf.dispfb    -> both read circuits read the same framebuffer
//   DISPLAY1 = DISPLAY2 - ((1 << 12) | 1)
// so circuit 2 sits one raster line lower (DY+1) and one VCK to the right (DX+1) of
// circuit 1. All GS dumps agree (DISPLAY1 DX 655 DY 71, DISPLAY2 DX 656 DY 72,
// MAGH 5 (6 VCK per pixel), MAGV 0, DW 2552, DH 511 = 512 lines, SMODE2 INT=1 FFMD=0:
// interlaced frame mode, so DY counts framebuffer rows). The merge circuit outputs
//   out = ALP * C1 + (1 - ALP) * C2,  ALP = 0x80 / 255
// with C1 = fb(x, y) and C2 = fb(x - 1 VCK, y - 1 row); BGCOLOR (0) where circuit 2
// has not started (the top row, the first VCK).
//
// Native mode (default): the game's own resolution.
//   Scene: drawn into FRAME FBP 224 (FBW 448, PSM CT32) with SCISSOR 0-431 x 0-511 and
//   XYOFFSET (1824, 1792) in every gameplay draw of every dump: a 432 x 512 target, the
//   same every frame (frame mode: both fields come from the one 512-line frame).
//   Projection: FUN_0040d478 builds the view->screen matrix with FUN_00263c70
//   (sceVu0ViewScreenMatrix: sx = scrz*ax*X/Z + cx, sy = scrz*ay*Y/Z + cy) from the
//   display struct 0x4fc7d0: scrz = +0x304, ax = 1, ay = +0x2f8, cx = 2048 + (+0x320),
//   cy = 2048 + (+0x324). scrz = (+0x2e8) * 0.5 / tan(FOV/2) (FUN_0040ce28, FUN_0040d670).
//   sea-of-flames.p2s RAM: +0x2e8 = 448, +0x300 FOV = 73.5, +0x2f8 = 1.5, +0x320 = -11,
//   +0x324 = 0 -> scrz 299.972, centre at drawable (213, 256). The fitted hero camera
//   (fit_dump_camera.py: 299.971 / 449.957 px, centre 213.0) agrees.
//   GUESS: GS pixel sample point = GL's (pixel centre); the projected centre 213, 256 is put
//   at GL window 213, 256. Not decoded on hardware; matches the GS-dump oracle (gsrender2
//   samples at +0.5) to 0.1 px, where a +0.5 shift left a 0.5 px offset. See fidelity audit.
//   Copy: the frame is copied FBP 224 -> FBP 112 (CT24) as 32-px sprites, X 1:1, which
//   DISPFB (FBP 112, DBX 0, DBY 0) shows. Ported as the same pixels (see GUESS below).
//   Merge: at VCK resolution, (DW + 1) = 2553 VCK x 512 lines, C1 = col floor(k / 6),
//   C2 = col floor((k - 1) / 6) one row up: exact, no fractional offsets.
//   Display: the 2553 x 512 window is shown 4:3 (what PCSX2 and a TV do with it),
//   letterboxed/pillarboxed into the canvas.
// Full mode (the default, Dan's choice 2026-10-28; `?res=native` gives native mode): today's sharp render at
//   canvas resolution, the camera's aspect filling the whole canvas, and the merge with
//   fractional offsets (one line = H / 512 canvas px, one VCK = (4/3 H / 425.5) / 6 px).
// The circuit-2 mix runs by default only in native mode; `?outblur=1` / `0` forces it on / off.
import * as THREE from 'three';

const ALP = 0x80 / 255;          // PMODE.ALP, FUN_0040d228 (| 0x8021)
const LINES = 512;               // DISPLAY.DH + 1
const VCKS = 2552 + 1;           // DISPLAY.DW + 1
const MAGH = 5 + 1;              // VCK per framebuffer pixel
const COLS = VCKS / MAGH;        // 425.5 framebuffer columns shown
const VCK = 1 / MAGH;            // circuit 2 DX + 1, in framebuffer pixels
export const FB_W = 432, FB_H = 512;            // scene SCISSOR (FRAME FBP 224)
const SCR_W = 448, FOV = 73.5, AY = 1.5;        // 0x4fc7d0 +0x2e8, +0x300, +0x2f8
const CX = 2048 - 11 - 1824, CY = 2048 + 0 - 1792;   // +0x320/+0x324 less XYOFFSET -> 213, 256
const SCRZ = SCR_W * 0.5 / Math.tan(FOV * 0.008726647);   // FUN_0040d670
const DISPLAY_ASPECT = 4 / 3;    // PAL 4:3 (the game has no 16:9 output; see fidelity audit)

const VS = `in vec3 position; out vec2 uv;
  void main() { uv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`;

function quadPass(fragmentShader, uniforms) {
  const mat = new THREE.RawShaderMaterial({ glslVersion: THREE.GLSL3, uniforms, vertexShader: VS, fragmentShader,
                                            depthTest: false, depthWrite: false });
  const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), mat);
  quad.frustumCulled = false;
  const scene = new THREE.Scene(); scene.add(quad);
  return { mat, scene };
}

export function createOutputStage(renderer, { enabled = true, msaa = false, native = true } = {}) {
  const qCam = new THREE.Camera();
  const size = new THREE.Vector2();

  // ---- full mode (improvement option): canvas-size target, fractional merge offsets
  const rt = new THREE.WebGLRenderTarget(1, 1, {
    depthBuffer: true, type: THREE.UnsignedByteType, samples: msaa ? 4 : 0,
    minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter,
  });
  const full = quadPass(`precision highp float; in vec2 uv; out vec4 o;
      uniform sampler2D src; uniform vec2 off; uniform float alp;
      void main() {
        vec3 c1 = texture(src, uv).rgb;
        vec2 u2 = uv + vec2(-off.x, off.y);            // GL v is up: the row above is +v
        vec3 c2 = (u2.x >= 0.0 && u2.y <= 1.0) ? texture(src, u2).rgb : vec3(0.0);   // BGCOLOR
        o = vec4(alp * c1 + (1.0 - alp) * c2, 1.0);
      }`, { src: { value: rt.texture }, off: { value: new THREE.Vector2() }, alp: { value: ALP } });

  // ---- native mode: 432x512 scene -> 2553x512 VCK merge -> 4:3 box
  const fbRT = new THREE.WebGLRenderTarget(FB_W, FB_H, {
    depthBuffer: true, type: THREE.UnsignedByteType, samples: msaa ? 4 : 0,
    minFilter: THREE.NearestFilter, magFilter: THREE.NearestFilter,
  });
  // GUESS: last scale, the analog picture to canvas pixels, is linear (bilinear) filtered.
  // Not decodable: the PCRTC output is an analog signal and the TV reconstructs it
  // continuously along a line (no pixel edges); the lines themselves are separate, but
  // 512 lines into ~1900 px with nearest would give rows of uneven height. Linear also
  // matches PCSX2's default display filter. See notes/fidelity-audit.md.
  const vckRT = new THREE.WebGLRenderTarget(VCKS, LINES, {
    depthBuffer: false, type: THREE.UnsignedByteType,
    minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter,
  });
  const merge = quadPass(`precision highp float; in vec2 uv; out vec4 o;
      uniform sampler2D src; uniform float alp, c2on;
      void main() {
        int k = int(gl_FragCoord.x);                   // VCK from the window start
        int line = ${LINES - 1} - int(gl_FragCoord.y); // display line, top = 0
        vec3 c1 = texelFetch(src, ivec2(k / ${MAGH}, ${FB_H - 1} - line), 0).rgb;
        vec3 c2 = vec3(0.0);                           // BGCOLOR before circuit 2 starts
        if (k >= 1 && line >= 1) c2 = texelFetch(src, ivec2((k - 1) / ${MAGH}, ${FB_H} - line), 0).rgb;
        if (c2on < 0.5) { o = vec4(c1, 1.0); return; }
        o = vec4(alp * c1 + (1.0 - alp) * c2, 1.0);
      }`, { src: { value: fbRT.texture }, alp: { value: ALP }, c2on: { value: 1 } });
  const present = quadPass(`precision highp float; in vec2 uv; out vec4 o;
      uniform sampler2D src;
      void main() { o = vec4(texture(src, uv).rgb, 1.0); }`, { src: { value: vckRT.texture } });

  // 4:3 box (letterbox / pillarbox) inside a w x h target, GL origin bottom-left
  function box(w, h) {
    let bw = w, bh = Math.round(w / DISPLAY_ASPECT);
    if (bh > h) { bh = h; bw = Math.round(h * DISPLAY_ASPECT); }
    return { x: Math.floor((w - bw) / 2), y: Math.floor((h - bh) / 2), w: bw, h: bh };
  }
  function targetSize(target) {
    if (target) size.set(target.width, target.height); else renderer.getDrawingBufferSize(size);
    return size;
  }

  const stage = {
    enabled,
    native,
    framebuffer: fbRT,                                 // the 432x512 scene target (headless checks)
    // set the camera's projection for this mode (call at start and on resize)
    fitCamera(camera) {
      if (camera.userData.fov0 === undefined) camera.userData.fov0 = camera.fov;
      if (stage.native) {
        // centre (213, 256) inside a 432 x 512 view: a symmetric frustum of 438 x 512 px
        // (centre 219, 256) cropped to x 6..438
        const hw = Math.max(CX, FB_W - CX), hh = Math.max(CY, FB_H - CY);
        camera.fov = 2 * Math.atan(hh / (SCRZ * AY)) * 180 / Math.PI;
        camera.setViewOffset(2 * hw, 2 * hh, hw - CX, hh - CY, FB_W, FB_H);
        camera.aspect = (2 * hw / SCRZ) / (2 * hh / (SCRZ * AY));   // after: setViewOffset sets aspect = fullW / fullH
      } else {
        // improvement option: today's behaviour, the hero fov with the canvas's aspect
        camera.fov = camera.userData.fov0;
        camera.clearViewOffset();
        renderer.getDrawingBufferSize(size);
        camera.aspect = size.x / size.y;
      }
      camera.updateProjectionMatrix();
    },
    // pick buffer: the size ID renders use, and canvas px (top-first) -> pick px (null = off picture)
    pickSize(w, h) { return stage.native ? { w: FB_W, h: FB_H } : { w, h }; },
    toPick(x, y, w, h) {
      if (!stage.native) return { x, y };
      const b = box(w, h), top = h - b.y - b.h;
      const fx = (x - b.x) / b.w * COLS, fy = (y - top) / b.h * LINES;
      if (fx < 0 || fy < 0 || fx >= COLS || fy >= LINES) return null;
      return { x: fx, y: fy };
    },
    info() {
      return stage.native
        ? { mode: 'native', fb: [FB_W, FB_H], vck: [VCKS, LINES], shown_cols: COLS, aspect: '4:3',
            scrz: +SCRZ.toFixed(3), ay: AY, centre: [CX, CY], merge: stage.enabled }
        : { mode: 'full', merge: stage.enabled };
    },
    // render `scene` through the output stage into `target` (null = the canvas)
    render(scene, camera, target = null) {
      const s = targetSize(target);
      if (stage.native) {
        const autoClear = renderer.autoClear;
        renderer.autoClear = false;                    // the passes clear explicitly (else the box's
        renderer.setRenderTarget(fbRT);                // draw would re-clear the bars to the scene colour)
        renderer.clear();
        renderer.render(scene, camera);
        merge.mat.uniforms.c2on.value = stage.enabled ? 1 : 0;
        renderer.setRenderTarget(vckRT);
        renderer.render(merge.scene, qCam);
        const b = box(s.x, s.y);
        renderer.setRenderTarget(target);
        const cc = renderer.getClearColor(new THREE.Color()), ca = renderer.getClearAlpha();
        renderer.setClearColor(0x000000, 1);           // outside the display window: black
        renderer.clear(true, true, false);
        renderer.setClearColor(cc, ca);
        // viewport: target.viewport (target px, read by setRenderTarget) or the canvas's (CSS px)
        if (target) { target.viewport.set(b.x, b.y, b.w, b.h); renderer.setRenderTarget(target); }
        else { const k = 1 / renderer.getPixelRatio(); renderer.setViewport(b.x * k, b.y * k, b.w * k, b.h * k); }
        renderer.render(present.scene, qCam);
        if (target) target.viewport.set(0, 0, s.x, s.y);
        else { const k = 1 / renderer.getPixelRatio(); renderer.setViewport(0, 0, s.x * k, s.y * k); }
        renderer.setRenderTarget(null);
        renderer.autoClear = autoClear;
        return;
      }
      if (!stage.enabled) { renderer.setRenderTarget(target); renderer.render(scene, camera); renderer.setRenderTarget(null); return; }
      if (rt.width !== s.x || rt.height !== s.y) rt.setSize(s.x, s.y);
      const line = s.y / LINES;                        // one PS2 line, canvas pixels
      const vck = (4 / 3) * s.y / COLS * VCK;          // one VCK, canvas pixels
      full.mat.uniforms.off.value.set(vck / s.x, line / s.y);
      renderer.setRenderTarget(rt);
      renderer.clear();
      renderer.render(scene, camera);
      renderer.setRenderTarget(target);
      renderer.render(full.scene, qCam);
      renderer.setRenderTarget(null);
    },
  };
  return stage;
}
