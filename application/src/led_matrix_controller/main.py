#!/usr/bin/env python3
"""
LED Matrix Controller - Main Entry Point

Control 128x32 HUB75 LED matrix panel via USB CDC.
Supports image, video, and various demo modes.

Usage:
    python -m led_matrix_controller.main --help
    python -m led_matrix_controller.main --image photo.jpg
    python -m led_matrix_controller.main --video movie.mp4
    python -m led_matrix_controller.main --demo rainbow
"""

import argparse
import sys
from pathlib import Path

from .controller import LEDMatrixController
from .devices import SerialDevice, TerminalDevice, ImageDevice


def create_device(args):
    """Create appropriate device based on arguments."""
    if args.device == "serial":
        return SerialDevice(port=args.port, baudrate=args.baudrate)
    elif args.device == "terminal":
        return TerminalDevice()
    elif args.device == "image":
        return ImageDevice(output_dir=args.output_dir)
    else:
        raise ValueError(f"Unknown device type: {args.device}")


def main():
    parser = argparse.ArgumentParser(
        description="LED Matrix Controller for 128x32 HUB75 panel",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Display image
  python -m led_matrix_controller.main --image photo.jpg

  # Play video
  python -m led_matrix_controller.main --video movie.mp4

  # Rainbow demo
  python -m led_matrix_controller.main --demo rainbow

  # Terminal preview (no hardware)
  python -m led_matrix_controller.main --device terminal --demo rainbow
"""
    )
    
    # Device options
    device_group = parser.add_argument_group("Device options")
    device_group.add_argument(
        "--device", "-d",
        choices=["serial", "terminal", "image"],
        default="serial",
        help="Output device type (default: serial)"
    )
    device_group.add_argument(
        "--port", "-p",
        default=None,
        help="Serial port (auto-detect if not specified)"
    )
    device_group.add_argument(
        "--baudrate", "-b",
        type=int,
        default=115200,
        help="Baud rate (default: 115200)"
    )
    device_group.add_argument(
        "--output-dir",
        default="output",
        help="Output directory for image device (default: output)"
    )
    
    # Input options (mutually exclusive)
    input_group = parser.add_argument_group("Input options")
    input_mutex = input_group.add_mutually_exclusive_group()
    input_mutex.add_argument(
        "--image", "-i",
        metavar="FILE",
        help="Display image file"
    )
    input_mutex.add_argument(
        "--video", "-v",
        metavar="FILE",
        help="Play video file"
    )
    input_mutex.add_argument(
        "--demo",
        choices=["rainbow", "gradient", "plasma", "fire", "matrix", "clock"],
        help="Run demo animation"
    )
    input_mutex.add_argument(
        "--fill", "-f",
        metavar="R,G,B",
        help="Fill with solid color (e.g., 255,0,0 for red)"
    )
    
    # Display options
    display_group = parser.add_argument_group("Display options")
    display_group.add_argument(
        "--loop",
        action="store_true",
        help="Loop video playback"
    )
    display_group.add_argument(
        "--fps",
        type=float,
        default=30.0,
        help="Target FPS for demos (default: 30)"
    )
    display_group.add_argument(
        "--brightness",
        type=float,
        default=1.0,
        help="Brightness multiplier 0.0-1.0 (default: 1.0)"
    )

    # Performance options
    perf_group = parser.add_argument_group("Performance options")
    perf_group.add_argument(
        "--max-fps",
        action="store_true",
        help="Ignore video FPS limit, send frames as fast as possible"
    )
    perf_group.add_argument(
        "--no-ack",
        action="store_true",
        help="Don't wait for ACK (faster but may drop frames)"
    )
    perf_group.add_argument(
        "--base64",
        action="store_true",
        help="Use Base64 encoding instead of binary (slower, for compatibility)"
    )
    
    args = parser.parse_args()
    
    # Validate arguments
    if not any([args.image, args.video, args.demo, args.fill]):
        parser.print_help()
        print("\nError: Please specify an input source (--image, --video, --demo, or --fill)")
        sys.exit(1)
    
    try:
        # Create device
        device = create_device(args)
        
        # Create controller
        controller = LEDMatrixController(
            device=device,
            brightness=args.brightness,
            binary_mode=not args.base64
        )
        
        # Connect
        controller.connect()
        
        try:
            if args.image:
                controller.display_image(args.image)
                input("Press Enter to exit...")

            elif args.video:
                controller.play_video(
                    args.video,
                    loop=args.loop,
                    max_fps=args.max_fps,
                    wait_ack=not args.no_ack
                )

            elif args.demo:
                controller.run_demo(args.demo, fps=args.fps)

            elif args.fill:
                r, g, b = map(int, args.fill.split(","))
                controller.fill((r, g, b))
                input("Press Enter to exit...")
                
        except KeyboardInterrupt:
            print("\nStopped")
        finally:
            controller.clear()
            controller.disconnect()
            
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
