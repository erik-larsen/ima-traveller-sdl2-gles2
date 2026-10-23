#!/usr/bin/env python3
"""
IMA Traveller v2.0 - Smooth Zoom Edition
=========================================
Original software by Driessens & Verstappen (1996)
Reverse engineered from Windows executable using Ghidra/Hex-Rays decompilation.

Version 2.0 adds smooth zoom transitions between generations:
- Gradually zoom into the center of the current generation
- At 2x zoom (showing center quarter), evolve to next generation
- Reset zoom - the 4 children fill the same visual space as the zoomed parent
- Creates seamless infinite zoom effect

Algorithm Overview:
------------------
The IMA algorithm breeds pixels. Each pixel divides into four children whose
appearance is defined by interaction with surrounding cells.

Technical Details:
-----------------
- Internal pixel format: HSV555
  - bits 10-14: V (Value/brightness)
  - bits 5-9:  S (Saturation)
  - bits 0-4:  H (Hue)
  
- Each evolution step:
  1. Reads a quarter-size region centered on focus point
  2. Each parent pixel produces 4 children (2x zoom)
  3. Children inherit from closest neighbors with small mutations
"""

import argparse
import numpy as np
from dataclasses import dataclass
from typing import Tuple, Optional, List
from pathlib import Path

# =============================================================================
# MUTATION TABLE - Extracted from decompiled code (FUN_004039f0)
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


# Neighbor patterns for each of the 4 children
NEIGHBOR_PATTERNS = [
    (3, 0, 1),  # Child 0 (top-left):     left, top-left, top
    (1, 2, 5),  # Child 1 (top-right):    top, top-right, right
    (7, 6, 3),  # Child 2 (bottom-left):  bottom, bottom-left, left
    (5, 8, 7),  # Child 3 (bottom-right): right, bottom-right, bottom
]

# Random bit positions for mutation table column selection
RAND_SHIFTS = [
    (0, 10, 20),   # Child 0
    (3, 13, 23),   # Child 1
    (5, 15, 25),   # Child 2
    (8, 18, 28),   # Child 3
]


@dataclass
class IMAConfig:
    width: int = 256
    height: int = 256
    seed: int = 12345
    seed_radius: int = 8


