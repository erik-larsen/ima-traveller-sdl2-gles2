#!/usr/bin/env python3
"""
IMA Traveller v3.0 - Interactive Edition
=========================================
Original software by Driessens & Verstappen (1996)

Interactive version with mouse-controlled focus point.
Move the mouse to steer where the infinite zoom goes.

Controls:
  Mouse     - Move focus point (where zoom/evolution centers)
  Space     - Pause/resume
  R         - Reset to initial state
  +/-       - Adjust zoom speed
  ESC/Q     - Quit
"""

import numpy as np
import pygame
from dataclasses import dataclass
from typing import Tuple

# =============================================================================
# MUTATION TABLE
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

NEIGHBOR_PATTERNS = [
    (3, 0, 1),  # Child 0 (top-left)
    (1, 2, 5),  # Child 1 (top-right)
    (7, 6, 3),  # Child 2 (bottom-left)
    (5, 8, 7),  # Child 3 (bottom-right)
]

RAND_SHIFTS = [
    (0, 10, 20),
    (3, 13, 23),
    (5, 15, 25),
    (8, 18, 28),
]


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
    def __init__(self, width: int = 256, height: int = 256, seed: int = 12345):
        self.width = width
        self.height = height
        self.seed = seed
        
        self.front_buffer = np.zeros((height, width), dtype=np.uint16)
        self.back_buffer = np.zeros((height, width), dtype=np.uint16)
        
        self.rng = LaggedFibonacciRNG(seed)
        self.color_table = self._build_color_table()
        
        self.focus_x = width // 4
        self.focus_y = height // 4
        
        # Row offset counters for mutation table lookup
        self.row_offset_v = 0
        self.row_offset_s = 0
        self.row_offset_h = 0
        
        self._init_seed(8)
    
    def reset(self):
        """Reset to initial state."""
        self.front_buffer.fill(0)
        self.back_buffer.fill(0)
        self.rng = LaggedFibonacciRNG(self.seed)
        self.row_offset_v = 0
        self.row_offset_s = 0
        self.row_offset_h = 0
        self._init_seed(8)
    
    def _build_color_table(self) -> np.ndarray:
        table = np.zeros((32, 32, 32, 3), dtype=np.uint8)
        
        for h_idx in range(32):
            for s_idx in range(32):
                for v_idx in range(32):
                    h = (h_idx * 8) * 1.4117647
                    s = (s_idx * 8) / 255.0
                    v = (v_idx * 8 + 7) / 255.0
                    
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
        """Set focus point (clamped to valid range [1, size/2-1])."""
        half_w = self.width // 2
        half_h = self.height // 2
        self.focus_x = max(1, min(half_w - 1, x))
        self.focus_y = max(1, min(half_h - 1, y))
    
    def set_focus_normalized(self, nx: float, ny: float):
        """Set focus from normalized coordinates (0-1)."""
        half_w = self.width // 2
        half_h = self.height // 2
        x = int(nx * half_w)
        y = int(ny * half_h)
        self.focus_x = max(1, min(half_w - 1, x))
        self.focus_y = max(1, min(half_h - 1, y))
    
    def _get_neighbors(self, x: int, y: int) -> np.ndarray:
        """Get 3x3 neighborhood with boundary clamping."""
        neighbors = np.zeros(9, dtype=np.uint16)
        idx = 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx = max(0, min(self.width - 1, x + dx))
                ny = max(0, min(self.height - 1, y + dy))
                neighbors[idx] = self.front_buffer[ny, nx]
                idx += 1
        return neighbors
    
    def _select_neighbor(self, center_val: int, n1_val: int, n2_val: int, n3_val: int) -> int:
        """
        Select neighbor value closest to center using chain comparison.
        Ties favor n2, then n3 (using >= comparison).
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
        
        # Select closest neighbor for each channel
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
        """Evolve one generation."""
        half_w = self.width // 2
        half_h = self.height // 2
        
        for src_y in range(half_h):
            for src_x in range(half_w):
                # Source coordinates with focus offset, clamped to valid range
                fx = max(1, min(self.width - 2, src_x + self.focus_x))
                fy = max(1, min(self.height - 2, src_y + self.focus_y))
                
                rand_val = self.rng.next()
                
                dst_x = src_x * 2
                dst_y = src_y * 2
                
                self.back_buffer[dst_y, dst_x] = self._evolve_pixel(fx, fy, 0, rand_val)
                self.back_buffer[dst_y, dst_x + 1] = self._evolve_pixel(fx, fy, 1, rand_val)
                self.back_buffer[dst_y + 1, dst_x] = self._evolve_pixel(fx, fy, 2, rand_val)
                self.back_buffer[dst_y + 1, dst_x + 1] = self._evolve_pixel(fx, fy, 3, rand_val)
        
        self.front_buffer, self.back_buffer = self.back_buffer, self.front_buffer
        
        # Increment row offset counters
        self.row_offset_v = (self.row_offset_v + 1) & 0xF
        self.row_offset_s = (self.row_offset_s + 1) & 0xF
        self.row_offset_h = (self.row_offset_h + 1) & 0xF
    
    def get_rgb_array(self) -> np.ndarray:
        """Get RGB array (height, width, 3)."""
        h_idx = self.front_buffer & 0x1F
        s_idx = (self.front_buffer >> 5) & 0x1F
        v_idx = (self.front_buffer >> 10) & 0x1F
        return self.color_table[h_idx, s_idx, v_idx]


class IMAViewer:
    """Interactive pygame viewer with smooth zoom and mouse steering."""
    
    def __init__(self, width: int = 512, height: int = 512, 
                 ca_size: int = 256, seed: int = 12345):
        pygame.init()
        
        self.width = width
        self.height = height
        self.ca_size = ca_size
        
        self.screen = pygame.display.set_mode((width, height))
        pygame.display.set_caption("IMA Traveller - Move mouse to steer")
        
        self.clock = pygame.time.Clock()
        
        # CA engine
        self.ima = IMATraveller(ca_size, ca_size, seed)
        
        # Zoom state
        self.zoom = 1.0
        self.zoom_speed = 0.02  # How fast zoom increases per frame
        self.max_zoom = 2.0
        
        # Focus (normalized 0-1)
        self.focus_nx = 0.5
        self.focus_ny = 0.5
        
        # Smoothing for mouse input
        self.target_focus_nx = 0.5
        self.target_focus_ny = 0.5
        self.focus_smoothing = 0.05
        
        self.paused = False
        self.running = True
        self.generation = 0
    
    def handle_events(self):
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                self.running = False
            elif event.type == pygame.KEYDOWN:
                if event.key in (pygame.K_ESCAPE, pygame.K_q):
                    self.running = False
                elif event.key == pygame.K_SPACE:
                    self.paused = not self.paused
                elif event.key == pygame.K_r:
                    self.ima.reset()
                    self.zoom = 1.0
                    self.generation = 0
                elif event.key in (pygame.K_PLUS, pygame.K_EQUALS):
                    self.zoom_speed = min(0.1, self.zoom_speed * 1.2)
                elif event.key == pygame.K_MINUS:
                    self.zoom_speed = max(0.005, self.zoom_speed / 1.2)
        
        # Update target focus from mouse position
        mx, my = pygame.mouse.get_pos()
        self.target_focus_nx = mx / self.width
        self.target_focus_ny = my / self.height
    
    def update(self):
        if self.paused:
            return
        
        # Smooth focus tracking
        self.focus_nx += (self.target_focus_nx - self.focus_nx) * self.focus_smoothing
        self.focus_ny += (self.target_focus_ny - self.focus_ny) * self.focus_smoothing
        
        # Update zoom
        self.zoom *= (1.0 + self.zoom_speed)
        
        # Check if we hit max zoom - time to evolve
        if self.zoom >= self.max_zoom:
            # Set CA focus based on current mouse position
            self.ima.set_focus_normalized(self.focus_nx, self.focus_ny)
            
            # Evolve to next generation
            self.ima.evolve()
            self.generation += 1
            
            # Reset zoom
            self.zoom = 1.0
    
    def render(self):
        # Get current CA state as RGB
        rgb = self.ima.get_rgb_array()
        
        # Apply zoom (crop and scale)
        h, w = rgb.shape[:2]
        crop_w = w / self.zoom
        crop_h = h / self.zoom
        
        # Center crop on focus point
        center_x = self.focus_nx * w
        center_y = self.focus_ny * h
        
        x0 = max(0, min(w - crop_w, center_x - crop_w / 2))
        y0 = max(0, min(h - crop_h, center_y - crop_h / 2))
        x1 = x0 + crop_w
        y1 = y0 + crop_h
        
        # Crop
        x0i, y0i = int(x0), int(y0)
        x1i, y1i = int(x1), int(y1)
        cropped = rgb[y0i:y1i, x0i:x1i]
        
        # Create pygame surface from cropped array
        # Need to transpose for pygame (it expects width, height, channels)
        surf = pygame.surfarray.make_surface(cropped.swapaxes(0, 1))
        
        # Scale to screen size
        scaled = pygame.transform.scale(surf, (self.width, self.height))
        
        self.screen.blit(scaled, (0, 0))
        
        # Draw info text
        font = pygame.font.Font(None, 24)
        info = f"Gen: {self.generation}  Zoom: {self.zoom:.2f}x  Speed: {self.zoom_speed:.3f}"
        if self.paused:
            info += "  [PAUSED]"
        text = font.render(info, True, (255, 255, 255))
        # Draw with shadow for visibility
        shadow = font.render(info, True, (0, 0, 0))
        self.screen.blit(shadow, (11, 11))
        self.screen.blit(text, (10, 10))
        
        pygame.display.flip()
    
    def run(self):
        while self.running:
            self.handle_events()
            self.update()
            self.render()
            self.clock.tick(60)
        
        pygame.quit()


def main():
    import argparse
    
    parser = argparse.ArgumentParser(
        description='IMA Traveller v3.0 - Interactive mouse-controlled version',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Controls:
  Mouse     - Steer focus point (where zoom/evolution centers)
  Space     - Pause/resume
  R         - Reset to initial state
  +/-       - Adjust zoom speed
  ESC/Q     - Quit
        ''')
    
    parser.add_argument('--width', type=int, default=512,
                        help='Window width (default: 512)')
    parser.add_argument('--height', type=int, default=512,
                        help='Window height (default: 512)')
    parser.add_argument('--ca-size', type=int, default=256,
                        help='CA grid size (default: 256)')
    parser.add_argument('--seed', type=int, default=12345,
                        help='Random seed (default: 12345)')
    
    args = parser.parse_args()
    
    print("IMA Traveller v3.0 - Interactive Edition")
    print("Move mouse to steer the infinite zoom")
    print("Press Space to pause, R to reset, ESC to quit")
    
    viewer = IMAViewer(
        width=args.width,
        height=args.height,
        ca_size=args.ca_size,
        seed=args.seed
    )
    viewer.run()


if __name__ == "__main__":
    main()
