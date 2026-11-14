#version 330 core
// Rondo recomp output shader: PC Engine frame -> display resolution in one pass.
// Matches harness/video.c (CPU reference):
//  - per-layer brightness/contrast (alpha channel = layer: 0 backdrop/BG, 1 sprite), gamma space
//  - filters: 0 PIXEL (nearest), 1 SHARP (sharp bilinear), 2 SCANLINES, 3 CRT (scanlines + aperture grille)
//  - beams computed in linear light; spot width grows with brightness
// The picture rectangle (uPicOrigin/uPicSize, in framebuffer pixels, top-left origin) is set by the host;
// everything outside it is the black border.

uniform sampler2D uSrc;       // RGBA8, texelFetch only (no filtering)
uniform vec2  uSrcSize;       // source width/height (e.g. 256x224)
uniform vec2  uPicOrigin;     // picture top-left in framebuffer pixels
uniform vec2  uPicSize;       // picture size in framebuffer pixels
uniform float uFbHeight;      // framebuffer height (gl_FragCoord is bottom-left)
uniform int   uFilter;
uniform vec2  uBri;           // x = backdrop/BG, y = sprites
uniform vec2  uCon;
uniform float uScan;          // scanline strength 0..1
uniform float uMask;          // aperture grille strength 0..1

out vec4 oColor;

vec3 fetchLin(int x, int y) {
    ivec2 p = clamp(ivec2(x, y), ivec2(0), ivec2(uSrcSize) - 1);
    vec4 t = texelFetch(uSrc, p, 0);
    bool spr = t.a > 0.5;
    float b = spr ? uBri.y : uBri.x, c = spr ? uCon.y : uCon.x;
    vec3 v = clamp(((t.rgb - 0.5) * c + 0.5) * b, 0.0, 1.0);
    // match the CPU path: adjust is quantised to 8 bits before linearising
    v = floor(v * 255.0 + 0.5) / 255.0;
    return pow(v, vec3(2.2));
}

// sharp-bilinear along x at source row `row`; `hs` = transition sharpness (output px per source px)
vec3 hsample(float u, int row, float hs) {
    float t = u - 0.5;
    float i0 = floor(t);
    float f = clamp((t - i0 - 0.5) * hs + 0.5, 0.0, 1.0);
    return mix(fetchLin(int(i0), row), fetchLin(int(i0) + 1, row), f);
}

void main() {
    vec2 fc = vec2(gl_FragCoord.x, uFbHeight - gl_FragCoord.y);      // top-left origin, pixel centres at .5
    vec2 p = fc - uPicOrigin;
    if (p.x < 0.0 || p.y < 0.0 || p.x >= uPicSize.x || p.y >= uPicSize.y) { oColor = vec4(0, 0, 0, 1); return; }
    float sx = uPicSize.x / uSrcSize.x, sy = uPicSize.y / uSrcSize.y;
    float u = p.x / sx, v = p.y / sy;
    vec3 o;
    if (uFilter == 0) {
        o = fetchLin(int(u), int(v));
    } else if (uFilter == 1) {
        float t = v - 0.5;
        float j0 = floor(t);
        float f = clamp((t - j0 - 0.5) * sy + 0.5, 0.0, 1.0);
        o = mix(hsample(u, int(j0), sx), hsample(u, int(j0) + 1, sx), f);
    } else {
        float hs = uFilter == 3 ? sx * 0.45 : sx;
        int j = int(floor(v));
        o = vec3(0.0);
        for (int k = 0; k < 3; k++) {
            int row = j - 1 + k;
            vec3 c = hsample(u, row, hs);
            float d = v - (float(row) + 0.5);
            vec3 sigma = 0.20 + 0.16 * clamp(c, 0.0, 1.0);
            vec3 beam = exp(-0.5 * (d / sigma) * (d / sigma)) / (sigma * 2.5066);
            vec3 plain = vec3(k == 1 ? 1.0 : 0.0);
            o += c * mix(plain, beam, uScan);
        }
        if (uFilter == 3) {
            float lo = 1.0 - 0.6 * uMask;
            float gain = 3.0 / (1.0 + 2.0 * lo);
            int ph = int(floor(fc.x)) % 3;
            vec3 m = vec3(ph == 0 ? 1.0 : lo, ph == 1 ? 1.0 : lo, ph == 2 ? 1.0 : lo);
            o *= m * gain;
        }
    }
    oColor = vec4(pow(clamp(o, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
