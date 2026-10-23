/* ima_core.c -- IMA cellular automata engine, a port of the decompiled
 * "traveller 2.0c" core (see ima_core.h). */

#include <stdlib.h>
#include <string.h>
#include "ima_core.h"

/* ---------------------------------------------------------------------------
 * Mutation table (sub_401A50): 16x16 signed values, rows selected by pixel
 * bits (+ cycling offset), columns by random bits.  Zeros in the center,
 * magnitudes growing toward the edges (max +/-7).
 * ------------------------------------------------------------------------ */
static const signed char MUT[16][16] = {
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },
    { -1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1 },
    { -1, -1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1 },
    { -2, -1, -1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  2 },
    { -2, -2, -1, -1,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  2,  2 },
    { -2, -2, -2, -1, -1,  0,  0,  0,  0,  0,  0,  1,  1,  1,  2,  2 },
    { -3, -2, -2, -1, -1, -1,  0,  0,  0,  0,  1,  1,  1,  2,  2,  3 },
    { -3, -3, -2, -2, -1, -1, -1,  0,  0,  1,  1,  1,  2,  2,  3,  3 },
    { -4, -3, -3, -2, -1, -1, -1, -1,  1,  1,  1,  1,  2,  3,  3,  4 },
    { -4, -4, -3, -2, -2, -1, -1, -1,  1,  1,  1,  2,  2,  3,  4,  4 },
    { -5, -4, -3, -2, -2, -2, -1, -1,  1,  1,  2,  2,  2,  3,  4,  5 },
    { -5, -4, -3, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  3,  4,  5 },
    { -6, -4, -3, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  3,  4,  6 },
    { -6, -5, -4, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  4,  5,  6 },
    { -7, -5, -4, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  4,  5,  7 },
    { -7, -6, -5, -4, -3, -2, -2, -1,  1,  2,  2,  3,  4,  5,  6,  7 },
};

/* Neighbor patterns per child (chain: n1 vs n2, ties -> n2; winner vs n3,
 * ties -> n3) from the 3x3 neighborhood
 *   [0][1][2]
 *   [3][4][5]   <- [4] = center (parent)
 *   [6][7][8]
 */
static const int PAT[4][3] = {
    { 3, 0, 1 },   /* child 0 (top-left)     */
    { 1, 2, 5 },   /* child 1 (top-right)    */
    { 7, 6, 3 },   /* child 2 (bottom-left)  */
    { 5, 8, 7 },   /* child 3 (bottom-right) */
};

/* Random-value bit positions selecting the mutation column, per child:
 * H channel from sub_402D20, S from sub_402EF0, V from sub_403140. */
static const int HSH[4] = {  0,  3,  5,  8 };
static const int SSH[4] = { 10, 13, 15, 18 };
static const int VSH[4] = { 20, 23, 25, 28 };

/* ---------------------------------------------------------------------------
 * RNG: Park-Miller LCG (sub_4039A0) seeding a lagged Fibonacci (24,55)
 * generator (sub_4039F0).  All 56 slots get LCG pair-sums; slot 0 is filled
 * but never read (j,k only visit 1..55).
 * ------------------------------------------------------------------------ */
static int pm_next(ImaRng *r)
{
    /* v = 48271*(s % 44488) - 3399*(s / 44488); if (v <= 0) v += 2^31-1 */
    int v = 48271 * (int)(r->lcg % 44488u) - 3399 * (int)(r->lcg / 44488u);
    if (v <= 0)
        v += 0x7FFFFFFF;
    r->lcg = (unsigned int)v;
    return v;
}

static void rng_init(ImaRng *r, unsigned int seed)
{
    int i;
    r->lcg = seed;
    for (i = 0; i < 56; i++) {
        unsigned int v1 = (unsigned int)pm_next(r);
        unsigned int v2 = (unsigned int)pm_next(r);
        r->state[i] = v1 + v2;
    }
    r->j = 24;
    r->k = 55;
}

static unsigned int rng_next(ImaRng *r)
{
    unsigned int out;
    int j_wrapped;
    r->state[r->k] += r->state[r->j];
    out = r->state[r->k];
    j_wrapped = (r->j-- == 1);
    r->k--;
    if (j_wrapped)
        r->j = 55;
    else if (r->k == 0)
        r->k = 55;
    return out;
}

/* ---------------------------------------------------------------------------
 * Palette (sub_402720 + sub_401080): 32768 entries indexed (H<<10)|(S<<5)|V,
 * each an RGBA5551 texel:  R[15:11] G[10:6] B[5:1] A[0]=1.
 *   hue = 8*H * 1.4117647 degrees (360/255), sat = 8*S / 255,
 *   val = (8*V | 7) / 255.
 * ------------------------------------------------------------------------ */
