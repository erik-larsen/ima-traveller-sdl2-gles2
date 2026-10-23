# IMA Traveller - Shadertoy Port

## Setup Instructions

1. Go to [shadertoy.com](https://www.shadertoy.com/new)
2. Click the **+** button next to "Image" to add **Buffer A**
3. Paste `buffer-a.glsl` into Buffer A
4. In Buffer A, click **iChannel0** and select **Buffer A** (self-reference)
5. Paste `image.glsl` into the Image tab
6. In Image, click **iChannel0** and select **Buffer A**
7. Click Play

## What's Preserved Exactly

- ✅ Mutation table (16x16, exact values)
- ✅ Neighbor selection patterns per child
- ✅ Neighbor selection logic (closest to center, with tie-breaking)
- ✅ HSV555 bit packing format
- ✅ Random bit shift patterns per child (V, S, H)
- ✅ Mutation row offset calculation from center pixel
- ✅ Row offset incrementing each frame
- ✅ 2x zoom evolution structure
- ✅ HSV to RGB conversion (exact formula)
- ✅ Focus point at (width/4, height/4)
- ✅ Seed radius of 8 pixels

## What's Approximated

- ⚠️ **RNG**: The original uses a sequential Lagged Fibonacci (24,55) PRNG
  seeded with Park-Miller LCG. This cannot be replicated in a parallel
  fragment shader. Replaced with a position-based hash function.

- ⚠️ **Initial seed pattern**: The original generates seed pixels sequentially
  using the RNG. The shader uses a deterministic hash per position.

## Visual Behavior

The shader produces the same *type* of organic, evolving patterns as the
original—the algorithm mechanics are identical. However, due to the RNG
difference, specific frame-by-frame output will differ.

The visual character (color evolution, branching, mutations) should be
indistinguishable from the original to a casual observer.
