"""
LED Matrix Controller

Main controller class that handles image/video processing
and communication with the LED matrix panel.
"""

import time
import base64
import math
from typing import Tuple, Optional, Union
from pathlib import Path

import numpy as np

try:
    import cv2
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False

from .devices.base import BaseDevice


# Display configuration
DISPLAY_WIDTH = 128
DISPLAY_HEIGHT = 32

# Binary mode magic header (must match firmware)
BINARY_MAGIC = bytes([0xFF, 0x00])


class LEDMatrixController:
    """Controller for 128x32 HUB75 LED Matrix panel."""
    
    def __init__(
        self,
        device: BaseDevice,
        width: int = DISPLAY_WIDTH,
        height: int = DISPLAY_HEIGHT,
        brightness: float = 1.0,
        binary_mode: bool = True
    ):
        """
        Initialize controller.

        Args:
            device: Output device (SerialDevice, TerminalDevice, etc.)
            width: Display width in pixels
            height: Display height in pixels
            brightness: Brightness multiplier (0.0-1.0)
            binary_mode: Use binary transfer (faster) instead of Base64
        """
        self.device = device
        self.width = width
        self.height = height
        self.brightness = max(0.0, min(1.0, brightness))
        self.binary_mode = binary_mode

        # FPS tracking
        self._frame_count = 0
        self._fps_start = time.time()
        self._current_fps = 0.0
    
    def connect(self) -> bool:
        """Connect to the device."""
        return self.device.connect()
    
    def disconnect(self):
        """Disconnect from the device."""
        self.device.disconnect()
    
    def _apply_brightness(self, image: np.ndarray) -> np.ndarray:
        """Apply brightness adjustment."""
        if self.brightness < 1.0:
            return (image * self.brightness).astype(np.uint8)
        return image
    
    def _resize_image(self, image: np.ndarray, fit_mode: str = "fit") -> np.ndarray:
        """
        Resize image to display dimensions with aspect ratio preservation.
        
        Args:
            image: Input image (H, W, 3) RGB
            fit_mode: "fit" (letterbox), "fill" (crop), or "stretch"
        """
        if not HAS_CV2:
            raise ImportError("OpenCV is required for image processing")
        
        h, w = image.shape[:2]
        target_w, target_h = self.width, self.height
        
        if fit_mode == "stretch":
            return cv2.resize(image, (target_w, target_h))
        
        # Calculate scaling
        scale_w = target_w / w
        scale_h = target_h / h
        
        if fit_mode == "fit":
            # Fit entire image (letterbox)
            scale = min(scale_w, scale_h)
        else:  # fill
            # Fill entire display (crop)
            scale = max(scale_w, scale_h)
        
        new_w = int(w * scale)
        new_h = int(h * scale)
        
        # Resize
        resized = cv2.resize(image, (new_w, new_h), interpolation=cv2.INTER_AREA)
        
        # Create output canvas
        output = np.zeros((target_h, target_w, 3), dtype=np.uint8)
        
        # Center the image
        x_offset = (target_w - new_w) // 2
        y_offset = (target_h - new_h) // 2
        
        # Crop if necessary (for fill mode)
        src_x = max(0, -x_offset)
        src_y = max(0, -y_offset)
        dst_x = max(0, x_offset)
        dst_y = max(0, y_offset)
        
        copy_w = min(new_w - src_x, target_w - dst_x)
        copy_h = min(new_h - src_y, target_h - dst_y)
        
        output[dst_y:dst_y+copy_h, dst_x:dst_x+copy_w] = \
            resized[src_y:src_y+copy_h, src_x:src_x+copy_w]
        
        return output
    
    def _rgb_to_rgb565(self, image: np.ndarray) -> np.ndarray:
        """Convert RGB888 to RGB565."""
        r = (image[:, :, 0] >> 3).astype(np.uint16)
        g = (image[:, :, 1] >> 2).astype(np.uint16)
        b = (image[:, :, 2] >> 3).astype(np.uint16)
        return (r << 11) | (g << 5) | b
    
    def _encode_frame(self, image: np.ndarray) -> bytes:
        """
        Encode image for transmission.

        Args:
            image: RGB image (H, W, 3) uint8

        Returns:
            Encoded bytes (binary or Base64 depending on mode)
        """
        # Resize to display dimensions
        if image.shape[:2] != (self.height, self.width):
            image = self._resize_image(image)

        # Apply brightness
        image = self._apply_brightness(image)

        # Horizontal flip for HUB75 shift register order
        # Data is shifted right-to-left, last pixel shifted stays at left edge
        image = np.fliplr(image)

        # Convert to RGB565
        rgb565 = self._rgb_to_rgb565(image)

        # Convert to bytes (little-endian)
        data = rgb565.astype('<u2').tobytes()

        if self.binary_mode:
            # Binary mode: magic header + raw data (no newline needed)
            return BINARY_MAGIC + data
        else:
            # Base64 mode: encoded data + newline
            b64_data = base64.b64encode(data)
            return b64_data + b'\n'
    
    def _update_fps(self):
        """Update FPS counter."""
        self._frame_count += 1
        elapsed = time.time() - self._fps_start
        if elapsed >= 1.0:
            self._current_fps = self._frame_count / elapsed
            self._frame_count = 0
            self._fps_start = time.time()
    
    @property
    def fps(self) -> float:
        """Get current FPS."""
        return self._current_fps
    
    def send_frame(self, image: np.ndarray, wait_ack: bool = True) -> bool:
        """
        Send a frame to the display.
        
        Args:
            image: RGB image (any size, will be resized)
            wait_ack: Wait for acknowledgment
            
        Returns:
            True if successful
        """
        encoded = self._encode_frame(image)
        result = self.device.send(encoded, wait_ack=wait_ack)
        
        if result:
            self._update_fps()
        
        return result
    
    def fill(self, color: Tuple[int, int, int]):
        """Fill display with solid color."""
        image = np.full((self.height, self.width, 3), color, dtype=np.uint8)
        self.send_frame(image)
    
    def clear(self):
        """Clear display (fill with black)."""
        self.fill((0, 0, 0))
    
    # ========================================
    # Input Modes
    # ========================================
    
    def display_image(self, path: Union[str, Path]):
        """Display a static image."""
        if not HAS_CV2:
            raise ImportError("OpenCV required: pip install opencv-python")
        
        image = cv2.imread(str(path))
        if image is None:
            raise ValueError(f"Cannot load image: {path}")
        
        # Convert BGR to RGB
        image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        self.send_frame(image)
    
    def play_video(
        self,
        path: Union[str, Path],
        loop: bool = False,
        max_fps: bool = False,
        wait_ack: bool = True
    ):
        """
        Play a video file.

        Args:
            path: Path to video file
            loop: Loop playback
            max_fps: If True, ignore video FPS and send as fast as possible
            wait_ack: Wait for ACK after each frame (disable for higher FPS)
        """
        if not HAS_CV2:
            raise ImportError("OpenCV required: pip install opencv-python")

        cap = cv2.VideoCapture(str(path))
        if not cap.isOpened():
            raise ValueError(f"Cannot open video: {path}")

        video_fps = cap.get(cv2.CAP_PROP_FPS) or 30
        frame_time = 1.0 / video_fps

        mode_str = "binary" if self.binary_mode else "base64"
        ack_str = "sync" if wait_ack else "async"
        fps_str = "max" if max_fps else f"{video_fps:.1f}"

        print(f"Playing: {path}")
        print(f"Mode: {mode_str}, ACK: {ack_str}, Target FPS: {fps_str}")
        print("Press Ctrl+C to stop")

        try:
            while True:
                ret, frame = cap.read()
                if not ret:
                    if loop:
                        cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                        continue
                    else:
                        break

                frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

                start = time.time()
                self.send_frame(frame, wait_ack=wait_ack)

                # Maintain frame rate (unless max_fps mode)
                if not max_fps:
                    elapsed = time.time() - start
                    if elapsed < frame_time:
                        time.sleep(frame_time - elapsed)

                # Print FPS periodically
                if self._frame_count == 0:
                    print(f"\rFPS: {self.fps:.1f}  ", end='', flush=True)

        finally:
            cap.release()
            print()
    
    # ========================================
    # Demo Modes
    # ========================================
    
    def run_demo(self, demo_name: str, fps: float = 30.0):
        """Run a demo animation."""
        demos = {
            "rainbow": self._demo_rainbow,
            "gradient": self._demo_gradient,
            "plasma": self._demo_plasma,
            "fire": self._demo_fire,
            "matrix": self._demo_matrix,
            "clock": self._demo_clock,
        }
        
        if demo_name not in demos:
            raise ValueError(f"Unknown demo: {demo_name}")
        
        print(f"Running demo: {demo_name}")
        print("Press Ctrl+C to stop")
        
        frame_time = 1.0 / fps
        
        try:
            t = 0.0
            while True:
                start = time.time()
                
                image = demos[demo_name](t)
                self.send_frame(image)
                
                t += frame_time
                
                elapsed = time.time() - start
                if elapsed < frame_time:
                    time.sleep(frame_time - elapsed)
                
                if self._frame_count == 0:
                    print(f"\rFPS: {self.fps:.1f}  ", end='', flush=True)
                    
        except KeyboardInterrupt:
            print()
    
    def _demo_rainbow(self, t: float) -> np.ndarray:
        """Rainbow gradient animation."""
        import colorsys
        
        image = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        for x in range(self.width):
            hue = (t * 0.5 + x / self.width) % 1.0
            r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
            image[:, x] = [int(r * 255), int(g * 255), int(b * 255)]
        
        return image
    
    def _demo_gradient(self, t: float) -> np.ndarray:
        """Color gradient animation."""
        import colorsys
        
        image = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        for y in range(self.height):
            for x in range(self.width):
                hue = (t * 0.2 + x / self.width * 0.5 + y / self.height * 0.5) % 1.0
                r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
                image[y, x] = [int(r * 255), int(g * 255), int(b * 255)]
        
        return image
    
    def _demo_plasma(self, t: float) -> np.ndarray:
        """Plasma effect."""
        import colorsys
        
        image = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        for y in range(self.height):
            for x in range(self.width):
                v1 = math.sin(x / 16.0 + t * 2)
                v2 = math.sin((y / 8.0 + t) * 2)
                v3 = math.sin((x / 16.0 + y / 16.0 + t) * 2)
                v4 = math.sin(math.sqrt(x*x + y*y) / 8.0 + t * 3)
                
                v = (v1 + v2 + v3 + v4) / 4.0
                hue = (v + 1.0) / 2.0
                
                r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
                image[y, x] = [int(r * 255), int(g * 255), int(b * 255)]
        
        return image
    
    def _demo_fire(self, t: float) -> np.ndarray:
        """Fire effect."""
        import random
        
        # Initialize or get fire buffer
        if not hasattr(self, '_fire_buffer'):
            self._fire_buffer = np.zeros((self.height + 2, self.width), dtype=np.float32)
        
        buf = self._fire_buffer
        
        # Generate heat at bottom
        for x in range(self.width):
            buf[self.height + 1, x] = random.random() * 255
        
        # Propagate fire upward
        for y in range(self.height, 0, -1):
            for x in range(self.width):
                decay = random.random() * 3
                x1 = max(0, x - 1)
                x2 = min(self.width - 1, x + 1)
                
                buf[y-1, x] = max(0, (buf[y, x1] + buf[y, x] + buf[y, x2] + buf[y+1, x]) / 4.0 - decay)
        
        # Convert to RGB
        image = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        for y in range(self.height):
            for x in range(self.width):
                v = int(buf[y, x])
                if v < 85:
                    image[y, x] = [v * 3, 0, 0]
                elif v < 170:
                    image[y, x] = [255, (v - 85) * 3, 0]
                else:
                    image[y, x] = [255, 255, (v - 170) * 3]
        
        return image
    
    def _demo_matrix(self, t: float) -> np.ndarray:
        """Matrix rain effect."""
        import random
        
        # Initialize drops
        if not hasattr(self, '_matrix_drops'):
            self._matrix_drops = [random.randint(-self.height, 0) for _ in range(self.width // 2)]
            self._matrix_speeds = [random.randint(1, 3) for _ in range(self.width // 2)]
        
        image = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        for i, (drop_y, speed) in enumerate(zip(self._matrix_drops, self._matrix_speeds)):
            x = i * 2
            
            # Draw trail
            for j in range(min(10, self.height)):
                y = int(drop_y) - j
                if 0 <= y < self.height:
                    brightness = max(0, 255 - j * 25)
                    image[y, x] = [0, brightness, 0]
            
            # Update position
            self._matrix_drops[i] = drop_y + speed * 0.5
            if self._matrix_drops[i] > self.height + 10:
                self._matrix_drops[i] = random.randint(-10, 0)
                self._matrix_speeds[i] = random.randint(1, 3)
        
        return image
    
    def _demo_clock(self, t: float) -> np.ndarray:
        """Digital clock display."""
        if not HAS_CV2:
            return np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        import datetime
        
        now = datetime.datetime.now()
        time_str = now.strftime("%H:%M:%S")
        
        image = np.zeros((self.height, self.width, 3), dtype=np.uint8)
        
        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 1.0
        thickness = 2
        
        (text_w, text_h), _ = cv2.getTextSize(time_str, font, font_scale, thickness)
        x = (self.width - text_w) // 2
        y = (self.height + text_h) // 2
        
        # Pulsing color
        hue = (t * 0.1) % 1.0
        import colorsys
        r, g, b = colorsys.hsv_to_rgb(hue, 0.7, 1.0)
        color = (int(r * 255), int(g * 255), int(b * 255))
        
        cv2.putText(image, time_str, (x, y), font, font_scale, color, thickness)
        
        return image
