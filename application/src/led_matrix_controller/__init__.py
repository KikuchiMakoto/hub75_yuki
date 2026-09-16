"""
LED Matrix Controller

Control 128x32 HUB75 LED matrix panel via USB CDC.
"""

from .controller import LEDMatrixController
from .devices import SerialDevice, TerminalDevice, ImageDevice

__version__ = "1.0.0"

__all__ = [
    "LEDMatrixController",
    "SerialDevice",
    "TerminalDevice",
    "ImageDevice",
]
