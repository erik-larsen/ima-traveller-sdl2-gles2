/*
 * traveller.c -- "IMA Traveller" (traveller 2.0c, Driessens & Verstappen 1996)
 * ported to GLUT / freeglut-sdl2-ogles2 (native Mac/Win/Linux via gl4es+ANGLE,
 * web via Emscripten).
 *
 * Reverse engineered from the original Windows 98 + OpenGL executable via
 * Ghidra/Hex-Rays decompilation; function comments reference the original's
 * addresses (sub_XXXXXX).  The CA core lives in ima_core.c and is bit-exact
 * with the Python reference (ima-traveller repo).
 *
 * The renderer reproduces the original's immediate-mode OpenGL scheme:
 *   - the CA image is palette-converted to RGBA5551 and tiled into a grid of
 *     256x256 GL_RGB5_A1 textures (Win98-era max texture size), two sets,
 *     swapped every generation (sub_4034C0 / sub_403D30 / sub_403DC0)
 *   - each frame draws the tile grid as GL_TRIANGLE_STRIP quads over a
 *     rectangle in NDC (identity projection/modelview), which zooms 2x per
 *     generation toward the mouse-driven focus point (sub_403E80/sub_4068A0)
 *   - evolution is amortized: 1/2/4/8 CA rows per displayed frame ("speed"),
 *     and at generation end the new image is uploaded one tile per frame
 *     (sub_4065C0), keeping the animation smooth on 1998 hardware
 *
 * Config file "traveller.config" (same keys/defaults as the original):
 *     speed 2          # 1..4 -> 1/2/4/8 CA rows per frame (5 = 16: port ext.)
 *     rate 60          # target fps for sync clock
 *     steer LINEAR     # LINEAR | CURVED (Hermite pan)
 *     sync CLOCK       # FREE | CLOCK | CHRONO | VBLANK
 *     filter NEAREST   # NEAREST | LINEAR
 *     detail LOW       # LOW (768x512) | MEDIUM (1024x768) | HIGH (1280x1024)
 *                      # | ULTRA (2048x1536) | ULTRA2 (2560x2048) (port ext.)
 *     xres 640         # window width: 640 | 800 | 1024
 *     fullscreen no
 *
 * Command line (port extensions):  -seed N   fixed RNG seed (default time(0))
 *                                  -trial    exit after 101 generations, as the
 *                                            original 2.0c demo did
 *                                  -dump N   write traveller_gen_XXX.bmp after
 *                                            each generation, quit after N
 *                                            (headless render verification)
 *
 * Keys: F1 or p = freeze,  F4 or s = single step (while frozen),  ESC = quit.
 * Right mouse button = menu.  Mouse position steers the zoom focus.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <GL/glut.h>
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

#include "ima_core.h"

#ifndef GL_RGB5_A1
#define GL_RGB5_A1 0x8057
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#endif
#ifndef GL_CLAMP
#define GL_CLAMP 0x2900
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNPACK_SKIP_ROWS
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#endif
#ifndef GL_UNPACK_SKIP_PIXELS
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif

#define TILE 256           /* original texture tile size (sub_406090)     */
#define MAX_TILES (10 * 8) /* detail ULTRA2 (port extension): 10x8 tiles  */
#define MAX_GRID  ((10 + 1) * (8 + 1))

/* ---- configuration (dword_42CD40..: defaults from WinMain) ------------- */
static int cfg_speed      = 2;   /* 1..4                                   */
static int cfg_rate       = 60;  /* fps for sync CLOCK                     */
static int cfg_steer      = 1;   /* 1 = LINEAR, 2 = CURVED (Hermite)       */
static int cfg_sync       = 1;   /* 0 FREE, 1 CLOCK, 2 CHRONO, 3 VBLANK    */
static int cfg_filter     = 1;   /* 1 = NEAREST, 2 = LINEAR                */
static int cfg_detail     = 0;   /* 0 LOW, 1 MEDIUM, 2 HIGH                */
static int cfg_xres       = 640;
static int cfg_fullscreen = 0;

/* The original always ran a CA close to its fixed 4:3 window sizes, giving
 * ~1 texel per pixel at generation start (2 at the end).  On a modern large
 * window the LOW default starts at several pixels per texel and looks blocky
 * long before the next generation refines it.  Unless traveller.config pins
 * them, detail is auto-matched to the drawable height at startup, and speed
 * is raised alongside so the generation cadence stays at the original's
 * ~2x zoom per 2.2 s. */
static int cfg_detail_set = 0;
static int cfg_speed_set  = 0;

