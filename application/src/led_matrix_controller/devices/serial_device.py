"""
Serial Device

Communicates with RP2040 via USB CDC serial.
"""

from typing import Optional

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

from .base import BaseDevice


class SerialDevice(BaseDevice):
    """Serial device for USB CDC communication with RP2040."""
    
    def __init__(
        self,
        port: Optional[str] = None,
        baudrate: int = 115200,
        timeout: float = 0.1
    ):
        """
        Initialize serial device.
        
        Args:
            port: Serial port (e.g., 'COM3', '/dev/ttyACM0')
                  Auto-detect if None
            baudrate: Baud rate (ignored for USB CDC but kept for compatibility)
            timeout: Read timeout in seconds
        """
        if not HAS_SERIAL:
            raise ImportError("pyserial required: pip install pyserial")
        
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self._serial: Optional[serial.Serial] = None
    
    def _auto_detect_port(self) -> Optional[str]:
        """Auto-detect RP2040 serial port."""
        ports = serial.tools.list_ports.comports()
        
        # Look for Pico/RP2040 identifiers
        for p in ports:
            desc = (p.description or "").lower()
            mfr = (p.manufacturer or "").lower()
            
            if any(x in desc for x in ['pico', 'rp2040', 'raspberry']):
                return p.device
            if 'raspberry' in mfr:
                return p.device
        
        # Fallback: any USB serial
        for p in ports:
            device = p.device.lower()
            if 'acm' in device or 'usb' in device:
                return p.device
        
        return None
    
    def connect(self) -> bool:
        """Connect to the serial device."""
        if self.port is None:
            self.port = self._auto_detect_port()
        
        if self.port is None:
            raise RuntimeError(
                "No serial device found. "
                "Check USB connection or specify --port"
            )
        
        try:
            self._serial = serial.Serial(
                self.port,
                self.baudrate,
                timeout=self.timeout,
                write_timeout=1.0
            )
            
            # Clear buffers
            self._serial.reset_input_buffer()
            self._serial.reset_output_buffer()
            
            print(f"Connected to {self.port}")
            return True
            
        except serial.SerialException as e:
            raise RuntimeError(f"Failed to connect to {self.port}: {e}")
    
    def disconnect(self):
        """Disconnect from the serial device."""
        if self._serial:
            self._serial.close()
            self._serial = None
    
    def send(self, data: bytes, wait_ack: bool = True) -> bool:
        """Send data to the device."""
        if not self._serial:
            raise RuntimeError("Not connected")
        
        try:
            self._serial.write(data)
            
            if wait_ack:
                ack = self._serial.read(1)
                if ack == b'K':
                    return True
                elif ack == b'E':
                    print("Device error")
                    return False
                else:
                    # Timeout or unexpected response
                    return False
            
            return True
            
        except serial.SerialException as e:
            print(f"Serial error: {e}")
            return False
    
    @property
    def is_connected(self) -> bool:
        """Check if connected."""
        return self._serial is not None and self._serial.is_open
