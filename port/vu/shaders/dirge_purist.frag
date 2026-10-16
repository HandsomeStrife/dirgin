#version 330 core
// dirge_purist.frag -- reproduces the GS per-pixel combine for Dirge of Cerberus.
// Evidence: ../RENDER-MATH.md (GIF format §3, UV §4, colour §2, fog §5).
//
// The GS primitive emitted by VU1 is textured + Gouraud with per-vertex
// RGBAQ. The classic PS2 TEX0.TFX=MODULATE combine is  frag = tex * vcol * 2
// for 8-bit colour (the "×2" / 128-is-1.0 convention). Expose TFX as a uniform
// so the loader can match each material's GIFtag.

in vec2  vUV;
in vec4  vColor;      // vertex colour, already lit in the vertex stage (0..1)
in float vFog;        // 1 = unfogged, 0 = fully fogged

uniform sampler2D uTex;
uniform int   uTexEnable;    // 0 = untextured (vertex colour only)
uniform int   uTFX;          // 0 MODULATE, 1 DECAL, 2 HIGHLIGHT, 3 HIGHLIGHT2 (GS TEX0.TFX)
uniform vec3  uFogColor;     // GS FOG register colour
uniform float uAlphaRef;     // alpha-test threshold (GS TEST.AREF/255), <0 disables

out vec4 fragColor;

void main()
{
    vec4 c = vColor;

    if (uTexEnable == 1) {
        vec4 t = texture(uTex, vUV);
        // PS2 MODULATE: Cf = Ct * Cv, with the 8-bit "128 = 1.0" doubling.
        // (GS computes (Ct * Cv) >> 7.)  Clamp like the GS does.
        if (uTFX == 0) {               // MODULATE
            c.rgb = min(t.rgb * vColor.rgb * 2.0, 1.0);
            c.a   = min(t.a   * vColor.a   * 2.0, 1.0);
        } else if (uTFX == 1) {        // DECAL: texel replaces, alpha from texel
            c.rgb = t.rgb;
            c.a   = t.a;
        } else {                       // HIGHLIGHT(2): modulate rgb, keep/!add vcol alpha
            c.rgb = min(t.rgb * vColor.rgb * 2.0 + vColor.a, 1.0);
            c.a   = (uTFX == 2) ? t.a : vColor.a;
        }
    }

    // GS alpha test (asset-authored via GIFtag A+D TEST register).
    if (uAlphaRef >= 0.0 && c.a < uAlphaRef) discard;

    // Fog: GS blends toward FOG colour by (1-F).  [VU packs F per vertex, §5]
    c.rgb = mix(uFogColor, c.rgb, vFog);

    fragColor = c;
}
</content>
