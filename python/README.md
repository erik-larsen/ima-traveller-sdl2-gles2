# Python reference (early 2026)

The first working reconstruction of the IMA cellular automaton, written with
Claude Opus from the Ghidra decompilation of `traveller20c.exe`.  It was the
stepping stone to the C port: it established the HSV555 pixels, the
per-child neighbor selection, the 16x16 mutation table and the
lagged-Fibonacci RNG, and showed that the zoom is the automaton itself.

| File | Contents |
|------|----------|
| `ima_traveller_final.py` | Core implementation; writes the generations to a GIF (needs numpy, Pillow) |
| `ima_traveller_v2.py` | Adds smooth zoom between generations and a configurable focus point |
| `ima_traveller_v3.py` | Interactive viewer with mouse steering (needs pygame) |
| `shadertoy/` | GLSL version for shadertoy.com; replaces the sequential RNG with a hash, so frames differ |

```sh
python3 ima_traveller_final.py --frames 60 --output ima.gif
```

## Differences from the C engine

Going back over the decompiled routines for the C port corrected several
details.  `ima_core.c` follows the decompilation; these scripts do not:

| | `ima_core.c` (matches the decompilation) | These scripts |
|---|---|---|
| Bits 10-14 | compared on full 15-bit pixel values, result wraps mod 32 (`FUN_00402d20`) | treated as V: compared as 5 bits, result clamped |
| Seed | 2x2 block of `rand & 0x7FFF` at the center (`FUN_00402400` mode 3) | radius-8 disc with tuned saturation and value |
| RNG init | all 56 slots from Park-Miller pair sums (`FUN_004039f0`) | slot 0 = seed, slots 1-55 from pair sums |
| RNG use | first pixel of each row reuses the previous value (`FUN_004029a0`) | a new value for every pixel |
| Row offsets | periods 3/5/7, from the 11th generation on (`FUN_00402cb0`, `FUN_00402c60`) | +1 every generation |

So the scripts produce the same kind of imagery but not the same pixels, and
they are kept here as a record of the reverse-engineering process, not as a
test oracle.
