#!/usr/bin/env python3
"""
IMA Traveller - Reverse Engineered Implementation
==================================================
Original software by Driessens & Verstappen (1996)
Reverse engineered from Windows executable using Ghidra/Hex-Rays decompilation.

Algorithm Overview:
------------------
The IMA algorithm breeds pixels. Each pixel divides into four children whose
appearance is defined by interaction with surrounding cells. The process creates
unpredictable constellations with coherent organic structure.

Technical Details:
-----------------
- Internal pixel format: HSV555 (not RGB!)
  - bits 10-14: V (Value/brightness)
  - bits 5-9:  S (Saturation)
  - bits 0-4:  H (Hue)
  
- Each evolution step:
  1. Reads a quarter-size region centered on focus point
  2. Each parent pixel produces 4 children (2x zoom)
  3. Children inherit from closest neighbors with small mutations
  
- Neighbor selection: For each child, compare 3 specific neighbors to center,
  select the one closest in value (per channel)
  
- Mutations: Small random adjustments from a 16x16 table (-7 to +7)

- RNG: Lagged Fibonacci (24, 55) with Park-Miller initialization

Usage:
------
    python ima_traveller_final.py [options]
    
    Options:
        --width W       Image width (default: 256)
        --height H      Image height (default: 256)
        --seed S        Random seed (default: 12345)
        --frames N      Number of frames to generate (default: 60)
        --output FILE   Output filename (default: ima_output.gif)
        --fps FPS       Frames per second for GIF (default: 10)
        --save-frames   Also save individual PNG frames
"""

import argparse
import numpy as np
from dataclasses import dataclass
from typing import Tuple, Optional
from pathlib import Path

# =============================================================================
# MUTATION TABLE - Extracted from decompiled code (FUN_004039f0)
# 16x16 signed integers, symmetric pattern with zeros in center
# =============================================================================
MUTATION_TABLE = np.array([
    [ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0],
    [-1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1],
    [-1, -1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1],
    [-2, -1, -1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  2],
    [-2, -2, -1, -1,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  2,  2],
    [-2, -2, -2, -1, -1,  0,  0,  0,  0,  0,  0,  1,  1,  1,  2,  2],
    [-3, -2, -2, -1, -1, -1,  0,  0,  0,  0,  1,  1,  1,  2,  2,  3],
    [-3, -3, -2, -2, -1, -1, -1,  0,  0,  1,  1,  1,  2,  2,  3,  3],
    [-4, -3, -3, -2, -1, -1, -1, -1,  1,  1,  1,  1,  2,  3,  3,  4],
    [-4, -4, -3, -2, -2, -1, -1, -1,  1,  1,  1,  2,  2,  3,  4,  4],
    [-5, -4, -3, -2, -2, -2, -1, -1,  1,  1,  2,  2,  2,  3,  4,  5],
    [-5, -4, -3, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  3,  4,  5],
    [-6, -4, -3, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  3,  4,  6],
    [-6, -5, -4, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  4,  5,  6],
    [-7, -5, -4, -3, -2, -2, -1, -1,  1,  1,  2,  2,  3,  4,  5,  7],
    [-7, -6, -5, -4, -3, -2, -2, -1,  1,  2,  2,  3,  4,  5,  6,  7],
], dtype=np.int32)


# =============================================================================
# NEIGHBOR PATTERNS - Extracted from FUN_00402d20, FUN_00402ef0, FUN_00403140
# Each child selects from 3 neighbors; indices into 3x3 neighborhood:
#   [0][1][2]
#   [3][4][5]  <- [4] is center
#   [6][7][8]
# =============================================================================
NEIGHBOR_PATTERNS = [
    (3, 0, 1),  # Child 0 (top-left):     left, top-left, top
    (1, 2, 5),  # Child 1 (top-right):    top, top-right, right
    (7, 6, 3),  # Child 2 (bottom-left):  bottom, bottom-left, left
    (5, 8, 7),  # Child 3 (bottom-right): right, bottom-right, bottom
]


# =============================================================================
# RANDOM BIT POSITIONS - Extracted from decompiled evolution functions
# Different bit ranges of the random value select mutation table columns
# Format: (V_shift, S_shift, H_shift) for each child
# =============================================================================
RAND_SHIFTS = [
    (0, 10, 20),   # Child 0
    (3, 13, 23),   # Child 1
    (5, 15, 25),   # Child 2
    (8, 18, 28),   # Child 3
]


