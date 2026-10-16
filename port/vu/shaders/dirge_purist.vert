#version 330 core
// dirge_purist.vert -- reproduces VU1 program A/B/D per-vertex math.
// Evidence: ../RENDER-MATH.md (lighting §1, colour §2, UV §4, fog §5).
//
// Vertex attributes are the DEQUANTISED model data (do the ITOF4/ITOF12 divides
// on the CPU/loader, or here -- shown here for clarity):
//   aPosRaw  : int16/int8 position   (world ÷16 = ITOF4 ; skinned ÷4096 = ITOF12)
//   aNormal  : normal (already in the space L is expressed in; skinned on VU0)
//   aUV      : int16 UV ÷4096
//   aColor   : per-vertex RGBA, 0..255  (ITOF0)
layout(location=0) in vec3  aPosRaw;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUVraw;
layout(location=3) in vec4  aColor;     // 0..255

uniform mat4  uMvp;            // vf01..vf04  (clip matrix)
uniform float uPosScale;       // 1/16 (ITOF4 world) or 1/4096 (ITOF12 skinned)
uniform float uUVScale;        // 1/4096 (ITOF12)
uniform vec2  uUVbias;         // vf10

// --- lighting (program D). For A/B set uNumLit=0 (pure vertex colour). ---
uniform int   uLit;            // 1 = program D path, 0 = A/B
uniform mat3  uLightDir;       // L : rows = 3 directional light dirs  (vf05,06,07)
uniform mat3  uLightCol;       // C : cols = 3 light RGB colours        (vf08,09,10)
uniform vec3  uAmbient;        // A : ambient RGB                       (vf11)
uniform float uSaturate;       // I : max channel value (e.g. 255.0)    (MINIi I)

// --- fog (program A/B) ---  F = clamp(a*w + b, fogmin, 1)
uniform float uFogA;           // from vf09
uniform float uFogB;
uniform float uFogMin;         // vf09.z

out vec2  vUV;
out vec4  vColor;              // 0..1, already lit if uLit
out float vFog;                // 1 = no fog, 0 = full fog

void main()
{
    vec3 pos = aPosRaw * uPosScale;
    vec4 clip = uMvp * vec4(pos, 1.0);

    // UV: ITOF12 + bias. Perspective-correct is handled by the rasteriser --
    // DO NOT pre-divide by w (the PS2 MULq-by-Q was only because the GS divides
    // ST by RGBAQ.Q at raster; a modern pipe interpolates s/w,t/w for free).
    vUV = aUVraw * uUVScale + uUVbias;

    vec3 col = aColor.rgb / 255.0;      // base vertex colour
    if (uLit == 1) {
        // d_i = max(Ldir_i . n, 0)   (3 directional lights)   [VU1-D idx 88-94]
        vec3 d   = max(uLightDir * normalize(aNormal), 0.0);
        // lit = C . d + ambient                               [VU1-D idx 98-102]
        vec3 lit = uLightCol * d + uAmbient;
        // rgb = saturate(lit (x) vertexColour)                [VU1-D idx 111-115]
        col = min(lit * col, vec3(uSaturate / 255.0));
    }
    vColor = vec4(col, aColor.a / 255.0);   // alpha passes through lighting

    // Fog factor from clip w (packed into XYZF2.F on PS2).    [VU1-A idx 94-104]
    vFog = clamp(uFogA * clip.w + uFogB, uFogMin, 1.0);

    gl_Position = clip;
}
</content>
