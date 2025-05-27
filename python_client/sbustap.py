"""
SBUS Controller Python Library

A comprehensive library for communicating with SBUS CDC devices that support
JSON-based channel control and monitoring.

Author: Generated for SBUS Control System
License: Proprietary
"""

import json
import time
import threading
import serial
import queue
from typing import Dict, List, Optional, Callable, Union, Tuple
from dataclasses import dataclass, asdict
from enum import Enum
import logging


class SBUSValueError(Exception):
    """Raised when SBUS values are out of valid range."""
    pass


class SBUSCommunicationError(Exception):
    """Raised when communication with SBUS device fails."""
    pass


class SBUSTimeoutError(Exception):
    """Raised when operations timeout."""
    pass


class MessageType(Enum):
    """Types of messages received from SBUS device."""
    READY = "ready"
    CHANNELS = "channels"
    STATUS = "status"
    CHANNEL_SET = "channel_set"
    CHANNELS_SET = "channels_set"
    CHANNEL_CLEARED = "channel_cleared"
    ALL_CLEARED = "all_cleared"
    OVERRIDE_STATUS = "override_status"
    OVERRIDE_EXPIRED = "override_expired"
    HELP = "help"
    ERROR = "error"


@dataclass
class ChannelData:
    """Represents channel data from SBUS device."""
    input_channels: List[int]
    output_channels: List[int]
    overrides: List[int]
    frame_lost: bool
    failsafe: bool
    timestamp: int


@dataclass
class DeviceStatus:
    """Represents device connection status."""
    connected: bool
    timestamp: int


@dataclass
class ChannelOverride:
    """Represents an active channel override."""
    channel: int
    value: int
    remaining_ms: int


@dataclass
class OverrideStatus:
    """Represents current override status."""
    overrides: List[ChannelOverride]
    timestamp: int


