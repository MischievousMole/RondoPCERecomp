#version 330 core
// Santable.ai idle screensaver: navy globe, cream constellation tree, Quicksand wordmark, slow drift.
uniform vec2 uRes;            // framebuffer size
uniform float uTime;          // seconds since the screensaver started
uniform sampler2D uWord;      // wordmark alpha (R channel)
uniform vec2 uWordSize;
out vec4 oColor;

const vec3 NAVY  = vec3(0x0e, 0x1c, 0x35) / 255.0;
const vec3 BLUE  = vec3(0x5b, 0x9b, 0xd5) / 255.0;
const vec3 DEEP  = vec3(0x2c, 0x4a, 0x6e) / 255.0;
const vec3 CREAM = vec3(0xf3, 0xea, 0xd3) / 255.0;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float seg(vec2 p, vec2 a, vec2 b) { vec2 pa = p - a, ba = b - a; float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0); return length(pa - ba * h); }

// tree constellation in globe space (unit = globe radius, y up)
const int NP = 15;
const vec2 P[NP] = vec2[NP](
    vec2(0.0, 0.62),
    vec2(-0.20, 0.36), vec2(-0.09, 0.38), vec2(0.09, 0.38), vec2(0.20, 0.36),
    vec2(-0.32, 0.10), vec2(-0.13, 0.12), vec2(0.13, 0.12), vec2(0.32, 0.10),
    vec2(-0.44, -0.18), vec2(0.44, -0.18),
    vec2(-0.06, -0.18), vec2(0.06, -0.18), vec2(-0.06, -0.32), vec2(0.06, -0.32));
const int NS = 14;
const ivec2 S[NS] = ivec2[NS](
    ivec2(0, 1), ivec2(1, 2), ivec2(2, 5), ivec2(5, 6), ivec2(6, 9), ivec2(9, 10),
    ivec2(10, 7), ivec2(7, 8), ivec2(8, 3), ivec2(3, 4), ivec2(4, 0),
    ivec2(11, 13), ivec2(13, 14), ivec2(14, 12));

void main() {
    vec2 fc = vec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y);
    float u = uRes.y;                                  // scale unit: screen height
    // background: navy with a soft vignette
    vec2 q = fc / uRes - 0.5;
    vec3 col = NAVY * (1.0 - 0.55 * dot(q, q) * 2.0);
    // sparse twinkling stars
    vec2 cell = floor(fc / (u * 0.025));
    float h = hash(cell);
    if (h > 0.965) {
        vec2 sp = (cell + vec2(hash(cell + 3.1), hash(cell + 7.7))) * (u * 0.025);
        float d = length(fc - sp) / u;
        float tw = 0.5 + 0.5 * sin(uTime * (0.6 + 2.0 * hash(cell + 1.3)) + h * 40.0);
        col += CREAM * smoothstep(0.0022, 0.0, d) * (0.25 + 0.55 * tw);
    }
    // drift: slow lissajous path, keeps the logo moving for burn-in protection
    float R = 0.27 * u;
    vec2 span = vec2(uRes.x * 0.5 - R * 1.6, uRes.y * 0.5 - R * 1.55);
    vec2 C = uRes * 0.5 + vec2(sin(uTime * 0.031), sin(uTime * 0.023 + 1.3)) * max(span, vec2(0.0)) * vec2(1.0, 0.55) - vec2(0.0, R * 0.25);
    vec2 g = (fc - C) / R; g.y = -g.y;
    float r = length(g);
    // globe body, meridians, rim
    if (r < 1.0) {
        vec3 body = mix(NAVY * 1.25, NAVY * 0.85, smoothstep(0.0, 1.0, r));
        float z = sqrt(max(0.0, 1.0 - r * r));
        float lon = atan(g.x, z) + uTime * 0.05;
        float lat = asin(clamp(g.y, -1.0, 1.0));
        float grid = max(smoothstep(0.035, 0.0, abs(fract(lon / 0.5236 + 0.5) - 0.5) * 0.5236 * z),
                         smoothstep(0.03, 0.0, abs(fract(lat / 0.5236 + 0.5) - 0.5) * 0.5236));
        body = mix(body, DEEP, grid * 0.55);
        col = body;
    }
    col += BLUE * smoothstep(0.06, 0.0, abs(r - 1.0)) * 0.55;          // rim
    col += BLUE * smoothstep(0.35, 0.0, r - 1.0) * step(1.0, r) * 0.10;  // outer glow
    // constellation tree
    if (r < 1.05) {
        float line = 1e3;
        for (int i = 0; i < NS; i++) line = min(line, seg(g, P[S[i].x], P[S[i].y]));
        col = mix(col, CREAM, smoothstep(0.012, 0.0, line) * 0.45);
        for (int i = 0; i < NP; i++) {
            float d = length(g - P[i]);
            float tw = 0.6 + 0.4 * sin(uTime * (1.1 + 0.37 * float(i)) + float(i) * 2.1);
            float sz = i == 0 ? 0.05 : 0.026;
            col += CREAM * (smoothstep(sz, 0.0, d) * tw + smoothstep(sz * 3.5, 0.0, d) * 0.18 * tw);
            if (i == 0) {   // four-point sparkle on the top star
                vec2 a = abs(g - P[0]);
                col += CREAM * (smoothstep(0.012, 0.0, a.y) * smoothstep(0.16, 0.0, a.x) + smoothstep(0.012, 0.0, a.x) * smoothstep(0.16, 0.0, a.y)) * 0.6 * tw;
            }
        }
    }
    // wordmark under the globe
    float ww = R * 2.6, wh = ww * uWordSize.y / uWordSize.x;
    vec2 wp = (fc - (C + vec2(-ww * 0.5, R * 1.18))) / vec2(ww, wh);
    if (wp.x >= 0.0 && wp.x <= 1.0 && wp.y >= 0.0 && wp.y <= 1.0) {
        float a = texture(uWord, wp).r;
        col = mix(col, CREAM, a * 0.95);
    }
    float fade = smoothstep(0.0, 2.0, uTime);
    oColor = vec4(col * fade, 1.0);
}