class LaggedFibonacciRNG:
    """
    Lagged Fibonacci PRNG (24, 55) - Extracted from FUN_004039f0
    
    Uses Park-Miller LCG for initialization, then generates values as:
        state[k] = state[k] + state[j]
    with j and k wrapping at 55.
    """
    
    def __init__(self, seed: int = 1):
        self.state = np.zeros(56, dtype=np.uint32)
        self.j = 24
        self.k = 55
        self._initialize(seed)
    
    def _initialize(self, seed: int):
        """Initialize state using Park-Miller LCG (a=48271, m=2^31-1)."""
        temp = seed
        self.state[0] = seed
        
        for i in range(1, 56):
            # Two Park-Miller steps per state value
            temp = (temp % 44488) * 48271 - (temp // 44488) * 3399
            if temp < 1:
                temp += 0x7FFFFFFF
            v1 = temp
            
            temp = (temp % 44488) * 48271 - (temp // 44488) * 3399
            if temp < 1:
                temp += 0x7FFFFFFF
            v2 = temp
            
            self.state[i] = np.uint32((v1 + v2) & 0xFFFFFFFF)
        
        self.j = 24
        self.k = 55
    
    def next(self) -> np.uint32:
        """Generate next random value."""
        sum_val = (int(self.state[self.k]) + int(self.state[self.j])) & 0xFFFFFFFF
        self.state[self.k] = np.uint32(sum_val)
        result = self.state[self.k]
        
        # Decrement indices with wraparound
        self.j = 55 if self.j == 1 else self.j - 1
        self.k = 55 if self.k == 1 else self.k - 1
        
        return result


def hsv_to_rgb(h: int, s: int, v: int) -> Tuple[int, int, int]:
    """
    Convert HSV555 (5-bit each) to RGB888.
    Based on FUN_00401080 from decompiled code.
    
    Args:
        h: Hue (0-31)
        s: Saturation (0-31)
        v: Value (0-31)
    
    Returns:
        (R, G, B) tuple with 8-bit values
    """
    # Scale to standard HSV ranges
    hue = h * 360.0 / 32.0      # 0-360 degrees
    sat = s / 31.0              # 0-1
    val = (v * 8 + 7) / 255.0   # Match original's scaling
    
    if sat == 0:
        gray = int(val * 255)
        return (gray, gray, gray)
    
    hue_sector = int(hue / 60.0) % 6
    f = (hue / 60.0) - int(hue / 60.0)
    
    p = val * (1 - sat)
    q = val * (1 - f * sat)
    t = val * (1 - (1 - f) * sat)
    
    if hue_sector == 0:
        r, g, b = val, t, p
    elif hue_sector == 1:
        r, g, b = q, val, p
    elif hue_sector == 2:
        r, g, b = p, val, t
    elif hue_sector == 3:
        r, g, b = p, q, val
    elif hue_sector == 4:
        r, g, b = t, p, val
    else:
        r, g, b = val, p, q
    
    return (
        max(0, min(255, int(r * 255))),
        max(0, min(255, int(g * 255))),
        max(0, min(255, int(b * 255)))
    )


@dataclass
class IMAConfig:
    """Configuration parameters for IMA Traveller."""
    width: int = 256
    height: int = 256
    seed: int = 12345
    seed_radius: int = 8  # Radius of initial seed region


class IMATraveller:
    """
    IMA Traveller cellular automata engine.
    
    Reverse engineered from the original 1996 Windows executable.
    """
    
    def __init__(self, config: Optional[IMAConfig] = None):
        if config is None:
            config = IMAConfig()
        
        self.config = config
        self.width = config.width
        self.height = config.height
        
        # Double-buffered pixel storage (HSV555 format)
        self.buffer_a = np.zeros((self.height, self.width), dtype=np.uint16)
        self.buffer_b = np.zeros((self.height, self.width), dtype=np.uint16)
        self.current = self.buffer_a
        self.next = self.buffer_b
        
        # Focus point for zoom (determines which region is expanded)
        self.focus_x = self.width // 4
        self.focus_y = self.height // 4
        
        # Random number generator
        self.rng = LaggedFibonacciRNG(config.seed)
        
        # Row offset state for mutation table lookup
        self.row_offset_v = 0
        self.row_offset_s = 0
        self.row_offset_h = 0
        
        # Build HSV to RGB color lookup table
        self._build_color_table()
        
        # Initialize with seed
        self._init_seed()
    
    def _build_color_table(self):
        """Build 32x32x32 HSV to RGB lookup table."""
        self.color_table = np.zeros((32, 32, 32, 3), dtype=np.uint8)
        for v in range(32):
            for s in range(32):
                for h in range(32):
                    self.color_table[v, s, h] = hsv_to_rgb(h, s, v)
    
    def _init_seed(self):
        """Initialize buffer with black background and colored seed region."""
        self.current.fill(0)
        
        center_x = self.width // 2
        center_y = self.height // 2
        radius = self.config.seed_radius
        
        # Create circular seed with varied HSV values
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if dy * dy + dx * dx <= radius * radius:
                    y = center_y + dy
                    x = center_x + dx
                    if 0 <= y < self.height and 0 <= x < self.width:
                        # Generate HSV pixel with good saturation and value
                        rand = int(self.rng.next())
                        h = rand & 0x1F
                        s = 20 + ((rand >> 5) & 0xF)   # High saturation
                        v = 16 + ((rand >> 9) & 0xF)   # Medium-high value
                        s = min(31, s)
                        v = min(31, v)
                        self.current[y, x] = (v << 10) | (s << 5) | h
    
    def set_focus(self, x: int, y: int):
        """
        Set the focus point for zoom.
        
        The focus point determines which region of the image is expanded
        during evolution. Should be in range [1, width//2 - 1].
        """
        half_w = self.width // 2
        half_h = self.height // 2
        self.focus_x = max(1, min(x, half_w - 1))
        self.focus_y = max(1, min(y, half_h - 1))
    
    def _select_neighbor(self, center: int, n1: int, n2: int, n3: int) -> int:
        """
        Select the neighbor closest to center value.
        
        Comparison chain from decompiled code:
        1. Compare n1 vs n2: if |center-n1| >= |center-n2|, winner = n2
        2. Compare winner vs n3: if |center-winner| >= |center-n3|, final = n3
        
        This means: closest wins, with ties favoring n2, then n3.
        """
        d1 = abs(center - n1)
        d2 = abs(center - n2)
        winner = n2 if d1 >= d2 else n1
        
        d_winner = abs(center - winner)
        d3 = abs(center - n3)
        return n3 if d_winner >= d3 else winner
    
    def _evolve_pixel(self, y: int, x: int) -> Tuple[int, int, int, int]:
        """
        Evolve one parent pixel into 4 children.
        
        Returns (child0, child1, child2, child3) as HSV555 values.
        """
        buf = self.current
        
        # Helper to get pixel with boundary clamping
        def get(py: int, px: int) -> int:
            py = max(0, min(self.height - 1, py))
            px = max(0, min(self.width - 1, px))
            return int(buf[py, px])
        
        # Get 3x3 neighborhood
        p = [
            get(y-1, x-1), get(y-1, x), get(y-1, x+1),
            get(y,   x-1), get(y,   x), get(y,   x+1),
            get(y+1, x-1), get(y+1, x), get(y+1, x+1),
        ]
        center = p[4]
        
        # Get random value for this quad
        rand_val = int(self.rng.next())
        
        # Mutation table row indices based on center pixel value
        row_v = (self.row_offset_v + (center >> 6)) & 0xF
        row_s = (self.row_offset_s + (center >> 1)) & 0xF
        row_h = (self.row_offset_h + (center >> 11)) & 0xF
        
        children = []
        
        for child_idx, (n1, n2, n3) in enumerate(NEIGHBOR_PATTERNS):
            v_shift, s_shift, h_shift = RAND_SHIFTS[child_idx]
            
            # Extract V, S, H channels from center and neighbors
            center_v = (center >> 10) & 0x1F
            center_s = (center >> 5) & 0x1F
            center_h = center & 0x1F
            
            n1_v = (p[n1] >> 10) & 0x1F
            n1_s = (p[n1] >> 5) & 0x1F
            n1_h = p[n1] & 0x1F
            
            n2_v = (p[n2] >> 10) & 0x1F
            n2_s = (p[n2] >> 5) & 0x1F
            n2_h = p[n2] & 0x1F
            
            n3_v = (p[n3] >> 10) & 0x1F
            n3_s = (p[n3] >> 5) & 0x1F
            n3_h = p[n3] & 0x1F
            
            # Select closest neighbor for each channel
            sel_v = self._select_neighbor(center_v, n1_v, n2_v, n3_v)
            sel_s = self._select_neighbor(center_s, n1_s, n2_s, n3_s)
            sel_h = self._select_neighbor(center_h, n1_h, n2_h, n3_h)
            
            # Get mutation table column from random bits
            col_v = (rand_val >> v_shift) & 0xF
            col_s = (rand_val >> s_shift) & 0xF
            col_h = (rand_val >> h_shift) & 0xF
            
            # Look up mutations
            mut_v = int(MUTATION_TABLE[row_v, col_v])
            mut_s = int(MUTATION_TABLE[row_s, col_s])
            mut_h = int(MUTATION_TABLE[row_h, col_h])
            
            # Apply mutations with clamping to 5-bit range
            new_v = max(0, min(31, sel_v + mut_v))
            new_s = max(0, min(31, sel_s + mut_s))
            new_h = max(0, min(31, sel_h + mut_h))
            
            # Pack as HSV555
            child = (new_v << 10) | (new_s << 5) | new_h
            children.append(child)
        
        return tuple(children)
    
    def evolve(self):
        """
        Perform one evolution step.
        
        Reads a quarter-size region starting at focus point,
        expands it 2x to fill the buffer.
        """
        half_w = self.width // 2
        half_h = self.height // 2
        src_x = self.focus_x
        src_y = self.focus_y
        
        self.next.fill(0)
        
        for row in range(half_h):
            read_y = max(1, min(self.height - 2, src_y + row))
            
            for col in range(half_w):
                read_x = max(1, min(self.width - 2, src_x + col))
                
                c0, c1, c2, c3 = self._evolve_pixel(read_y, read_x)
                
                dst_y = row * 2
                dst_x = col * 2
                
                self.next[dst_y, dst_x] = c0
                self.next[dst_y, dst_x + 1] = c1
                self.next[dst_y + 1, dst_x] = c2
                self.next[dst_y + 1, dst_x + 1] = c3
        
        # Swap buffers
        self.current, self.next = self.next, self.current
        
        # Update row offsets
        self.row_offset_v = (self.row_offset_v + 1) & 0xF
        self.row_offset_s = (self.row_offset_s + 1) & 0xF
        self.row_offset_h = (self.row_offset_h + 1) & 0xF
    
    def get_rgb_buffer(self) -> np.ndarray:
        """
        Convert internal HSV buffer to RGB for display.
        
        Returns:
            numpy array of shape (height, width, 3) with uint8 RGB values
        """
        rgb = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        v = (self.current >> 10) & 0x1F
        s = (self.current >> 5) & 0x1F
        h = self.current & 0x1F
        
        # Use color table lookup
        for y in range(self.height):
            for x in range(self.width):
                rgb[y, x] = self.color_table[v[y, x], s[y, x], h[y, x]]
        
        return rgb


def main():
    """Command-line interface for IMA Traveller."""
    parser = argparse.ArgumentParser(
        description='IMA Traveller - Reverse engineered cellular automata',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  %(prog)s                          Generate default 60-frame GIF
  %(prog)s --frames 120 --fps 15    Longer, smoother animation
  %(prog)s --seed 42 --save-frames  Different seed, save PNGs too
  %(prog)s --width 512 --height 512 Larger resolution
        '''
    )
    
    parser.add_argument('--width', type=int, default=256,
                        help='Image width (default: 256)')
    parser.add_argument('--height', type=int, default=256,
                        help='Image height (default: 256)')
    parser.add_argument('--seed', type=int, default=12345,
                        help='Random seed (default: 12345)')
    parser.add_argument('--frames', type=int, default=60,
                        help='Number of frames to generate (default: 60)')
    parser.add_argument('--output', type=str, default='ima_output.gif',
                        help='Output filename (default: ima_output.gif)')
    parser.add_argument('--fps', type=int, default=10,
                        help='Frames per second for GIF (default: 10)')
    parser.add_argument('--save-frames', action='store_true',
                        help='Also save individual PNG frames')
    parser.add_argument('--seed-radius', type=int, default=8,
                        help='Radius of initial seed region (default: 8)')
    
    args = parser.parse_args()
    
    # Import PIL here to give better error if not installed
    try:
        from PIL import Image
    except ImportError:
        print("Error: PIL/Pillow is required. Install with: pip install Pillow")
        return 1
    
    # Create configuration
    config = IMAConfig(
        width=args.width,
        height=args.height,
        seed=args.seed,
        seed_radius=args.seed_radius
    )
    
    # Initialize engine
    print(f"Initializing IMA Traveller ({args.width}x{args.height}, seed={args.seed})")
    ima = IMATraveller(config)
    ima.set_focus(args.width // 4, args.height // 4)
    
    # Generate frames
    frames = []
    output_dir = Path(args.output).parent or Path('.')
    base_name = Path(args.output).stem
    
    print(f"Generating {args.frames} frames...")
    for i in range(args.frames):
        rgb = ima.get_rgb_buffer()
        img = Image.fromarray(rgb, 'RGB')
        frames.append(img)
        
        if args.save_frames:
            frame_path = output_dir / f"{base_name}_frame_{i:04d}.png"
            img.save(frame_path)
        
        ima.evolve()
        
        if (i + 1) % 10 == 0 or i == args.frames - 1:
            print(f"  Frame {i + 1}/{args.frames}")
    
    # Save GIF
    duration = 1000 // args.fps  # milliseconds per frame
    frames[0].save(
        args.output,
        save_all=True,
        append_images=frames[1:],
        duration=duration,
        loop=0
    )
    print(f"Saved: {args.output}")
    
    if args.save_frames:
        print(f"Saved {args.frames} individual frames as PNG")
    
    return 0


if __name__ == "__main__":
    exit(main())
