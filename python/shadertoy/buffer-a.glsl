// =============================================================================
// Buffer A (CA State)
// =============================================================================
// This buffer stores the CA state in HSV555 format.
// Evolution only occurs at generation boundaries (every ZOOM_FRAMES frames).
// Between evolutions, the buffer passes through unchanged.

const int ZOOM_FRAMES = 30;  // Frames per zoom cycle (match with Image shader)

// Mutation table - exact 16x16 values from original
const int MUT[256] = int[256](
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    -1,-1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    -2,-1,-1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2,
    -2,-2,-1,-1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2,
    -2,-2,-2,-1,-1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2,
    -3,-2,-2,-1,-1,-1, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3,
    -3,-3,-2,-2,-1,-1,-1, 0, 0, 1, 1, 1, 2, 2, 3, 3,
    -4,-3,-3,-2,-1,-1,-1,-1, 1, 1, 1, 1, 2, 3, 3, 4,
    -4,-4,-3,-2,-2,-1,-1,-1, 1, 1, 1, 2, 2, 3, 4, 4,
    -5,-4,-3,-2,-2,-2,-1,-1, 1, 1, 2, 2, 2, 3, 4, 5,
    -5,-4,-3,-3,-2,-2,-1,-1, 1, 1, 2, 2, 3, 3, 4, 5,
    -6,-4,-3,-3,-2,-2,-1,-1, 1, 1, 2, 2, 3, 3, 4, 6,
    -6,-5,-4,-3,-2,-2,-1,-1, 1, 1, 2, 2, 3, 4, 5, 6,
    -7,-5,-4,-3,-2,-2,-1,-1, 1, 1, 2, 2, 3, 4, 5, 7,
    -7,-6,-5,-4,-3,-2,-2,-1, 1, 2, 2, 3, 4, 5, 6, 7
);

// Hash function (approximates sequential RNG)
uint uhash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint hash3(uint x, uint y, uint z) {
    return uhash(x + uhash(y + uhash(z)));
}

// Read HSV555 value with clamping (matches Python _get_neighbors)
int readHSV(ivec2 pos, ivec2 res) {
    pos = clamp(pos, ivec2(0), res - 1);
    return int(texelFetch(iChannel0, pos, 0).r);
}

