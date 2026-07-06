/* ima_core.h -- the IMA cellular automata engine of "traveller 2.0c"
 * (Driessens & Verstappen, 1996), reverse engineered from the Windows
 * executable via Ghidra/Hex-Rays decompilation.
 *
 * Pure computation, no GL: pixels are 15-bit (H<<10)|(S<<5)|V words, and
 * the palette maps them to RGBA5551 texels exactly like the original.
 * Function comments reference the original's addresses (sub_XXXXXX).
 *
 * Bit-exact with the Python reference (ima-traveller/ima_traveller_final.py):
 * same seed + same focus => identical buffers, verified by CRC32.
 */
#ifndef IMA_CORE_H
#define IMA_CORE_H

typedef struct {
    unsigned int state[56];   /* lagged Fibonacci (24,55) state; [0] unused */
    int          j, k;        /* lag indices, decrement, wrap 1 -> 55      */
    unsigned int lcg;         /* Park-Miller LCG state (init only)         */
} ImaRng;

typedef struct {
    int             width, height;     /* CA size (multiples of 256 in the
                                          original: 768x512/1024x768/1280x1024) */
    unsigned short *front;             /* this[18]: latest image (displayed) */
    unsigned short *back;              /* this[17]: previous image (source)  */
    unsigned short *display;           /* RGBA5551 display image (this[3])   */
    unsigned short  palette[32768];    /* HSV555 -> RGBA5551 (sub_402720)    */
    int             focus_x, focus_y;  /* this[16671/16672]                  */
    unsigned int    gen_counter;       /* this[16670], starts at 1           */
    int             off_h, off_s, off_v;  /* row offsets this[16673..5]      */
    ImaRng          rng;
    unsigned int    rand_val;          /* this[16682]: current random value  */
} Ima;

/* Allocate buffers, build palette, set focus (1,1). width/height even. */
int  ima_create(Ima *m, int width, int height);
void ima_destroy(Ima *m);

/* sub_402400 mode 3: re-init RNG with seed, clear image, write a 2x2 block
 * of 4 random pixels at (h/2-1, w/2-1). */
void ima_seed(Ima *m, unsigned int seed);

/* sub_402890: clamp focus to [1, half-1] per axis. */
void ima_set_focus(Ima *m, int x, int y);

/* sub_402CB0: cycle mutation row offsets, periods 3 (H) / 5 (S) / 7 (V).
 * The original's driver only calls this from the 11th generation on. */
void ima_update_row_offsets(Ima *m);

/* sub_402C50: swap front/back; call at generation start (the latest image
 * becomes the evolution source). */
void ima_swap(Ima *m);

/* sub_4029A0: evolve source rows y0..y1 (of the half-size focus region in
 * back) into destination rows 2*y0..2*y1+1 of front (each parent -> 2x2
 * children). Full generation = rows 0..height/2-1. */
void ima_evolve_rows(Ima *m, int y0, int y1);

/* sub_402920: palette-convert front rows y0..y1 into display. */
void ima_convert_rows(Ima *m, int y0, int y1);

#endif /* IMA_CORE_H */
