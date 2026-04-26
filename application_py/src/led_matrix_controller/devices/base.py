"""
Base Device Interface

Abstract base class for output devices.
"""

from abc import ABC, abstractmethod


class BaseDevice(ABC):
    """Abstract base class for LED matrix output devices."""
    
    @abstractmethod
    def connect(self) -> bool:
        """
        Connect to the device.
        
        Returns:
            True if connection successful
        """
        pass
    
    @abstractmethod
    def disconnect(self):
        """Disconnect from the device."""
        pass
    
    @abstractmethod
    def send(self, data: bytes, wait_ack: bool = True) -> bool:
        """
        Send data to the device.
        
        Args:
            data: Encoded frame data
            wait_ack: Wait for acknowledgment
            
        Returns:
            True if successful
        """
        pass
    
    @property
    @abstractmethod
    def is_connected(self) -> bool:
        """Check if device is connected."""
        pass