static unsigned int rng_seed;    /* ima this[16]: time(0) unless -seed     */
static int opt_trial = 0;        /* original quit-after-101-cycles gate    */
static int opt_dump = 0;         /* -dump N: BMP per generation, then quit */

/* ---- window ------------------------------------------------------------ */
static int win_w = 640, win_h = 480;

/* ---- CA ("ima") --------------------------------------------------------- */
static Ima ima;

/* ---- zoomer (object at dword_42CC60; offsets noted as this+N) ---------- */
static unsigned int cycles;        /* this+4:  completed generations       */
static int   frame_in_gen;         /* this+8:  frames drawn this generation*/
static int   cur_row;              /* this+12: next CA row block           */
static int   upload_idx = -1;      /* >=0: uploading tile N, one per frame */
static int   rows_per_step;        /* this+16: 1/2/4/8 from speed          */
static int   half_rows;            /* this+20: ca_h / 2                    */
static float focus_fx, focus_fy;   /* this+60/64: focus in [-0.5, 0.5]     */
static float lin_x0, lin_y0;       /* this+68/72: Hermite p0               */
static float lin_x1, lin_y1;       /* this+76/80: Hermite p1               */
static float tan_x1, tan_y1;       /* this+84/88: Hermite m1               */
static float tan_x0, tan_y0;       /* this+92/96: Hermite m0 (prev m1)     */
static float quad_cx, quad_cy;     /* this+100/104: quad center (NDC)      */
static float vel_x, vel_y;         /* this+108/112: LINEAR pan velocity    */
static float quad_w, quad_h;       /* this+116/120: quad size (NDC)        */
static float zoomf;                /* this+124: per-frame zoom, 2^(1/n)    */
static float *hermite = 0;         /* this+128: n * [h00 h01 h10 h11]      */
static int   frames_per_gen;       /* dword_42CD80                         */

/* ---- texture plane (this+28: sub_403A80 object) ------------------------ */
static int    tiles_x, tiles_y;
static GLuint tex[2][MAX_TILES];   /* [set][tile]; set swaps per generation */
static int    tex_front;           /* drawn set; 1-tex_front is upload set  */
static float  verts[MAX_GRID * 3];

/* ---- timing (QueryPerformanceCounter equivalents) ----------------------- */
static Uint64 perf_freq;
static Uint64 frame_start;         /* this+160 */
static Uint64 frame_ticks;         /* this+180: freq / rate (sync CLOCK)   */
static Uint64 chrono_ticks = 1000; /* this+176: adaptive target (CHRONO)   */
static Uint64 chrono_sum;          /* dword_42CD7C */
static unsigned int chrono_cnt;    /* dword_42CD78 */
static Uint64 stat_min = ~(Uint64)0, stat_max = 0, stat_frames = 0;
static double stat_start;

/* ---- input ------------------------------------------------------------- */
static int mouse_x, mouse_y;       /* last pointer position (drawable px)  */
static int steer_live = 0;         /* pointer has taken over from center   */
static int anchor_x = -1, anchor_y;/* pointer position when steering armed */
#define STEER_ARM_PT 8             /* move this far (points) to take over  */
static int frozen = 0;             /* byte_42CBF0 */
static int step_once = 0;

/* ========================================================================
 * config file (sub_405390); same keys as the original "traveller.config"
 * ======================================================================== */
static void load_config(const char *fname)
{
    char line[192], key[64], val[64];
    FILE *f = fopen(fname, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %63s", key, val) != 2)
            continue;
        if (!strcmp(key, "speed")) { cfg_speed = atoi(val); cfg_speed_set = 1; }
        if (!strcmp(key, "rate"))   cfg_rate = atoi(val);
        if (!strcmp(key, "steer"))  cfg_steer = !strcmp(val, "CURVED") ? 2 : 1;
        if (!strcmp(key, "sync"))
            cfg_sync = !strcmp(val, "CHRONO") ? 2
                     : !strcmp(val, "CLOCK")  ? 1
                     : !strcmp(val, "VBLANK") ? 3 : 0;
        if (!strcmp(key, "filter")) cfg_filter = !strcmp(val, "LINEAR") ? 2 : 1;
        if (!strcmp(key, "xres"))   cfg_xres = atoi(val);
        if (!strcmp(key, "detail")) {
            cfg_detail = !strcmp(val, "MEDIUM") ? 1
                       : !strcmp(val, "HIGH")   ? 2
                       : !strcmp(val, "ULTRA")  ? 3
                       : !strcmp(val, "ULTRA2") ? 4 : 0;
            cfg_detail_set = 1;
        }
        if (!strcmp(key, "fullscreen")) cfg_fullscreen = strcmp(val, "no") != 0;
    }
    fclose(f);
}

