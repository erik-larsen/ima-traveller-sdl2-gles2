// =============================================================================
// IMA TRAVELLER
// =============================================================================
// Port of Ima Traveller from 1996:
// - https://notnot.home.xs4all.nl/expanding-structures/ima-traveller/ima-traveller.html
// - Decompiled to low-level C from Windows exe by Ghidra & Hexrays
// - Reversed to Python and ported to Shadertoy by Claude Opus 4.5
//
// To do:
// - Mouse-positioned focal point
// - Smoother appearance of new generations
//

// =============================================================================
// Image (Display with Zoom)
// =============================================================================
// This shader reads the CA state from Buffer A and applies smooth zoom.
// Zoom goes from 1.0x to 2.0x over ZOOM_FRAMES, then resets when CA evolves.

const int ZOOM_FRAMES = 30;  // Must match Buffer A

// Focus point (0.5, 0.5 = center) - can be made dynamic
const vec2 FOCUS = vec2(0.5);

// Set to true for pixelated look, false for smooth bilinear
const bool NEAREST = false;

vec3 hsv555ToRgb(int hsv) {
    int h = hsv & 0x1F;
    int s = (hsv >> 5) & 0x1F;
    int v = (hsv >> 10) & 0x1F;
    
    // Match Python color table formula
    float hue = float(h * 8) * 1.4117647;  // 0-350 degrees
    float sat = float(s * 8) / 255.0;
    float val = float(v * 8 + 7) / 255.0;
    
    if (sat == 0.0) {
        return vec3(val);
    }
    
    float hue_sector = hue / 60.0;
    int sector = int(hue_sector) % 6;
    float f = fract(hue_sector);
    
    float p = val * (1.0 - sat);
    float q = val * (1.0 - sat * f);
    float t = val * (1.0 - sat * (1.0 - f));
    
    vec3 rgb;
    if (sector == 0) rgb = vec3(val, t, p);
    else if (sector == 1) rgb = vec3(q, val, p);
    else if (sector == 2) rgb = vec3(p, val, t);
    else if (sector == 3) rgb = vec3(p, q, val);
    else if (sector == 4) rgb = vec3(t, p, val);
    else rgb = vec3(val, p, q);
    
    return clamp(rgb, 0.0, 1.0);
}

// Sample buffer with bilinear interpolation
vec3 sampleBilinear(vec2 uv, vec2 res) {
    vec2 texel = uv * res - 0.5;
    vec2 f = fract(texel);
    ivec2 i = ivec2(floor(texel));
    
    // Clamp coordinates (matching Python clamping behavior)
    ivec2 i00 = clamp(i + ivec2(0, 0), ivec2(0), ivec2(res) - 1);
    ivec2 i10 = clamp(i + ivec2(1, 0), ivec2(0), ivec2(res) - 1);
    ivec2 i01 = clamp(i + ivec2(0, 1), ivec2(0), ivec2(res) - 1);
    ivec2 i11 = clamp(i + ivec2(1, 1), ivec2(0), ivec2(res) - 1);
    
    vec3 c00 = hsv555ToRgb(int(texelFetch(iChannel0, i00, 0).r));
    vec3 c10 = hsv555ToRgb(int(texelFetch(iChannel0, i10, 0).r));
    vec3 c01 = hsv555ToRgb(int(texelFetch(iChannel0, i01, 0).r));
    vec3 c11 = hsv555ToRgb(int(texelFetch(iChannel0, i11, 0).r));
    
    vec3 c0 = mix(c00, c10, f.x);
    vec3 c1 = mix(c01, c11, f.x);
    return mix(c0, c1, f.y);
}

// Sample buffer with nearest neighbor
vec3 sampleNearest(vec2 uv, vec2 res) {
    ivec2 i = clamp(ivec2(uv * res), ivec2(0), ivec2(res) - 1);
    return hsv555ToRgb(int(texelFetch(iChannel0, i, 0).r));
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 res = iResolution.xy;
    vec2 uv = fragCoord / res;
    
    // Calculate zoom for current frame
    // Matches Python: t = z / args.zoom_frames; zoom = 2.0 ** t
    int zoomFrame = iFrame % ZOOM_FRAMES;
    float t = float(zoomFrame) / float(ZOOM_FRAMES);
    float zoom = pow(2.0, t);  // 1.0 -> 2.0 exponentially
    
    // Calculate crop region
    // At zoom 1.0: show full image
    // At zoom 2.0: show center quarter (each dimension halved)
    float cropSize = 1.0 / zoom;  // 1.0 -> 0.5
    
    // Center crop on focus point, clamped to valid region
    // Matches Python render_zoomed_frame logic
    vec2 cropMin = FOCUS - cropSize * 0.5;
    vec2 cropMax = FOCUS + cropSize * 0.5;
    
    // Clamp to keep crop within [0, 1]
    if (cropMin.x < 0.0) { cropMin.x = 0.0; cropMax.x = cropSize; }
    if (cropMin.y < 0.0) { cropMin.y = 0.0; cropMax.y = cropSize; }
    if (cropMax.x > 1.0) { cropMax.x = 1.0; cropMin.x = 1.0 - cropSize; }
    if (cropMax.y > 1.0) { cropMax.y = 1.0; cropMin.y = 1.0 - cropSize; }
    
    // Map screen UV to source UV within crop region
    vec2 srcUV = cropMin + uv * (cropMax - cropMin);
    
    // Sample the buffer
    vec3 rgb;
    if (NEAREST) {
        rgb = sampleNearest(srcUV, res);
    } else {
        rgb = sampleBilinear(srcUV, res);
    }
    
    fragColor = vec4(rgb, 1.0);
}