static void hsv_to_rgb(double hue, double sat, double val,
                       double *r, double *g, double *b)   /* sub_401080 */
{
    int    sector;
    double f, p, q, t;
    if (sat == 0.0) {
        *r = *g = *b = val;
        return;
    }
    if (hue >= 360.0)
        hue = 0.0;
    sector = (int)(hue / 60.0);
    f = hue / 60.0 - (double)sector;
    p = (1.0 - sat) * val;
    q = (1.0 - f * sat) * val;
    t = (1.0 - (1.0 - f) * sat) * val;
    switch (sector) {
    case 0: *r = val; *g = t;   *b = p;   break;
    case 1: *r = q;   *g = val; *b = p;   break;
    case 2: *r = p;   *g = val; *b = t;   break;
    case 3: *r = p;   *g = q;   *b = val; break;
    case 4: *r = t;   *g = p;   *b = val; break;
    default:*r = val; *g = p;   *b = q;   break;
    }
}

static void build_palette(unsigned short *pal)
{
    int h, s, v;
    for (h = 0; h < 32; h++) {
        double hue = (double)(8 * h) * 1.4117647;
        for (s = 0; s < 32; s++) {
            double sat = (double)(8 * s) * 0.0039215679;
            for (v = 0; v < 32; v++) {
                double val = (double)((8 * v) | 7) * 0.0039215679;
                double r, g, b;
                hsv_to_rgb(hue, sat, val, &r, &g, &b);
                /* original packing, alpha bit forced to 1 by the |4 */
                pal[(h << 10) | (s << 5) | v] = (unsigned short)
                    (((unsigned char)((int)(b * 255.0) | 4) >> 2)
                     | (8 * ((32 * ((int)(r * 255.0) & 0xF8))
                             | ((int)(g * 255.0) & 0xF8))));
            }
        }
    }
}

/* ------------------------------------------------------------------------ */

int ima_create(Ima *m, int width, int height)
{
    memset(m, 0, sizeof *m);
    m->width  = width;
    m->height = height;
    m->front   = (unsigned short *)calloc((size_t)width * height, 2);
    m->back    = (unsigned short *)calloc((size_t)width * height, 2);
    m->display = (unsigned short *)calloc((size_t)width * height, 2);
    if (!m->front || !m->back || !m->display) {
        ima_destroy(m);
        return 0;
    }
    build_palette(m->palette);
    m->focus_x = 1;              /* sub_401A50: sub_402890(this, 1, 1) */
    m->focus_y = 1;
    m->gen_counter = 1;
    return 1;
}

void ima_destroy(Ima *m)
{
    free(m->front);   m->front = 0;
    free(m->back);    m->back = 0;
    free(m->display); m->display = 0;
}

void ima_seed(Ima *m, unsigned int seed)
{
    /* sub_402400 mode 3: 2x2 random block at (h/2-1, w/2-1) in this[18] */
    int base = m->width * ((m->height >> 1) - 1) + (m->width >> 1) - 1;
    int pos[4], i;
    pos[0] = base;
    pos[1] = base + 1;
    pos[2] = base + m->width;
    pos[3] = base + m->width + 1;
    rng_init(&m->rng, seed);
    memset(m->front, 0, (size_t)m->width * m->height * 2);
    memset(m->back,  0, (size_t)m->width * m->height * 2);
    for (i = 0; i < 4; i++) {
        m->rand_val = rng_next(&m->rng);
        m->front[pos[i]] = (unsigned short)(m->rand_val & 0x7FFF);
    }
}

void ima_set_focus(Ima *m, int x, int y)
{
    int hw = m->width >> 1, hh = m->height >> 1;
    m->focus_x = (x <= 0) ? 1 : (x < hw ? x : hw - 1);
    m->focus_y = (y <= 0) ? 1 : (y < hh ? y : hh - 1);
}

void ima_update_row_offsets(Ima *m)
{
    /* sub_402CB0: periods hardwired by the driver via sub_402C60(3, 5, 7) */
    unsigned int g = m->gen_counter;
    if (g % 3u == 0) m->off_h = (m->off_h + 1) & 0xF;
    if (g % 5u == 0) m->off_s = (m->off_s + 1) & 0xF;
    if (g % 7u == 0) m->off_v = (m->off_v + 1) & 0xF;
    m->gen_counter = g + 1;
}

void ima_swap(Ima *m)
{
    unsigned short *t = m->back;
    m->back  = m->front;
    m->front = t;
}