/* sub_4062F0 clamped speed to 1..4; 5 (16 rows/frame) is a port extension
 * so the ULTRA2 CA keeps the original generation cadence */
static void set_speed(int s)
{
    cfg_speed = (s < 1) ? 1 : (s > 5) ? 5 : s;
}

/* ========================================================================
 * textures (sub_403A80 / sub_4034C0 / sub_403DD0)
 * ======================================================================== */
static void set_filter(int mode)   /* sub_406330 */
{
    GLint f = (mode != 1) ? GL_LINEAR : GL_NEAREST;
    int s, i;
    cfg_filter = mode;
    for (s = 0; s < 2; s++) {
        for (i = 0; i < tiles_x * tiles_y; i++) {
            glBindTexture(GL_TEXTURE_2D, tex[s][i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        }
    }
}

static void create_textures(void)  /* sub_4034C0 */
{
    int s, r, c;
    /* one glGenTextures per set: the sets are rows of tex[2][MAX_TILES],
     * which are not contiguous when tiles_x*tiles_y < MAX_TILES */
    glGenTextures(tiles_x * tiles_y, tex[0]);
    glGenTextures(tiles_x * tiles_y, tex[1]);
    for (s = 0; s < 2; s++) {
        for (r = 0; r < tiles_y; r++) {
            for (c = 0; c < tiles_x; c++) {
                glBindTexture(GL_TEXTURE_2D, tex[s][c + tiles_x * r]);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, ima.width);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, TILE * c);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, TILE * r);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, TILE, TILE, 0,
                             GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, ima.display);
                glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
            }
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (cfg_filter != 1)
        set_filter(cfg_filter);
}

static void destroy_textures(void)
{
    glDeleteTextures(tiles_x * tiles_y, tex[0]);
    glDeleteTextures(tiles_x * tiles_y, tex[1]);
}

static void upload_tile(int c, int r)   /* sub_403D30, into the back set */
{
    glBindTexture(GL_TEXTURE_2D, tex[1 - tex_front][c + tiles_x * r]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, ima.width);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, TILE * c);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, TILE * r);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TILE, TILE,
                    GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, ima.display);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
}

/* ========================================================================
 * quad grid (sub_403F70) and tile drawing (sub_403E80)
 * ======================================================================== */
static void update_grid(float x0, float y0, float x1, float y1)
{
    float dx = (x1 - x0) / (float)tiles_x;
    float dy = (y1 - y0) / (float)tiles_y;
    int r, c;
    float *v = verts;
    for (r = 0; r <= tiles_y; r++) {
        for (c = 0; c <= tiles_x; c++) {
            *v++ = x0 + (float)c * dx;
            *v++ = y0 + (float)r * dy;
            *v++ = 0.0f;
        }
    }
}

static void draw_tiles(void)
{
    int r, c;
    for (r = 0; r < tiles_y; r++) {
        for (c = 0; c < tiles_x; c++) {
            int i0 = c + (tiles_x + 1) * r;        /* bottom-left vertex  */
            int i1 = i0 + tiles_x + 1;             /* top-left vertex     */
            glBindTexture(GL_TEXTURE_2D, tex[tex_front][c + tiles_x * r]);
            glBegin(GL_TRIANGLE_STRIP);
            glTexCoord2f(0.0f, 0.0f); glVertex3fv(&verts[3 * i0]);
            glTexCoord2f(1.0f, 0.0f); glVertex3fv(&verts[3 * (i0 + 1)]);
            glTexCoord2f(0.0f, 1.0f); glVertex3fv(&verts[3 * i1]);
            glTexCoord2f(1.0f, 1.0f); glVertex3fv(&verts[3 * (i1 + 1)]);
            glEnd();
        }
    }
}

/* ========================================================================
 * Hermite pan table (sub_406AE0) and evaluation (sub_406C10)
 * ======================================================================== */
static void build_hermite(int n, float zf)
{
    int i;
    for (i = 0; i < n; i++) {
        float u  = (float)(pow((double)zf, (double)i) - 1.0);  /* in [0,1] */
        float u2 = u * u;
        float u3 = u2 * u;
        hermite[4 * i + 0] = 1.0f - (3.0f * u2 - 2.0f * u3);   /* h00 */
        hermite[4 * i + 1] = 3.0f * u2 - 2.0f * u3;            /* h01 */
        hermite[4 * i + 2] = u3 - 2.0f * u2 + u;               /* h10 */
        hermite[4 * i + 3] = u3 - u2;                          /* h11 */
    }
}

