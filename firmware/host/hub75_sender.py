#!/usr/bin/env python3
"""
HUB75 LED Panel Controller - Host Side
Sends COBS-encoded RGB565 frames via USB CDC

Requirements:
    pip install pyserial numpy opencv-python

Usage:
    python hub75_sender.py                    # Rainbow demo
    python hub75_sender.py image.png          # Display image
    python hub75_sender.py video.mp4          # Play video
    python hub75_sender.py --camera           # Webcam input
"""

import sys
import time
import argparse
from typing import Optional, Tuple

import numpy as np

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Please install pyserial: pip install pyserial")
    sys.exit(1)

try:
    import cv2
except ImportError:
    print("Please install opencv-python: pip install opencv-python")
    sys.exit(1)


# Display configuration
DISPLAY_WIDTH = 128
DISPLAY_HEIGHT = 32


def cobs_encode(data: bytes) -> bytes:
    """
    Encode data using COBS (Consistent Overhead Byte Stuffing).
    Returns COBS-encoded data (does not include terminating 0x00).
    """
    if not data:
        return b'\x01'

    output = bytearray()
    code_index = 0
    code = 1

    output.append(0)  # Placeholder for first code

    for byte in data:
        if byte == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1

            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1

    output[code_index] = code
    return bytes(output)


def rgb888_to_rgb565(image: np.ndarray) -> np.ndarray:
    """Convert RGB888 to RGB565"""
    r = (image[:, :, 0] >> 3).astype(np.uint16)
    g = (image[:, :, 1] >> 2).astype(np.uint16)
    b = (image[:, :, 2] >> 3).astype(np.uint16)
    return (r << 11) | (g << 5) | b


class HUB75Controller:
    """Control HUB75 LED panel via USB CDC"""
    
    def __init__(self, port: str = None):
        self.port = port
        self.serial: Optional[serial.Serial] = None
        self._frame_count = 0
        self._fps_start = time.time()
        self._fps = 0.0
    
    def connect(self) -> bool:
        if self.port is None:
            self.port = self._find_port()
        
        if self.port is None:
            raise RuntimeError("No device found")
        
        self.serial = serial.Serial(self.port, 115200, timeout=0.1)
        self.serial.reset_input_buffer()
        self.serial.reset_output_buffer()
        print(f"Connected: {self.port}")
        return True
    
    def _find_port(self) -> Optional[str]:
        for p in serial.tools.list_ports.comports():
            desc = (p.description or "").lower()
            if any(x in desc for x in ['pico', 'rp2040']):
                return p.device
        for p in serial.tools.list_ports.comports():
            if 'acm' in p.device.lower() or 'usb' in p.device.lower():
                return p.device
        return None
    
    def disconnect(self):
        if self.serial:
            self.serial.close()
            self.serial = None
    
    def send_frame(self, image: np.ndarray, wait_ack: bool = True) -> bool:
        if self.serial is None:
            raise RuntimeError("Not connected")

        # Resize
        if image.shape[:2] != (DISPLAY_HEIGHT, DISPLAY_WIDTH):
            image = cv2.resize(image, (DISPLAY_WIDTH, DISPLAY_HEIGHT))

        # Convert to RGB565
        rgb565 = rgb888_to_rgb565(image)
        raw_data = rgb565.astype('<u2').tobytes()

        # COBS encode and add 0x00 terminator
        encoded = cobs_encode(raw_data)
        self.serial.write(encoded + b'\x00')

        self._update_fps()
        return True
    
    def _update_fps(self):
        self._frame_count += 1
        elapsed = time.time() - self._fps_start
        if elapsed >= 1.0:
            self._fps = self._frame_count / elapsed
            self._frame_count = 0
            self._fps_start = time.time()
    
    @property
    def fps(self) -> float:
        return self._fps
    
    def fill(self, color: Tuple[int, int, int]):
        img = np.full((DISPLAY_HEIGHT, DISPLAY_WIDTH, 3), color, dtype=np.uint8)
        self.send_frame(img)
    
    def clear(self):
        self.fill((0, 0, 0))


def demo_rainbow(ctrl: HUB75Controller):
    import colorsys
    print("Rainbow (Ctrl+C to stop)")
    hue = 0.0
    try:
        while True:
            img = np.zeros((DISPLAY_HEIGHT, DISPLAY_WIDTH, 3), dtype=np.uint8)
            for x in range(DISPLAY_WIDTH):
                h = (hue + x / DISPLAY_WIDTH) % 1.0
                r, g, b = colorsys.hsv_to_rgb(h, 1.0, 1.0)
                img[:, x] = [int(r*255), int(g*255), int(b*255)]
            ctrl.send_frame(img)
            hue = (hue + 0.01) % 1.0
            if ctrl._frame_count == 0:
                print(f"\rFPS: {ctrl.fps:.1f}  ", end='', flush=True)
    except KeyboardInterrupt:
        print()


def demo_video(ctrl: HUB75Controller, path: str):
    print(f"Playing: {path}")
    cap = cv2.VideoCapture(path)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30
    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                continue
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            start = time.time()
            ctrl.send_frame(frame)
            elapsed = time.time() - start
            if elapsed < 1.0/fps:
                time.sleep(1.0/fps - elapsed)
            if ctrl._frame_count == 0:
                print(f"\rFPS: {ctrl.fps:.1f}  ", end='', flush=True)
    except KeyboardInterrupt:
        print()
    cap.release()


def demo_camera(ctrl: HUB75Controller, cam_id: int = 0):
    print(f"Camera {cam_id} (Ctrl+C to stop)")
    cap = cv2.VideoCapture(cam_id)
    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                continue
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            ctrl.send_frame(frame)
            if ctrl._frame_count == 0:
                print(f"\rFPS: {ctrl.fps:.1f}  ", end='', flush=True)
    except KeyboardInterrupt:
        print()
    cap.release()


def demo_image(ctrl: HUB75Controller, path: str):
    print(f"Image: {path}")
    img = cv2.imread(path)
    if img is None:
        print("Error loading image")
        return
    img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    ctrl.send_frame(img)
    input("Enter to exit...")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', nargs='?')
    parser.add_argument('--port', '-p')
    parser.add_argument('--camera', '-c', action='store_true')
    parser.add_argument('--camera-id', type=int, default=0)
    parser.add_argument('--fill', '-f')
    args = parser.parse_args()
    
    ctrl = HUB75Controller(args.port)
    try:
        ctrl.connect()
        
        if args.fill:
            r, g, b = map(int, args.fill.split(','))
            ctrl.fill((r, g, b))
        elif args.camera:
            demo_camera(ctrl, args.camera_id)
        elif args.input:
            ext = args.input.lower().split('.')[-1]
            if ext in ['mp4', 'avi', 'mov', 'mkv', 'webm']:
                demo_video(ctrl, args.input)
            else:
                demo_image(ctrl, args.input)
        else:
            demo_rainbow(ctrl)
    finally:
        ctrl.clear()
        ctrl.disconnect()


if __name__ == '__main__':
    main()
