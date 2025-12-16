"""
Simulator Devices

Terminal output and image file output for testing without hardware.
"""

import base64
from pathlib import Path
from typing import Optional

import numpy as np

from .base import BaseDevice


# Display configuration
DISPLAY_WIDTH = 128
DISPLAY_HEIGHT = 32


class TerminalDevice(BaseDevice):
    """
    Terminal-based simulator.
    Displays LED matrix output using ASCII art in the terminal.
    """
    
    def __init__(self, use_color: bool = True):
        """
        Initialize terminal device.
        
        Args:
            use_color: Use ANSI color codes
        """
        self.use_color = use_color
        self._connected = False
        self._frame_count = 0
    
    def connect(self) -> bool:
        """Connect (no-op for terminal)."""
        self._connected = True
        print("Terminal simulator connected")
        print(f"Display size: {DISPLAY_WIDTH}x{DISPLAY_HEIGHT}")
        return True
    
    def disconnect(self):
        """Disconnect (no-op for terminal)."""
        self._connected = False
        # Clear terminal formatting
        print("\033[0m")
    
    def send(self, data: bytes, wait_ack: bool = True) -> bool:
        """Display frame in terminal."""
        if not self._connected:
            return False
        
        try:
            # Decode Base64
            b64_data = data.rstrip(b'\n')
            raw_data = base64.b64decode(b64_data)
            
            # Convert to RGB565 array
            rgb565 = np.frombuffer(raw_data, dtype='<u2').reshape(
                DISPLAY_HEIGHT, DISPLAY_WIDTH
            )
            
            # Convert to RGB888
            r = ((rgb565 >> 11) & 0x1F) << 3
            g = ((rgb565 >> 5) & 0x3F) << 2
            b = (rgb565 & 0x1F) << 3
            
            # Clear screen and move cursor
            print("\033[H\033[J", end='')
            
            # Display using block characters
            # Process 2 rows at a time using half blocks
            for y in range(0, DISPLAY_HEIGHT, 2):
                line = ""
                for x in range(DISPLAY_WIDTH):
                    r_top, g_top, b_top = r[y, x], g[y, x], b[y, x]
                    
                    if y + 1 < DISPLAY_HEIGHT:
                        r_bot, g_bot, b_bot = r[y+1, x], g[y+1, x], b[y+1, x]
                    else:
                        r_bot, g_bot, b_bot = 0, 0, 0
                    
                    if self.use_color:
                        # Use ANSI 24-bit color
                        # Upper half block with top color foreground, bottom color background
                        line += f"\033[38;2;{r_top};{g_top};{b_top}m"
                        line += f"\033[48;2;{r_bot};{g_bot};{b_bot}m▀"
                    else:
                        # Simple brightness-based ASCII
                        brightness = (int(r_top) + int(g_top) + int(b_top)) // 3
                        chars = " .:-=+*#%@"
                        idx = min(len(chars) - 1, brightness * len(chars) // 256)
                        line += chars[idx]
                
                print(line + "\033[0m")
            
            self._frame_count += 1
            return True
            
        except Exception as e:
            print(f"Terminal display error: {e}")
            return False
    
    @property
    def is_connected(self) -> bool:
        return self._connected


class ImageDevice(BaseDevice):
    """
    Image file output device.
    Saves frames as PNG images with LED-like rendering.
    """
    
    def __init__(
        self,
        output_dir: str = "output",
        scale: int = 10,
        led_style: bool = True
    ):
        """
        Initialize image device.
        
        Args:
            output_dir: Output directory for images
            scale: Pixel scale factor
            led_style: Render with LED-like circular pixels
        """
        self.output_dir = Path(output_dir)
        self.scale = scale
        self.led_style = led_style
        self._connected = False
        self._frame_count = 0
        
        try:
            import cv2
            self._cv2 = cv2
        except ImportError:
            raise ImportError("OpenCV required: pip install opencv-python")
    
    def connect(self) -> bool:
        """Create output directory."""
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self._connected = True
        self._frame_count = 0
        print(f"Image output: {self.output_dir.absolute()}")
        return True
    
    def disconnect(self):
        """Finalize output."""
        self._connected = False
        print(f"Saved {self._frame_count} frames")
    
    def send(self, data: bytes, wait_ack: bool = True) -> bool:
        """Save frame as image."""
        if not self._connected:
            return False
        
        try:
            # Decode Base64
            b64_data = data.rstrip(b'\n')
            raw_data = base64.b64decode(b64_data)
            
            # Convert to RGB565 array
            rgb565 = np.frombuffer(raw_data, dtype='<u2').reshape(
                DISPLAY_HEIGHT, DISPLAY_WIDTH
            )
            
            # Convert to RGB888
            r = ((rgb565 >> 11) & 0x1F) << 3
            g = ((rgb565 >> 5) & 0x3F) << 2
            b = (rgb565 & 0x1F) << 3
            
            image = np.stack([r, g, b], axis=-1).astype(np.uint8)
            
            # Render with LED style
            if self.led_style:
                output = self._render_led_style(image)
            else:
                output = self._cv2.resize(
                    image,
                    (DISPLAY_WIDTH * self.scale, DISPLAY_HEIGHT * self.scale),
                    interpolation=self._cv2.INTER_NEAREST
                )
            
            # Convert RGB to BGR for OpenCV
            output = self._cv2.cvtColor(output, self._cv2.COLOR_RGB2BGR)
            
            # Save
            filename = self.output_dir / f"frame_{self._frame_count:06d}.png"
            self._cv2.imwrite(str(filename), output)
            
            self._frame_count += 1
            return True
            
        except Exception as e:
            print(f"Image save error: {e}")
            return False
    
    def _render_led_style(self, image: np.ndarray) -> np.ndarray:
        """Render with LED-like circular pixels and glow effect."""
        h, w = image.shape[:2]
        s = self.scale
        
        # Add border
        border = 20
        output_h = h * s + border * 2
        output_w = w * s + border * 2
        
        output = np.zeros((output_h, output_w, 3), dtype=np.uint8)
        
        # Draw LEDs
        led_radius = s // 2 - 1
        
        for y in range(h):
            for x in range(w):
                r, g, b = image[y, x]
                
                if r > 10 or g > 10 or b > 10:  # Skip very dark pixels
                    cx = border + x * s + s // 2
                    cy = border + y * s + s // 2
                    
                    # Glow effect (larger, dimmer circle)
                    glow_color = (int(r * 0.3), int(g * 0.3), int(b * 0.3))
                    self._cv2.circle(output, (cx, cy), led_radius + 2, glow_color, -1)
                    
                    # Main LED
                    led_color = (int(r), int(g), int(b))
                    self._cv2.circle(output, (cx, cy), led_radius, led_color, -1)
                    
                    # Highlight (center bright spot)
                    highlight_color = (
                        min(255, int(r * 1.2)),
                        min(255, int(g * 1.2)),
                        min(255, int(b * 1.2))
                    )
                    self._cv2.circle(output, (cx, cy), led_radius // 2, highlight_color, -1)
        
        return output
    
    @property
    def is_connected(self) -> bool:
        return self._connected