static void eval_hermite(void)
{
    const float *t = &hermite[4 * frame_in_gen];
    quad_cx = lin_x0 * t[0] + lin_x1 * t[1] + tan_x0 * t[2] + tan_x1 * t[3];
    quad_cy = lin_y0 * t[0] + lin_y1 * t[1] + tan_y0 * t[2] + tan_y1 * t[3];
}

/* ========================================================================
 * frame pacing (the QueryPerformanceCounter waits of sub_4068A0)
 * ======================================================================== */
static void pace_frame(void)
{
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 elapsed = now - frame_start;

    if (cfg_sync == 2) {                       /* CHRONO: adaptive          */
        if (cur_row == half_rows - rows_per_step) {
            /* heavy frames (last chunk + tile uploads): measure them      */
            if (chrono_cnt) {
                chrono_sum += elapsed;
                if (elapsed < stat_min) stat_min = elapsed;
                if (elapsed > stat_max) stat_max = elapsed;
            }
            if (chrono_cnt++ == (unsigned int)(tiles_x * tiles_y))
                chrono_ticks = chrono_sum / (Uint64)(tiles_x * tiles_y);
        } else {
            chrono_sum = 0;
            chrono_cnt = 0;
#ifndef __EMSCRIPTEN__
            while (SDL_GetPerformanceCounter() - frame_start < chrono_ticks)
                SDL_Delay(0);
#endif
        }
    } else if (cfg_sync == 1) {                /* CLOCK: fixed rate         */
#ifndef __EMSCRIPTEN__
        if (elapsed < frame_ticks)
            while (SDL_GetPerformanceCounter() - frame_start < frame_ticks)
                SDL_Delay(0);
#endif
    }
    if (cfg_sync != 2) {
        if (elapsed < stat_min) stat_min = elapsed;
        if (elapsed > stat_max) stat_max = elapsed;
    }
    frame_start = SDL_GetPerformanceCounter();
}

/* ========================================================================
 * one displayed frame (sub_4068A0): pace, place quad, draw, swap, advance
 * ======================================================================== */
static void draw_frame(void)
{
    float x0, y0;

    pace_frame();

    x0 = quad_cx - quad_w * 0.5f;
    y0 = quad_cy - quad_h * 0.5f;
    update_grid(x0, y0, x0 + quad_w, y0 + quad_h);
    draw_tiles();
    glutSwapBuffers();
    stat_frames++;

    frame_in_gen++;
    quad_w *= zoomf;
    quad_h *= zoomf;
    if (cfg_steer == 2) {
        eval_hermite();
    } else {
        quad_cx += vel_x;
        quad_cy += vel_y;
        vel_x *= zoomf;
        vel_y *= zoomf;
    }
}

/* ========================================================================
 * generation start (sub_406690 + sub_406440)
 * ======================================================================== */
static void generation_start(void)
{
    int   hw = ima.width >> 1, hh = ima.height >> 1;
    float fx, fy, half_qw;
    int   n;

    /* sub_406440: poll the mouse.  The original warped the cursor to the
     * window center for the first 3 generations (SetCursorPos in
     * sub_404630), so steering then began from center.  The web cannot warp
     * the pointer, so the focus stays centered until the pointer is moved
     * a few points after that (see motion()). */
    fx = -1.0f + 2.0f * (float)(steer_live ? mouse_x : win_w >> 1)
                      / (float)win_w;
    fy =  1.0f - 2.0f * (float)(steer_live ? mouse_y : win_h >> 1)
                      / (float)win_h;
    if (fx < -0.5f) fx = -0.5f; else if (fx > 0.5f) fx = 0.5f;
    if (fy < -0.5f) fy = -0.5f; else if (fy > 0.5f) fy = 0.5f;
    ima_set_focus(&ima, (int)((fx + 0.5f) * (float)hw),
                        (int)((fy + 0.5f) * (float)hh));
    /* the pan uses the clamped, quantized focus read back from the CA */
    focus_fx = (float)ima.focus_x / (float)hw - 0.5f;
    focus_fy = (float)ima.focus_y / (float)hh - 0.5f;

    tex_front = 1 - tex_front;                    /* sub_403DC0 */

    if (cycles > 9)                               /* row offsets from gen 11 */
        ima_update_row_offsets(&ima);

    rows_per_step = 1 << (cfg_speed - 1);         /* 1,2,4,8,16 */
    n = tiles_x * tiles_y + half_rows / rows_per_step;
    zoomf = (float)pow(2.0, 1.0 / (double)n);
    if (n != frames_per_gen)
        build_hermite(n, zoomf);
    frames_per_gen = n;

    /* quad: 1.1x overscanned NDC rect, aspect-corrected to square texels.
     * The original's fixed 4:3 windows always yielded half_qw >= 1.03, so
     * the quad covered the screen; on arbitrary (wide) windows the same
     * formula can drop below 1.0 and expose side bands of stale frame at
     * generation reset -- scale up (preserving aspect) to keep the 1.1x
     * overscan in both dimensions. */
    quad_h  = 2.2f;
    half_qw = (float)ima.width / (float)ima.height
              * (float)win_h / (float)win_w * 1.1f;
    if (half_qw < 1.1f) {
        quad_h *= 1.1f / half_qw;
        half_qw = 1.1f;
    }
    quad_w  = half_qw + half_qw;
    quad_cx = 0.0f;
    quad_cy = 0.0f;

    if (cfg_steer == 2) {                         /* CURVED: Hermite        */
        lin_x0 = 0.0f;
        lin_y0 = 0.0f;
        tan_x0 = tan_x1;                          /* m0 = previous m1       */
        tan_y0 = tan_y1;
        lin_x1 = (-focus_fx - focus_fx) * half_qw;
        lin_y1 = (-focus_fy - focus_fy) * (quad_h * 0.5f);
        tan_x1 = lin_x1 - lin_x0;
        tan_y1 = lin_y1 - lin_y0;
    } else {                                      /* LINEAR                 */
        float k = 2.0f - (zoomf + zoomf);         /* small negative         */
        vel_x = focus_fx * half_qw * k;
        vel_y = focus_fy * k * (quad_h * 0.5f);   /* orig: 1.1 = quad_h/2   */
    }

    ima_swap(&ima);                               /* sub_402C50 */
}