class LaggedFibonacciRNG:
    """
    Lagged Fibonacci PRNG (24, 55) - Matches FUN_004039f0 from decompiled code.
    
    Uses Park-Miller LCG (a=48271) for initialization, then generates values as:
        state[k] = state[k] + state[j]
    with j and k decrementing and wrapping at 1 to 55.
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
            # Two Park-Miller steps per state value (matching FUN_004039f0)
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
    
    def next(self) -> int:
        """Generate next random value."""
        sum_val = (int(self.state[self.k]) + int(self.state[self.j])) & 0xFFFFFFFF
        self.state[self.k] = np.uint32(sum_val)
        result = int(self.state[self.k])
        
        # Decrement indices with wraparound (1-55, not 0-54)
        self.j = 55 if self.j == 1 else self.j - 1
        self.k = 55 if self.k == 1 else self.k - 1
        
        return result


class IMATraveller:
    """Main IMA cellular automaton engine."""
    
    def __init__(self, config: IMAConfig):
        self.config = config
        self.width = config.width
        self.height = config.height
        
        # Double buffer - internal HSV555 format
        self.front_buffer = np.zeros((config.height, config.width), dtype=np.uint16)
        self.back_buffer = np.zeros((config.height, config.width), dtype=np.uint16)
        
        # RNG
        self.rng = LaggedFibonacciRNG(config.seed)
        
        # HSV to RGB lookup table (32x32x32 = 32768 entries)
        self.color_table = self._build_color_table()
        
        # Focus point (center of evolution)
        self.focus_x = config.width // 4
        self.focus_y = config.height // 4
        
        # Row offset counters for mutation table lookup (from FUN_00402cb0)
        # These cycle 0-15 and add variation over generations
        self.row_offset_v = 0
        self.row_offset_s = 0
        self.row_offset_h = 0
        
        # Initialize with seed pattern
        self._init_seed(config.seed_radius)
    
    def _build_color_table(self) -> np.ndarray:
        """Build HSV555 to RGB888 lookup table."""
        table = np.zeros((32, 32, 32, 3), dtype=np.uint8)
        
        for h_idx in range(32):
            for s_idx in range(32):
                for v_idx in range(32):
                    # Convert indices to HSV values
                    h = (h_idx * 8) * 1.4117647  # 0-350 degrees
                    s = (s_idx * 8) / 255.0       # 0-1
                    v = (v_idx * 8 + 7) / 255.0   # ~0.03-1
                    
                    # HSV to RGB
                    if s == 0:
                        r = g = b = v
                    else:
                        h_sector = h / 60.0
                        i = int(h_sector) % 6
                        f = h_sector - int(h_sector)
                        p = v * (1 - s)
                        q = v * (1 - s * f)
                        t = v * (1 - s * (1 - f))
                        
                        if i == 0:   r, g, b = v, t, p
                        elif i == 1: r, g, b = q, v, p
                        elif i == 2: r, g, b = p, v, t
                        elif i == 3: r, g, b = p, q, v
                        elif i == 4: r, g, b = t, p, v
                        else:        r, g, b = v, p, q
                    
                    table[h_idx, s_idx, v_idx] = [
                        int(r * 255), int(g * 255), int(b * 255)
                    ]
        
        return table
    
    def _init_seed(self, radius: int):
        """Initialize with small colored seed region in center."""
        cx, cy = self.width // 2, self.height // 2
        
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if dx*dx + dy*dy <= radius*radius:
                    x, y = cx + dx, cy + dy
                    if 0 <= x < self.width and 0 <= y < self.height:
                        # Generate HSV pixel from single RNG value (matching v1)
                        rand = self.rng.next()
                        h = rand & 0x1F
                        s = 20 + ((rand >> 5) & 0xF)   # High saturation
                        v = 16 + ((rand >> 9) & 0xF)   # Medium-high value
                        s = min(31, s)
                        v = min(31, v)
                        self.front_buffer[y, x] = (v << 10) | (s << 5) | h
    
    def set_focus(self, x: int, y: int):
        """
        Set the focus point for evolution.
        Clamped to valid range [1, width/2-1] per original (FUN_00402890).
        """
        half_w = self.width // 2
        half_h = self.height // 2
        self.focus_x = max(1, min(half_w - 1, x))
        self.focus_y = max(1, min(half_h - 1, y))
    
    def _get_neighbors(self, x: int, y: int) -> np.ndarray:
        """Get 3x3 neighborhood with boundary clamping."""
        neighbors = np.zeros(9, dtype=np.uint16)
        idx = 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                # Clamp to valid range (matching original behavior)
                nx = max(0, min(self.width - 1, x + dx))
                ny = max(0, min(self.height - 1, y + dy))
                neighbors[idx] = self.front_buffer[ny, nx]
                idx += 1
        return neighbors
    
    def _select_neighbor(self, center_val: int, n1_val: int, n2_val: int, n3_val: int) -> int:
        """
        Select the neighbor value closest to center value.
        
        Chain comparison from decompiled code (FUN_00402d20):
        1. Compare n1 vs n2: if |center-n1| >= |center-n2|, winner = n2, else n1
        2. Compare winner vs n3: if |center-winner| >= |center-n3|, final = n3, else winner
        
        Ties favor n2, then n3 (the >= comparison).
        """
        d1 = abs(center_val - n1_val)
        d2 = abs(center_val - n2_val)
        winner = n2_val if d1 >= d2 else n1_val
        
        d_winner = abs(center_val - winner)
        d3 = abs(center_val - n3_val)
        return n3_val if d_winner >= d3 else winner
    
    def _evolve_pixel(self, x: int, y: int, child_idx: int, rand_val: int) -> int:
        """Evolve one child pixel from parent at (x, y)."""
        neighbors = self._get_neighbors(x, y)
        center = int(neighbors[4])
        
        pattern = NEIGHBOR_PATTERNS[child_idx]
        v_shift, s_shift, h_shift = RAND_SHIFTS[child_idx]
        
        # Extract channel values from center
        center_v = (center >> 10) & 0x1F
        center_s = (center >> 5) & 0x1F
        center_h = center & 0x1F
        
        # Get the three neighbors for this child's pattern
        n1, n2, n3 = pattern
        
        # Extract channel values for each neighbor
        n1_v = (int(neighbors[n1]) >> 10) & 0x1F
        n1_s = (int(neighbors[n1]) >> 5) & 0x1F
        n1_h = int(neighbors[n1]) & 0x1F
        
        n2_v = (int(neighbors[n2]) >> 10) & 0x1F
        n2_s = (int(neighbors[n2]) >> 5) & 0x1F
        n2_h = int(neighbors[n2]) & 0x1F
        
        n3_v = (int(neighbors[n3]) >> 10) & 0x1F
        n3_s = (int(neighbors[n3]) >> 5) & 0x1F
        n3_h = int(neighbors[n3]) & 0x1F
        
        # Select closest neighbor for each channel (chain comparison with >= for ties)
        sel_v = self._select_neighbor(center_v, n1_v, n2_v, n3_v)
        sel_s = self._select_neighbor(center_s, n1_s, n2_s, n3_s)
        sel_h = self._select_neighbor(center_h, n1_h, n2_h, n3_h)
        
        # Calculate mutation table row indices (include row offset counters)
        row_v = (self.row_offset_v + (center >> 6)) & 0xF
        row_s = (self.row_offset_s + (center >> 1)) & 0xF
        row_h = (self.row_offset_h + (center >> 11)) & 0xF
        
        # Get mutation table column from random bits
        col_v = (rand_val >> v_shift) & 0xF
        col_s = (rand_val >> s_shift) & 0xF
        col_h = (rand_val >> h_shift) & 0xF
        
        # Look up mutations and apply with clamping
        mut_v = int(MUTATION_TABLE[row_v, col_v])
        mut_s = int(MUTATION_TABLE[row_s, col_s])
        mut_h = int(MUTATION_TABLE[row_h, col_h])
        
        new_v = max(0, min(31, sel_v + mut_v))
        new_s = max(0, min(31, sel_s + mut_s))
        new_h = max(0, min(31, sel_h + mut_h))
        
        # Pack as HSV555
        return (new_v << 10) | (new_s << 5) | new_h
    
    def evolve(self):
        """Evolve one generation: each pixel becomes 4 children."""
        half_w = self.width // 2
        half_h = self.height // 2
        
        for src_y in range(half_h):
            for src_x in range(half_w):
                # Source coordinates with focus offset, clamped to valid range
                # Original clamps focus to [1, width/2-1] to ensure 3x3 kernel fits
                fx = max(1, min(self.width - 2, src_x + self.focus_x))
                fy = max(1, min(self.height - 2, src_y + self.focus_y))
                
                # Get random value for this pixel
                rand_val = self.rng.next()
                
                # Destination coordinates (2x2 block)
                dst_x = src_x * 2
                dst_y = src_y * 2
                
                # Generate 4 children
                self.back_buffer[dst_y, dst_x] = self._evolve_pixel(fx, fy, 0, rand_val)
                self.back_buffer[dst_y, dst_x + 1] = self._evolve_pixel(fx, fy, 1, rand_val)
                self.back_buffer[dst_y + 1, dst_x] = self._evolve_pixel(fx, fy, 2, rand_val)
                self.back_buffer[dst_y + 1, dst_x + 1] = self._evolve_pixel(fx, fy, 3, rand_val)
        
        # Swap buffers
        self.front_buffer, self.back_buffer = self.back_buffer, self.front_buffer
        
        # Increment row offset counters (from FUN_00402cb0)
        self.row_offset_v = (self.row_offset_v + 1) & 0xF
        self.row_offset_s = (self.row_offset_s + 1) & 0xF
        self.row_offset_h = (self.row_offset_h + 1) & 0xF
    
    def get_rgb_buffer(self) -> np.ndarray:
        """Convert current HSV555 buffer to RGB888."""
        rgb = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        h_idx = self.front_buffer & 0x1F
        s_idx = (self.front_buffer >> 5) & 0x1F
        v_idx = (self.front_buffer >> 10) & 0x1F
        
        rgb = self.color_table[h_idx, s_idx, v_idx]
        
        return rgb


def render_zoomed_frame(rgb_buffer: np.ndarray, zoom: float, 
                        output_size: Tuple[int, int],
                        focus_x: float = 0.5, focus_y: float = 0.5,
                        interpolation: str = 'bilinear') -> np.ndarray:
    """
    Render a frame with zoom applied, centered on focus point.
    
    Args:
        rgb_buffer: Source RGB image
        zoom: Zoom factor (1.0 = full image, 2.0 = quarter area scaled 2x)
        output_size: (width, height) of output
        focus_x: Horizontal focus point (0.0 = left, 1.0 = right)
        focus_y: Vertical focus point (0.0 = top, 1.0 = bottom)
        interpolation: 'nearest' or 'bilinear'
    
    Returns:
        Zoomed RGB image
    """
    from PIL import Image
    
    h, w = rgb_buffer.shape[:2]
    out_w, out_h = output_size
    
    # Calculate crop dimensions
    crop_w = w / zoom
    crop_h = h / zoom
    
    # Center crop on focus point, clamped to valid region
    center_x = focus_x * w
    center_y = focus_y * h
    
    x0 = max(0, min(w - crop_w, center_x - crop_w / 2))
    y0 = max(0, min(h - crop_h, center_y - crop_h / 2))
    x1 = x0 + crop_w
    y1 = y0 + crop_h
    
    # Use PIL for crop and resize
    img = Image.fromarray(rgb_buffer, 'RGB')
    cropped = img.crop((x0, y0, x1, y1))
    
    resample = Image.NEAREST if interpolation == 'nearest' else Image.BILINEAR
    scaled = cropped.resize((out_w, out_h), resample)
    
    return np.array(scaled)


def main():
    parser = argparse.ArgumentParser(
        description='IMA Traveller v2.0 - Smooth zoom cellular automata',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  %(prog)s                                Generate default animation
  %(prog)s --generations 10 --zoom-frames 20   10 generations, smooth 20-frame zoom each
  %(prog)s --seed 42 --interpolation nearest   Pixelated zoom style
  %(prog)s --width 512 --height 512            Larger resolution
  %(prog)s --focus-x 0.3 --focus-y 0.7         Off-center focus point
        ''')
    
    parser.add_argument('--width', type=int, default=256,
                        help='Image width (default: 256)')
    parser.add_argument('--height', type=int, default=256,
                        help='Image height (default: 256)')
    parser.add_argument('--seed', type=int, default=12345,
                        help='Random seed (default: 12345)')
    parser.add_argument('--generations', type=int, default=10,
                        help='Number of CA generations (default: 10)')
    parser.add_argument('--zoom-frames', type=int, default=15,
                        help='Frames per zoom cycle (default: 15)')
    parser.add_argument('--output', type=str, default='ima_zoom.gif',
                        help='Output filename (default: ima_zoom.gif)')
    parser.add_argument('--fps', type=int, default=30,
                        help='Frames per second for GIF (default: 30)')
    parser.add_argument('--interpolation', type=str, default='bilinear',
                        choices=['nearest', 'bilinear'],
                        help='Zoom interpolation method (default: bilinear)')
    parser.add_argument('--save-frames', action='store_true',
                        help='Also save individual PNG frames')
    parser.add_argument('--seed-radius', type=int, default=8,
                        help='Radius of initial seed region (default: 8)')
    parser.add_argument('--focus-x', type=float, default=0.5,
                        help='Horizontal focus point 0.0-1.0 (default: 0.5 = center)')
    parser.add_argument('--focus-y', type=float, default=0.5,
                        help='Vertical focus point 0.0-1.0 (default: 0.5 = center)')
    
    args = parser.parse_args()
    
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
    print(f"IMA Traveller v2.0 ({args.width}x{args.height}, seed={args.seed})")
    print(f"Focus point: ({args.focus_x:.2f}, {args.focus_y:.2f})")
    print(f"Generating {args.generations} generations with {args.zoom_frames} zoom frames each")
    
    ima = IMATraveller(config)
    
    # Current focus point (normalized 0-1)
    # Structured to allow dynamic updates in future versions
    focus_x = args.focus_x
    focus_y = args.focus_y
    
    # Generate frames with smooth zoom
    frames = []
    output_dir = Path(args.output).parent or Path('.')
    base_name = Path(args.output).stem
    
    total_frames = args.generations * args.zoom_frames
    frame_count = 0
    
    for gen in range(args.generations):
        # Update CA focus from current focus point
        # Convert normalized (0-1) to CA coordinates (0 to width/2)
        ca_focus_x = int(focus_x * (args.width // 2))
        ca_focus_y = int(focus_y * (args.height // 2))
        ima.set_focus(ca_focus_x, ca_focus_y)
        
        # Get current generation's RGB buffer
        rgb = ima.get_rgb_buffer()
        
        # Generate zoom frames for this generation
        for z in range(args.zoom_frames):
            # Future: focus_x, focus_y could be updated here for dynamic steering
            
            # Zoom from 1.0 to 2.0 over the zoom frames
            # Use exponential interpolation for constant perceived zoom speed
            t = z / args.zoom_frames
            zoom = 2.0 ** t  # 1.0 -> 2.0 exponentially
            
            # Render zoomed frame centered on focus point
            frame_rgb = render_zoomed_frame(
                rgb, zoom, 
                (args.width, args.height),
                focus_x, focus_y,
                args.interpolation
            )
            
            img = Image.fromarray(frame_rgb, 'RGB')
            frames.append(img)
            
            if args.save_frames:
                frame_path = output_dir / f"{base_name}_frame_{frame_count:04d}.png"
                img.save(frame_path)
            
            frame_count += 1
        
        # Evolve to next generation
        ima.evolve()
        
        print(f"  Generation {gen + 1}/{args.generations} complete ({frame_count} frames)")
    
    # Save GIF
    duration = 1000 // args.fps
    frames[0].save(
        args.output,
        save_all=True,
        append_images=frames[1:],
        duration=duration,
        loop=0
    )
    print(f"Saved: {args.output} ({len(frames)} frames)")
    
    if args.save_frames:
        print(f"Saved {len(frames)} individual frames as PNG")
    
    return 0


if __name__ == "__main__":
    exit(main())