class SBUSController:
    """
    Main controller class for SBUS CDC device communication.
    
    This class provides methods to:
    - Connect to and monitor SBUS devices
    - Control individual channels or multiple channels
    - Monitor channel data and device status
    - Handle overrides and timeouts
    """
    
    # SBUS constants
    SBUS_MIN_VALUE = 0
    SBUS_MAX_VALUE = 2047
    SBUS_CENTER_VALUE = 992
    SBUS_CHANNELS = 16
    
    # Typical SBUS ranges for conversion
    SBUS_TYPICAL_MIN = 172
    SBUS_TYPICAL_MAX = 1811
    
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 1.0):
        """
        Initialize SBUS controller.
        
        Args:
            port: Serial port name (e.g., 'COM3' on Windows, '/dev/ttyACM0' on Linux)
            baudrate: Serial communication baudrate (default: 115200)
            timeout: Serial read timeout in seconds (default: 1.0)
        """
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        
        self.serial_conn: Optional[serial.Serial] = None
        self.is_connected = False
        self.is_monitoring = False
        
        # Threading
        self._monitor_thread: Optional[threading.Thread] = None
        self._stop_monitoring = threading.Event()
        self._message_queue = queue.Queue()
        
        # Callbacks
        self._channel_callback: Optional[Callable[[ChannelData], None]] = None
        self._status_callback: Optional[Callable[[DeviceStatus], None]] = None
        self._override_callback: Optional[Callable[[ChannelOverride], None]] = None
        self._error_callback: Optional[Callable[[str], None]] = None
        
        # State tracking
        self._last_channel_data: Optional[ChannelData] = None
        self._last_device_status: Optional[DeviceStatus] = None
        self._response_timeout = 5.0
        
        # Logging
        self.logger = logging.getLogger(__name__)
    
    def connect(self) -> bool:
        """
        Connect to the SBUS device.
        
        Returns:
            True if connection successful, False otherwise
            
        Raises:
            SBUSCommunicationError: If connection fails
        """
        try:
            self.serial_conn = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=self.timeout,
                write_timeout=self.timeout
            )
            
            # Wait for device ready message
            start_time = time.time()
            while time.time() - start_time < 10.0:  # 10 second timeout
                if self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode('utf-8').strip()
                    if line:
                        try:
                            msg = json.loads(line)
                            if msg.get('type') in ['ready', 'channels', 'status']:
                                self.is_connected = True
                                self.logger.info("Connected to SBUS device")
                                return True
                        except json.JSONDecodeError:
                            continue
                time.sleep(0.1)
            
            raise SBUSCommunicationError("Device ready message not received")
            
        except serial.SerialException as e:
            raise SBUSCommunicationError(f"Failed to connect to {self.port}: {e}")
    
    def disconnect(self):
        """Disconnect from the SBUS device."""
        self.stop_monitoring()
        
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
            self.is_connected = False
            self.logger.info("Disconnected from SBUS device")
    
    def start_monitoring(self) -> bool:
        """
        Start monitoring SBUS data in a separate thread.
        
        Returns:
            True if monitoring started successfully
            
        Raises:
            SBUSCommunicationError: If not connected or monitoring already active
        """
        if not self.is_connected:
            raise SBUSCommunicationError("Not connected to device")
        
        if self.is_monitoring:
            raise SBUSCommunicationError("Monitoring already active")
        
        self._stop_monitoring.clear()
        self._monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self._monitor_thread.start()
        self.is_monitoring = True
        self.logger.info("Started monitoring SBUS data")
        return True
    
    def stop_monitoring(self):
        """Stop monitoring SBUS data."""
        if self.is_monitoring:
            self._stop_monitoring.set()
            if self._monitor_thread:
                self._monitor_thread.join(timeout=2.0)
            self.is_monitoring = False
            self.logger.info("Stopped monitoring SBUS data")
    
    def _monitor_loop(self):
        """Main monitoring loop running in separate thread."""
        buffer = ""
        
        while not self._stop_monitoring.is_set():
            try:
                if self.serial_conn and self.serial_conn.in_waiting > 0:
                    data = self.serial_conn.read(self.serial_conn.in_waiting).decode('utf-8')
                    buffer += data
                    
                    # Process complete lines
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        line = line.strip()
                        if line:
                            self._process_message(line)
                
                time.sleep(0.01)  # Small delay to prevent busy waiting
                
            except Exception as e:
                self.logger.error(f"Error in monitoring loop: {e}")
                if self._error_callback:
                    self._error_callback(str(e))
                time.sleep(0.1)
    
    def _process_message(self, message: str):
        """Process incoming JSON message from device."""
        try:
            msg = json.loads(message)
            msg_type = msg.get('type')
            
            if msg_type == MessageType.CHANNELS.value:
                channel_data = ChannelData(
                    input_channels=msg['input_channels'],
                    output_channels=msg['output_channels'],
                    overrides=msg['overrides'],
                    frame_lost=msg['frameLost'],
                    failsafe=msg['failsafe'],
                    timestamp=msg['timestamp']
                )
                self._last_channel_data = channel_data
                
                if self._channel_callback:
                    self._channel_callback(channel_data)
            
            elif msg_type == MessageType.STATUS.value:
                status = DeviceStatus(
                    connected=msg['connected'],
                    timestamp=msg['timestamp']
                )
                self._last_device_status = status
                
                if self._status_callback:
                    self._status_callback(status)
            
            elif msg_type == MessageType.OVERRIDE_EXPIRED.value:
                override = ChannelOverride(
                    channel=msg['channel'],
                    value=0,  # Not provided in expired message
                    remaining_ms=0
                )
                
                if self._override_callback:
                    self._override_callback(override)
            
            elif msg_type == MessageType.ERROR.value:
                error_msg = msg['message']
                self.logger.error(f"Device error: {error_msg}")
                
                if self._error_callback:
                    self._error_callback(error_msg)
            
            # Queue message for synchronous operations
            self._message_queue.put(msg)
            
        except json.JSONDecodeError as e:
            self.logger.warning(f"Failed to parse JSON message: {message}")
        except Exception as e:
            self.logger.error(f"Error processing message: {e}")
    
    def _send_command(self, command: Dict) -> Dict:
        """
        Send command and wait for response.
        
        Args:
            command: Command dictionary to send
            
        Returns:
            Response dictionary
            
        Raises:
            SBUSCommunicationError: If not connected or send fails
            SBUSTimeoutError: If no response received within timeout
        """
        if not self.is_connected or not self.serial_conn:
            raise SBUSCommunicationError("Not connected to device")
        
        # Clear message queue
        while not self._message_queue.empty():
            try:
                self._message_queue.get_nowait()
            except queue.Empty:
                break
        
        # Send command
        cmd_str = json.dumps(command) + '\n'
        self.serial_conn.write(cmd_str.encode('utf-8'))
        
        # Wait for response
        start_time = time.time()
        while time.time() - start_time < self._response_timeout:
            try:
                msg = self._message_queue.get(timeout=0.1)
                return msg
            except queue.Empty:
                continue
        
        raise SBUSTimeoutError(f"No response received within {self._response_timeout} seconds")
    
    @staticmethod
    def validate_channel(channel: int):
        """Validate channel number."""
        if not 1 <= channel <= SBUSController.SBUS_CHANNELS:
            raise SBUSValueError(f"Channel must be between 1 and {SBUSController.SBUS_CHANNELS}")
    
    @staticmethod
    def validate_value(value: int):
        """Validate SBUS value."""
        if not SBUSController.SBUS_MIN_VALUE <= value <= SBUSController.SBUS_MAX_VALUE:
            raise SBUSValueError(f"Value must be between {SBUSController.SBUS_MIN_VALUE} and {SBUSController.SBUS_MAX_VALUE}")
    
    def set_channel(self, channel: int, value: int) -> bool:
        """
        Set a single channel override.
        
        Args:
            channel: Channel number (1-16)
            value: SBUS value (0-2047)
            
        Returns:
            True if successful
            
        Raises:
            SBUSValueError: If channel or value is invalid
            SBUSCommunicationError: If communication fails
        """
        self.validate_channel(channel)
        self.validate_value(value)
        
        command = {
            "command": "set_channel",
            "channel": channel,
            "value": value
        }
        
        response = self._send_command(command)
        
        if response.get('type') == 'channel_set':
            return True
        elif response.get('type') == 'error':
            raise SBUSCommunicationError(f"Device error: {response.get('message')}")
        else:
            return False
    
    def set_channels(self, channels: List[Tuple[int, int]]) -> int:
        """
        Set multiple channel overrides.
        
        Args:
            channels: List of (channel, value) tuples
            
        Returns:
            Number of channels successfully set
            
        Raises:
            SBUSValueError: If any channel or value is invalid
            SBUSCommunicationError: If communication fails
        """
        # Validate all channels and values first
        for channel, value in channels:
            self.validate_channel(channel)
            self.validate_value(value)
        
        channel_objects = [
            {"channel": channel, "value": value}
            for channel, value in channels
        ]
        
        command = {
            "command": "set_channels",
            "channels": channel_objects
        }
        
        response = self._send_command(command)
        
        if response.get('type') == 'channels_set':
            return response.get('count', 0)
        elif response.get('type') == 'error':
            raise SBUSCommunicationError(f"Device error: {response.get('message')}")
        else:
            return 0
    
    def clear_channel(self, channel: int) -> bool:
        """
        Clear a single channel override.
        
        Args:
            channel: Channel number (1-16)
            
        Returns:
            True if successful
        """
        self.validate_channel(channel)
        
        command = {
            "command": "clear_channel",
            "channel": channel
        }
        
        response = self._send_command(command)
        return response.get('type') == 'channel_cleared'
    
    def clear_all_channels(self) -> bool:
        """
        Clear all channel overrides.
        
        Returns:
            True if successful
        """
        command = {"command": "clear_all"}
        response = self._send_command(command)
        return response.get('type') == 'all_cleared'
    
    def get_override_status(self) -> OverrideStatus:
        """
        Get current override status.
        
        Returns:
            OverrideStatus object with current overrides
        """
        command = {"command": "status"}
        response = self._send_command(command)
        
        if response.get('type') == 'override_status':
            overrides = [
                ChannelOverride(
                    channel=override['channel'],
                    value=override['value'],
                    remaining_ms=override['remaining_ms']
                )
                for override in response.get('overrides', [])
            ]
            
            return OverrideStatus(
                overrides=overrides,
                timestamp=response.get('timestamp', 0)
            )
        else:
            return OverrideStatus(overrides=[], timestamp=int(time.time() * 1000))
    
    def get_help(self) -> Dict:
        """
        Get device help information.
        
        Returns:
            Help information dictionary
        """
        command = {"command": "help"}
        response = self._send_command(command)
        return response
    
    # Callback setters
    def set_channel_callback(self, callback: Callable[[ChannelData], None]):
        """Set callback for channel data updates."""
        self._channel_callback = callback
    
    def set_status_callback(self, callback: Callable[[DeviceStatus], None]):
        """Set callback for device status updates."""
        self._status_callback = callback
    
    def set_override_callback(self, callback: Callable[[ChannelOverride], None]):
        """Set callback for override expiration events."""
        self._override_callback = callback
    
    def set_error_callback(self, callback: Callable[[str], None]):
        """Set callback for error messages."""
        self._error_callback = callback
    
    # Property getters
    @property
    def last_channel_data(self) -> Optional[ChannelData]:
        """Get the last received channel data."""
        return self._last_channel_data
    
    @property
    def last_device_status(self) -> Optional[DeviceStatus]:
        """Get the last received device status."""
        return self._last_device_status
    
    # Utility methods
    @staticmethod
    def sbus_to_microseconds(sbus_value: int) -> int:
        """
        Convert SBUS value to microseconds (typical servo range).
        
        Args:
            sbus_value: SBUS value (0-2047)
            
        Returns:
            Microseconds (1000-2000)
        """
        # Map SBUS range to typical servo microsecond range
        return int((sbus_value - SBUSController.SBUS_TYPICAL_MIN) * 
                  (2000 - 1000) / (SBUSController.SBUS_TYPICAL_MAX - SBUSController.SBUS_TYPICAL_MIN) + 1000)
    
    @staticmethod
    def microseconds_to_sbus(microseconds: int) -> int:
        """
        Convert microseconds to SBUS value.
        
        Args:
            microseconds: Microseconds (1000-2000)
            
        Returns:
            SBUS value (0-2047)
        """
        return int((microseconds - 1000) * 
                  (SBUSController.SBUS_TYPICAL_MAX - SBUSController.SBUS_TYPICAL_MIN) / (2000 - 1000) + 
                  SBUSController.SBUS_TYPICAL_MIN)
    
    @staticmethod
    def sbus_to_percent(sbus_value: int) -> float:
        """
        Convert SBUS value to percentage (-100 to +100).
        
        Args:
            sbus_value: SBUS value (0-2047)
            
        Returns:
            Percentage (-100.0 to +100.0)
        """
        return (sbus_value - SBUSController.SBUS_CENTER_VALUE) / 819.0 * 100.0
    
    @staticmethod
    def percent_to_sbus(percent: float) -> int:
        """
        Convert percentage to SBUS value.
        
        Args:
            percent: Percentage (-100.0 to +100.0)
            
        Returns:
            SBUS value (0-2047)
        """
        return int(percent / 100.0 * 819.0 + SBUSController.SBUS_CENTER_VALUE)
    
    def __enter__(self):
        """Context manager entry."""
        self.connect()
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit."""
        self.disconnect()


# Example usage and testing functions
def example_basic_usage():
    """Example of basic library usage."""
    
    # Configure logging
    logging.basicConfig(level=logging.INFO)
    
    try:
        # Connect to device (adjust port as needed)
        controller = SBUSController('/dev/ttyACM0')  # or 'COM3' on Windows
        controller.connect()
        
        # Set up callbacks
        def on_channel_data(data: ChannelData):
            print(f"Channels: {data.input_channels[:4]}...")  # Print first 4 channels
            if data.overrides:
                print(f"Active overrides: {data.overrides}")
        
        def on_status_change(status: DeviceStatus):
            print(f"SBUS Status: {'Connected' if status.connected else 'Disconnected'}")
        
        def on_error(error: str):
            print(f"Error: {error}")
        
        controller.set_channel_callback(on_channel_data)
        controller.set_status_callback(on_status_change)
        controller.set_error_callback(on_error)
        
        # Start monitoring
        controller.start_monitoring()
        
        # Control examples
        print("Setting channel 1 to center position...")
        controller.set_channel(1, SBUSController.SBUS_CENTER_VALUE)
        
        time.sleep(1)
        
        print("Setting multiple channels...")
        controller.set_channels([
            (1, 1000),  # Channel 1 to 1000
            (2, 1500),  # Channel 2 to 1500
            (3, 2000),  # Channel 3 to 2000
        ])
        
        time.sleep(3)
        
        print("Getting override status...")
        status = controller.get_override_status()
        for override in status.overrides:
            print(f"Channel {override.channel}: {override.value} ({override.remaining_ms}ms remaining)")
        
        time.sleep(2)
        
        print("Clearing all overrides...")
        controller.clear_all_channels()
        
        # Monitor for a few more seconds
        time.sleep(5)
        
    except Exception as e:
        print(f"Error: {e}")
    finally:
        controller.disconnect()


def example_context_manager():
    """Example using context manager."""
    
    try:
        with SBUSController('/dev/ttyACM0') as controller:
            # Set channel using percentage
            percent_50 = SBUSController.percent_to_sbus(50.0)
            controller.set_channel(1, percent_50)
            
            # Set channel using microseconds
            us_1500 = SBUSController.microseconds_to_sbus(1500)
            controller.set_channel(2, us_1500)
            
            time.sleep(3)
            
    except Exception as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    # Run basic example
    example_basic_usage()