/* ========================================================================
 * one zoomer cycle (sub_4065C0): evolve a row chunk, convert, draw; at
 * generation end upload the new image one tile per drawn frame
 * ======================================================================== */
static void quit_with_stats(void)
{
    double secs = (double)SDL_GetPerformanceCounter() / (double)perf_freq
                  - stat_start;
    printf("Zoomer: ima %dx%d\n", ima.width, ima.height);
    printf("  %u cycles, %6.3f fps average, ", cycles,
           (double)stat_frames / (secs > 0 ? secs : 1));
    printf("frame min %6.3f, max %6.3f msec\n",
           (double)stat_min * 1000.0 / (double)perf_freq,
           (double)stat_max * 1000.0 / (double)perf_freq);
    destroy_textures();
    ima_destroy(&ima);
    exit(0);
}

/* -dump: BMP framebuffer capture for headless render verification.
 * glFinish() is required under gl4es before glReadPixels. */
static void dump_frame(unsigned int gen)
{
    static const unsigned char hdr0[2] = { 'B', 'M' };
    unsigned char info[52];
    char name[64];
    int row_bytes = (3 * win_w + 3) & ~3;
    unsigned int data_size = (unsigned int)row_bytes * win_h;
    unsigned int file_size = 54 + data_size;
    unsigned char *px = (unsigned char *)malloc((size_t)4 * win_w * win_h);
    unsigned char *row = (unsigned char *)calloc(1, (size_t)row_bytes);
    FILE *f;
    int x, y;

    glFinish();
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    snprintf(name, sizeof name, "traveller_gen_%03u.bmp", gen);
    f = fopen(name, "wb");
    if (f) {
        memset(info, 0, sizeof info);
        info[0] = (unsigned char)(file_size);
        info[1] = (unsigned char)(file_size >> 8);
        info[2] = (unsigned char)(file_size >> 16);
        info[3] = (unsigned char)(file_size >> 24);
        info[8] = 54;                       /* pixel data offset */
        info[12] = 40;                      /* BITMAPINFOHEADER  */
        info[16] = (unsigned char)(win_w);
        info[17] = (unsigned char)(win_w >> 8);
        info[20] = (unsigned char)(win_h);
        info[21] = (unsigned char)(win_h >> 8);
        info[24] = 1;                       /* planes            */
        info[26] = 24;                      /* bpp               */
        fwrite(hdr0, 1, 2, f);
        fwrite(info, 1, 52, f);
        for (y = 0; y < win_h; y++) {       /* BMP is bottom-up, like GL */
            const unsigned char *s = px + (size_t)4 * win_w * y;
            for (x = 0; x < win_w; x++) {
                row[3 * x]     = s[4 * x + 2];
                row[3 * x + 1] = s[4 * x + 1];
                row[3 * x + 2] = s[4 * x];
            }
            fwrite(row, 1, (size_t)row_bytes, f);
        }
        fclose(f);
        printf("wrote %s\n", name);
    }
    free(row);
    free(px);
}