// Select neighbor closest to center with tie-breaking chain (matches Python _select_neighbor)
// Chain comparison: >= means ties favor n2 over n1, then n3 over winner
int selectNeighbor(int center, int n1, int n2, int n3) {
    int d1 = abs(center - n1);
    int d2 = abs(center - n2);
    int winner = (d1 >= d2) ? n2 : n1;
    
    int dw = abs(center - winner);
    int d3 = abs(center - n3);
    return (dw >= d3) ? n3 : winner;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    ivec2 res = ivec2(iResolution.xy);
    ivec2 pos = ivec2(fragCoord);
    
    if (pos.x >= res.x || pos.y >= res.y) {
        fragColor = vec4(0.0);
        return;
    }
    
    int generation = iFrame / ZOOM_FRAMES;
    int zoomFrame = iFrame % ZOOM_FRAMES;
    
    // Detect resolution change by checking stored value in pixel (0,0)
    vec4 corner = texelFetch(iChannel0, ivec2(0, 0), 0);
    bool needsReset = (iFrame == 0) || (int(corner.g) != res.x) || (int(corner.b) != res.y);
    
    // Frame 0 and resolution change: Initialize with seed region
    if (needsReset) {
        int hsv = 0;
        ivec2 center = res / 2;
        int radius = 8;
        ivec2 d = pos - center;
        
        if (d.x * d.x + d.y * d.y <= radius * radius) {
            // Matches Python _init_seed: single RNG call, specific bit extraction
            uint rand = hash3(uint(pos.x), uint(pos.y), 12345u);
            int h = int(rand) & 0x1F;
            int s = 20 + ((int(rand) >> 5) & 0xF);   // High saturation
            int v = 16 + ((int(rand) >> 9) & 0xF);   // Medium-high value
            s = min(31, s);
            v = min(31, v);
            hsv = (v << 10) | (s << 5) | h;
        }
        
        // Store resolution in corner pixel
        float g = (pos == ivec2(0, 0)) ? iResolution.x : 0.0;
        float b = (pos == ivec2(0, 0)) ? iResolution.y : 0.0;
        fragColor = vec4(float(hsv), g, b, 1.0);
        return;
    }
    
    // If not at a generation boundary, pass through unchanged
    if (zoomFrame != 0) {
        fragColor = texelFetch(iChannel0, pos, 0);
        return;
    }
    
    // ==========================================================================
    // EVOLUTION - only runs at generation boundaries (zoomFrame == 0)
    // ==========================================================================
    
    // Focus point (normalized 0.5, 0.5 = center)
    // Convert to CA coordinates and clamp to [1, res/2 - 1] per Python set_focus
    vec2 focus_norm = vec2(0.5);  // Can be made dynamic later
    int half_w = res.x / 2;
    int half_h = res.y / 2;
    int focus_x = clamp(int(focus_norm.x * float(half_w)), 1, half_w - 1);
    int focus_y = clamp(int(focus_norm.y * float(half_h)), 1, half_h - 1);
    
    // Which parent pixel produced this output pixel?
    int col = pos.x / 2;  // src_x in Python
    int row = pos.y / 2;  // src_y in Python
    int child_idx = (pos.y & 1) * 2 + (pos.x & 1);
    
    // Source position with focus offset, clamped to [1, res-2] to ensure 3x3 kernel fits
    // Matches Python: fx = max(1, min(self.width - 2, src_x + self.focus_x))
    int fx = clamp(col + focus_x, 1, res.x - 2);
    int fy = clamp(row + focus_y, 1, res.y - 2);
    
    // Read 3x3 neighborhood with clamping
    int p0 = readHSV(ivec2(fx - 1, fy - 1), res);
    int p1 = readHSV(ivec2(fx,     fy - 1), res);
    int p2 = readHSV(ivec2(fx + 1, fy - 1), res);
    int p3 = readHSV(ivec2(fx - 1, fy),     res);
    int p4 = readHSV(ivec2(fx,     fy),     res);  // center
    int p5 = readHSV(ivec2(fx + 1, fy),     res);
    int p6 = readHSV(ivec2(fx - 1, fy + 1), res);
    int p7 = readHSV(ivec2(fx,     fy + 1), res);
    int p8 = readHSV(ivec2(fx + 1, fy + 1), res);
    
    int center = p4;
    
    // Neighbor patterns per child (from NEIGHBOR_PATTERNS)
    int n1_idx, n2_idx, n3_idx;
    if (child_idx == 0) { n1_idx = 3; n2_idx = 0; n3_idx = 1; }       // left, top-left, top
    else if (child_idx == 1) { n1_idx = 1; n2_idx = 2; n3_idx = 5; }  // top, top-right, right
    else if (child_idx == 2) { n1_idx = 7; n2_idx = 6; n3_idx = 3; }  // bottom, bottom-left, left
    else { n1_idx = 5; n2_idx = 8; n3_idx = 7; }                      // right, bottom-right, bottom
    
    // Get neighbor pixels
    int pn1, pn2, pn3;
    if (n1_idx == 0) pn1 = p0; else if (n1_idx == 1) pn1 = p1; else if (n1_idx == 2) pn1 = p2;
    else if (n1_idx == 3) pn1 = p3; else if (n1_idx == 5) pn1 = p5;
    else if (n1_idx == 6) pn1 = p6; else if (n1_idx == 7) pn1 = p7; else pn1 = p8;
    
    if (n2_idx == 0) pn2 = p0; else if (n2_idx == 1) pn2 = p1; else if (n2_idx == 2) pn2 = p2;
    else if (n2_idx == 3) pn2 = p3; else if (n2_idx == 5) pn2 = p5;
    else if (n2_idx == 6) pn2 = p6; else if (n2_idx == 7) pn2 = p7; else pn2 = p8;
    
    if (n3_idx == 0) pn3 = p0; else if (n3_idx == 1) pn3 = p1; else if (n3_idx == 2) pn3 = p2;
    else if (n3_idx == 3) pn3 = p3; else if (n3_idx == 5) pn3 = p5;
    else if (n3_idx == 6) pn3 = p6; else if (n3_idx == 7) pn3 = p7; else pn3 = p8;
    
    // Random bit shifts per child (from RAND_SHIFTS)
    int v_shift, s_shift, h_shift;
    if (child_idx == 0) { v_shift = 0; s_shift = 10; h_shift = 20; }
    else if (child_idx == 1) { v_shift = 3; s_shift = 13; h_shift = 23; }
    else if (child_idx == 2) { v_shift = 5; s_shift = 15; h_shift = 25; }
    else { v_shift = 8; s_shift = 18; h_shift = 28; }
    
    // Random value for this parent pixel (hash approximation)
    uint rand_val = hash3(uint(col), uint(row), uint(generation));
    
    // Row offset increments each generation (matches Python row_offset_v/s/h)
    int row_offset = generation & 0xF;
    
    // Extract channels from center
    int center_v = (center >> 10) & 0x1F;
    int center_s = (center >> 5) & 0x1F;
    int center_h = center & 0x1F;
    
    // Extract channels from neighbors
    int n1_v = (pn1 >> 10) & 0x1F, n1_s = (pn1 >> 5) & 0x1F, n1_h = pn1 & 0x1F;
    int n2_v = (pn2 >> 10) & 0x1F, n2_s = (pn2 >> 5) & 0x1F, n2_h = pn2 & 0x1F;
    int n3_v = (pn3 >> 10) & 0x1F, n3_s = (pn3 >> 5) & 0x1F, n3_h = pn3 & 0x1F;
    
    // Select closest neighbor per channel (tie-breaking chain)
    int sel_v = selectNeighbor(center_v, n1_v, n2_v, n3_v);
    int sel_s = selectNeighbor(center_s, n1_s, n2_s, n3_s);
    int sel_h = selectNeighbor(center_h, n1_h, n2_h, n3_h);
    
    // Mutation table row indices with generation-based offset
    // Matches Python: row_v = (self.row_offset_v + (center >> 6)) & 0xF
    int row_v = (row_offset + (center >> 6)) & 0xF;
    int row_s = (row_offset + (center >> 1)) & 0xF;
    int row_h = (row_offset + (center >> 11)) & 0xF;
    
    // Mutation table column from random bits
    int col_v = (int(rand_val) >> v_shift) & 0xF;
    int col_s = (int(rand_val) >> s_shift) & 0xF;
    int col_h = (int(rand_val) >> h_shift) & 0xF;
    
    // Look up mutations
    int mut_v = MUT[row_v * 16 + col_v];
    int mut_s = MUT[row_s * 16 + col_s];
    int mut_h = MUT[row_h * 16 + col_h];
    
    // Apply mutations with clamping (matches Python max(0, min(31, ...)))
    int new_v = clamp(sel_v + mut_v, 0, 31);
    int new_s = clamp(sel_s + mut_s, 0, 31);
    int new_h = clamp(sel_h + mut_h, 0, 31);
    
    // Pack as HSV555
    int child = (new_v << 10) | (new_s << 5) | new_h;
    
    // Preserve resolution in corner pixel
    float g = (pos == ivec2(0, 0)) ? iResolution.x : 0.0;
    float b = (pos == ivec2(0, 0)) ? iResolution.y : 0.0;
    fragColor = vec4(float(child), g, b, 1.0);
}