/* Chain comparison used by all three channels; ties prefer n2, then n3. */
static int chain_sel(int c, int n1, int n2, int n3)
{
    int d1 = c - n1; if (d1 < 0) d1 = -d1;
    int d2 = c - n2; if (d2 < 0) d2 = -d2;
    int w  = (d1 >= d2) ? n2 : n1;
    int dw = c - w;  if (dw < 0) dw = -dw;
    int d3 = c - n3; if (d3 < 0) d3 = -d3;
    return (dw >= d3) ? n3 : w;
}

/* One parent -> 4 children (sub_402D20 + sub_402EF0 + sub_403140).
 * p[] = 3x3 neighborhood, full 16-bit values. */
static void breed(const Ima *m, const int p[9], unsigned short out[4])
{
    int c = p[4];
    unsigned int r = m->rand_val;
    /* Cross-channel row keying: H row from S bits, S row from V bits,
     * V row from H bits of the parent. */
    const signed char *mh = MUT[((c >> 6)  + m->off_h) & 0xF];
    const signed char *ms = MUT[((c >> 1)  + m->off_s) & 0xF];
    const signed char *mv = MUT[((c >> 11) + m->off_v) & 0xF];
    int i;
    for (i = 0; i < 4; i++) {
        int n1 = p[PAT[i][0]], n2 = p[PAT[i][1]], n3 = p[PAT[i][2]];

        /* H (bits 10-14, sub_402D20): compares FULL pixel values (S and V
         * act as tie-breakers) and WRAPS mod 32 -- hue is circular. */
        int hh = ((chain_sel(c, n1, n2, n3) >> 10)
                  + mh[(r >> HSH[i]) & 0xF]) & 0x1F;

        /* S (bits 5-9, sub_402EF0): masked comparison, clamps. */
        int ss = (chain_sel(c & 0x3E0, n1 & 0x3E0, n2 & 0x3E0, n3 & 0x3E0) >> 5)
                 + ms[(r >> SSH[i]) & 0xF];
        if (ss < 0) ss = 0; else if (ss > 31) ss = 31;

        /* V (bits 0-4, sub_403140): masked comparison, clamps. */
        int vv = chain_sel(c & 0x1F, n1 & 0x1F, n2 & 0x1F, n3 & 0x1F)
                 + mv[(r >> VSH[i]) & 0xF];
        if (vv < 0) vv = 0; else if (vv > 31) vv = 31;

        out[i] = (unsigned short)((hh << 10) | (ss << 5) | vv);
    }
}

void ima_evolve_rows(Ima *m, int y0, int y1)
{
    int w = m->width, hw = w >> 1;
    int row;
    for (row = y0; row <= y1; row++) {
        const unsigned short *src =
            m->back + (size_t)(m->focus_y + row) * w + m->focus_x;
        unsigned short *d0 = m->front + (size_t)(2 * row) * w;
        unsigned short *d1 = d0 + w;
        unsigned short out[4];
        int p[9], col;

        /* Focus clamping guarantees the 3x3 window stays in bounds. */
        p[0] = src[-w - 1]; p[1] = src[-w]; p[2] = src[-w + 1];
        p[3] = src[-1];     p[4] = src[0];  p[5] = src[1];
        p[6] = src[w - 1];  p[7] = src[w];  p[8] = src[w + 1];

        /* First pixel of each row reuses the previous random value --
         * sub_4029A0 only steps the generator between columns. */
        breed(m, p, out);
        d0[0] = out[0]; d0[1] = out[1];
        d1[0] = out[2]; d1[1] = out[3];

        for (col = 1; col < hw; col++) {
            const unsigned short *nc = src + col + 1;
            p[0] = p[1]; p[1] = p[2];
            p[3] = p[4]; p[4] = p[5];
            p[6] = p[7]; p[7] = p[8];
            p[2] = nc[-w]; p[5] = nc[0]; p[8] = nc[w];
            m->rand_val = rng_next(&m->rng);
            breed(m, p, out);
            d0[2 * col] = out[0]; d0[2 * col + 1] = out[1];
            d1[2 * col] = out[2]; d1[2 * col + 1] = out[3];
        }
    }
}

void ima_convert_rows(Ima *m, int y0, int y1)
{
    int w = m->width, y, x;
    for (y = y0; y <= y1; y++) {
        const unsigned short *s = m->front + (size_t)y * w;
        unsigned short       *d = m->display + (size_t)y * w;
        for (x = 0; x < w; x++)
            d[x] = m->palette[s[x]];
    }
}