static void zoomer_cycle(void)
{
    /* Generation-end tile uploads, one per displayed frame -- as the
     * original paced them (each Win98 swap = one frame here; exactly one
     * glutSwapBuffers per display callback, which the Emscripten rAF /
     * gl4es swap emulation requires).  cur_row stays at the last chunk
     * during uploads so CHRONO pacing sees these as heavy frames. */
    if (upload_idx >= 0) {
        upload_tile(upload_idx % tiles_x, upload_idx / tiles_x);
        /* the original draws a dummy point after each upload,
         * forcing the driver to transfer the texture now */
        glBegin(GL_POINTS);
        glVertex3f(0.0f, 0.0f, 0.0f);
        glEnd();
        draw_frame();
        if (++upload_idx >= tiles_x * tiles_y) {
            upload_idx = -1;
            cur_row = 0;
            frame_in_gen = 0;
            cycles++;
            if (opt_dump) {
                dump_frame(cycles);
                if (cycles >= (unsigned int)opt_dump)
                    quit_with_stats();
            }
        }
        return;
    }

    if (cur_row == 0)
        generation_start();

    ima_evolve_rows(&ima, cur_row, cur_row + rows_per_step - 1);
    ima_convert_rows(&ima, 2 * cur_row, 2 * (cur_row + rows_per_step) - 1);
    draw_frame();

    if (cur_row == half_rows - rows_per_step)
        upload_idx = 0;      /* generation image complete: start uploads */
    else
        cur_row += rows_per_step;
}

/* ========================================================================
 * (re)initialize CA + textures for the current detail level (sub_406090)
 * ======================================================================== */
static void zoomer_init(int first)
{
    /* LOW/MEDIUM/HIGH are the original's sizes; ULTRA is a port extension
     * (same 256-tile architecture) for retina-class drawables the original
     * never targeted. */
    static const int ca_size[5][2] =
        { {768, 512}, {1024, 768}, {1280, 1024}, {2048, 1536}, {2560, 2048} };
    int cw = ca_size[cfg_detail][0], ch = ca_size[cfg_detail][1];

    if (!first) {
        destroy_textures();
        ima_destroy(&ima);
        free(hermite);
    }
    if (!ima_create(&ima, cw, ch)) {
        fprintf(stderr, "traveller: out of memory\n");
        exit(1);
    }
    ima_seed(&ima, rng_seed);
    ima_convert_rows(&ima, 0, ch - 1);            /* sub_402900 */

    tiles_x = cw / TILE;
    tiles_y = ch / TILE;
    half_rows = ch / 2;
    create_textures();
    tex_front = 0;

    hermite = (float *)malloc(sizeof(float) * 4
                              * (size_t)(half_rows + tiles_x * tiles_y));
    frames_per_gen = 0;                            /* force Hermite rebuild */
    cycles = 0;
    cur_row = 0;
    upload_idx = -1;
    frame_in_gen = 0;
    tan_x1 = tan_y1 = 0.0f;
    steer_live = 0;
    anchor_x = -1;
    stat_frames = 0;
    stat_min = ~(Uint64)0;
    stat_max = 0;
    stat_start = (double)SDL_GetPerformanceCounter() / (double)perf_freq;
    frame_start = SDL_GetPerformanceCounter();
}

/* ========================================================================
 * GL state (sub_404430) and per-frame preamble (sub_404630)
 * ======================================================================== */
static void init_gl(void)
{
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_TEXTURE_2D);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static int zoomer_ready = 0;

/* Match the CA size to the drawable so a texel is ~1 pixel at generation
 * start, like the original's window/CA pairings; keep the zoom pace by
 * scaling speed with the larger CA (LOW/speed2 = 128 evolve frames/gen). */
static void auto_detail(void)
{
    int dh = glutGet(GLUT_WINDOW_HEIGHT);
    if (dh <= 0)
        dh = win_h;
    cfg_detail = (512 * 10 >= dh * 9)  ? 0
               : (768 * 10 >= dh * 9)  ? 1
               : (1024 * 10 >= dh * 9) ? 2
               : (1536 * 10 >= dh * 9) ? 3 : 4;
    if (!cfg_speed_set)
        cfg_speed = (cfg_detail == 0) ? 2
                  : (cfg_detail == 4) ? 5
                  : (cfg_detail == 3) ? 4 : 3;
}

static void display(void)
{
    if (!zoomer_ready) {
        if (!cfg_detail_set)
            auto_detail();
        zoomer_init(1);
        zoomer_ready = 1;
        printf("traveller: ima %dx%d, %dx%d tiles, speed %d, seed %u\n",
               ima.width, ima.height, tiles_x, tiles_y, cfg_speed, rng_seed);
    }

    if (frozen && !step_once) {
        /* keep showing the current frame */
        float x0 = quad_cx - quad_w * 0.5f;
        float y0 = quad_cy - quad_h * 0.5f;
        glViewport(0, 0, win_w, win_h);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_TEXTURE_2D);
        update_grid(x0, y0, x0 + quad_w, y0 + quad_h);
        draw_tiles();
        glutSwapBuffers();
        return;
    }
    step_once = 0;

    if (opt_trial && cycles > 100)     /* the original 2.0c demo limit */
        quit_with_stats();

    /* sub_404630 */
    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    zoomer_cycle();
}

static void idle(void)
{
    if (!frozen)
        glutPostRedisplay();
}

/* ========================================================================
 * input + menu (sub_404B10 / sub_4046E0)
 * ======================================================================== */
/* drawable pixels per window point (motion coordinates are drawable px) */
static float pixel_scale(void)
{
#ifdef __EMSCRIPTEN__
    double cw = 0.0, ch = 0.0;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    return cw > 0.0 ? (float)win_w / (float)cw : 1.0f;
#else
    SDL_Window *w = SDL_GL_GetCurrentWindow();
    int pw = 0, ph = 0;
    if (w)
        SDL_GetWindowSize(w, &pw, &ph);
    return pw > 0 ? (float)win_w / (float)pw : 1.0f;
#endif
}

static void motion(int x, int y)
{
    mouse_x = x;
    mouse_y = y;
    /* take over from center once the pointer moves STEER_ARM_PT from
     * where it rested after the first 3 (centered) generations */
    if (!steer_live && cycles >= 3) {
        if (anchor_x < 0) {
            anchor_x = x;
            anchor_y = y;
        } else {
            int t = (int)(STEER_ARM_PT * pixel_scale() + 0.5f);
            if (abs(x - anchor_x) >= t || abs(y - anchor_y) >= t)
                steer_live = 1;
        }
    }
}

static void keyboard(unsigned char key, int x, int y)
{
    (void)x; (void)y;
    switch (key) {
    case 27:
        quit_with_stats();
        break;
    case 'p': case 'P':
        frozen = !frozen;
        break;
    case 's': case 'S':
        if (frozen) {
            step_once = 1;
            glutPostRedisplay();
        }
        break;
    }
}

static void special(int key, int x, int y)
{
    (void)x; (void)y;
    if (key == GLUT_KEY_F1)
        frozen = !frozen;
    else if (key == GLUT_KEY_F4 && frozen) {
        step_once = 1;
        glutPostRedisplay();
    }
}

static void reshape(int w, int h)
{
    win_w = w > 0 ? w : 1;
    win_h = h > 0 ? h : 1;
}

enum {
    M_FREEZE = 1,
    M_SPEED_DOWN, M_SPEED_UP,
    M_STEER_LINEAR, M_STEER_CURVED,
    M_SYNC_FREE, M_SYNC_CLOCK, M_SYNC_CHRONO, M_SYNC_VBLANK,
    M_RATE_40, M_RATE_60, M_RATE_80, M_RATE_100, M_RATE_120,
    M_FILTER_NEAREST, M_FILTER_LINEAR,
    M_DETAIL_LOW, M_DETAIL_MEDIUM, M_DETAIL_HIGH, M_DETAIL_ULTRA,
    M_DETAIL_ULTRA2,
    M_QUIT
};

static void apply_rate(int rate)
{
    cfg_rate = rate;
    cfg_sync = 1;
    frame_ticks = perf_freq / (Uint64)cfg_rate;
}

static void set_detail(int detail)
{
    cfg_detail = detail;
    cfg_detail_set = 1;
    rng_seed = (unsigned int)time(0);   /* original re-seeds on re-init */
    zoomer_init(0);
}

static void menu(int item)
{
    switch (item) {
    case M_FREEZE:       frozen = !frozen;             break;
    case M_SPEED_DOWN:   set_speed(cfg_speed - 1);     break;
    case M_SPEED_UP:     set_speed(cfg_speed + 1);     break;
    case M_STEER_LINEAR: cfg_steer = 1;                break;
    case M_STEER_CURVED: cfg_steer = 2;                break;
    /* sub_4063B0: vsync on for VBLANK only, explicitly off otherwise
     * (CLOCK's busy-wait pacing + vsync would halve the frame rate) */
    case M_SYNC_FREE:    cfg_sync = 0;
                         SDL_GL_SetSwapInterval(0);    break;
    case M_SYNC_CLOCK:   cfg_sync = 1;
                         SDL_GL_SetSwapInterval(0);    break;
    case M_SYNC_CHRONO:  cfg_sync = 2;
                         SDL_GL_SetSwapInterval(0);    break;
    case M_SYNC_VBLANK:  cfg_sync = 3;
                         SDL_GL_SetSwapInterval(1);    break;
    case M_RATE_40:      apply_rate(40);               break;
    case M_RATE_60:      apply_rate(60);               break;
    case M_RATE_80:      apply_rate(80);               break;
    case M_RATE_100:     apply_rate(100);              break;
    case M_RATE_120:     apply_rate(120);              break;
    case M_FILTER_NEAREST: set_filter(1);              break;
    case M_FILTER_LINEAR:  set_filter(2);              break;
    case M_DETAIL_LOW:    set_detail(0);               break;
    case M_DETAIL_MEDIUM: set_detail(1);               break;
    case M_DETAIL_HIGH:   set_detail(2);               break;
    case M_DETAIL_ULTRA:  set_detail(3);               break;
    case M_DETAIL_ULTRA2: set_detail(4);               break;
    case M_QUIT:          quit_with_stats();           break;
    }
}

static void build_menu(void)
{
    int steer = glutCreateMenu(menu);
    glutAddMenuEntry("linear", M_STEER_LINEAR);
    glutAddMenuEntry("curved", M_STEER_CURVED);

    int sync = glutCreateMenu(menu);
    glutAddMenuEntry("free",   M_SYNC_FREE);
    glutAddMenuEntry("clock",  M_SYNC_CLOCK);
    glutAddMenuEntry("chrono", M_SYNC_CHRONO);
    glutAddMenuEntry("vblank", M_SYNC_VBLANK);

    int rate = glutCreateMenu(menu);
    glutAddMenuEntry("40",  M_RATE_40);
    glutAddMenuEntry("60",  M_RATE_60);
    glutAddMenuEntry("80",  M_RATE_80);
    glutAddMenuEntry("100", M_RATE_100);
    glutAddMenuEntry("120", M_RATE_120);

    int filter = glutCreateMenu(menu);
    glutAddMenuEntry("nearest", M_FILTER_NEAREST);
    glutAddMenuEntry("linear",  M_FILTER_LINEAR);

    int detail = glutCreateMenu(menu);
    glutAddMenuEntry("low (768x512)",     M_DETAIL_LOW);
    glutAddMenuEntry("medium (1024x768)", M_DETAIL_MEDIUM);
    glutAddMenuEntry("high (1280x1024)",  M_DETAIL_HIGH);
    glutAddMenuEntry("ultra (2048x1536)", M_DETAIL_ULTRA);
    glutAddMenuEntry("ultra2 (2560x2048)", M_DETAIL_ULTRA2);

    glutCreateMenu(menu);
    glutAddMenuEntry("freeze (F1)", M_FREEZE);
    glutAddMenuEntry("speed -", M_SPEED_DOWN);
    glutAddMenuEntry("speed +", M_SPEED_UP);
    glutAddSubMenu("steer",  steer);
    glutAddSubMenu("sync",   sync);
    glutAddSubMenu("rate",   rate);
    glutAddSubMenu("filter", filter);
    glutAddSubMenu("detail", detail);
    glutAddMenuEntry("quit (ESC)", M_QUIT);
    glutAttachMenu(GLUT_RIGHT_BUTTON);
}

/* ======================================================================== */
int main(int argc, char **argv)
{
    int i;

    rng_seed = (unsigned int)time(0);
    load_config("traveller.config");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-seed") && i + 1 < argc)
            rng_seed = (unsigned int)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "-trial"))
            opt_trial = 1;
        else if (!strcmp(argv[i], "-dump") && i + 1 < argc)
            opt_dump = atoi(argv[++i]);
    }

    win_w = (cfg_xres == 800) ? 800 : (cfg_xres == 1024) ? 1024 : 640;
    win_h = (cfg_xres == 800) ? 600 : (cfg_xres == 1024) ? 768  : 480;

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(win_w, win_h);
    glutCreateWindow("traveller");
    if (cfg_fullscreen)
        glutFullScreen();

    perf_freq = SDL_GetPerformanceFrequency();
    frame_ticks = perf_freq / (Uint64)(cfg_rate > 0 ? cfg_rate : 60);
    /* sub_4063B0: vsync only for sync VBLANK, explicitly off otherwise */
    SDL_GL_SetSwapInterval(cfg_sync == 3 ? 1 : 0);

    init_gl();
    /* zoomer_init happens on the first display callback (auto_detail needs
     * the final drawable size, which reshape reports before then) */

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutPassiveMotionFunc(motion);
    glutMotionFunc(motion);
    glutKeyboardFunc(keyboard);
    glutSpecialFunc(special);
    glutIdleFunc(idle);
    build_menu();

    glutMainLoop();
    return 0;
}
