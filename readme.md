# ESP32-C3 SBUS Capture

A high-performance SBUS protocol capture and retransmission tool for ESP32-C3 microcontrollers. This project reads SBUS data from RC receivers, outputs structured JSON data via USB serial, and retransmits the SBUS signal for passthrough applications.

## Features

- **Real-time SBUS capture** - Reads 16-channel SBUS data at 100kHz
- **JSON output format** - Machine-readable data for computer integration
- **SBUS retransmission** - Acts as SBUS repeater/splitter
- **Connection monitoring** - Automatic detection of SBUS signal loss
- **Error detection** - Reports frame lost and failsafe conditions
- **Low latency** - Minimal delay between input and output

## Hardware Requirements

### Components
- ESP32-C3 development board (any variant)
- SBUS-compatible RC receiver
- USB cable for programming and data output
- Jumper wires for connections

### Wiring Diagram

```
SBUS Receiver    ESP32-C3
┌─────────────┐  ┌─────────────┐
│     GND     │──│     GND     │
│     5V      │──│     5V      │
│    SBUS     │──│  GPIO 20    │ (RX)
└─────────────┘  │  GPIO 21    │ (TX) ── To SBUS device
                 └─────────────┘
```

### Pin Configuration
- **GPIO 20** (RX) - SBUS input from receiver
- **GPIO 21** (TX) - SBUS output for retransmission
- **USB** - Serial data output to computer

## Software Setup

### Prerequisites
- [Arduino CLI](https://arduino.github.io/arduino-cli/) or Arduino IDE
- ESP32 Arduino Core installed

### Installation

1. **Clone or download** this project
2. **Install ESP32 core** (if not already installed):
   ```bash
   arduino-cli core install esp32:esp32
   ```

3. **Compile the firmware**:
   ```bash
   arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc .
   ```

4. **Upload to ESP32-C3**:
   ```bash
   arduino-cli upload -b esp32:esp32:esp32c3:CDCOnBoot=cdc -p /dev/ttyUSB0 .
   ```
   *(Replace `/dev/ttyUSB0` with your actual port)*

## Usage

### Basic Operation

1. **Connect hardware** according to wiring diagram
2. **Power on** the ESP32-C3 and RC receiver
3. **Connect USB** to computer
4. **Open serial monitor** at 115200 baud
5. **Move RC transmitter** controls to see data

### Serial Output Format

The device outputs JSON messages via USB serial at 115200 baud:

#### Channel Data Messages
```json
{
  "type": "channels",
  "timestamp": 12345,
  "channels": [992, 1024, 856, 1200, 992, 992, 992, 992, 992, 992, 992, 992, 992, 992, 992, 992],
  "frameLost": false,
  "failsafe": false
}
```

**Fields:**
- `type` - Always "channels" for channel data
- `timestamp` - Milliseconds since ESP32 startup
- `channels` - Array of 16 channel values (0-2047, typical range 172-1811)
- `frameLost` - Boolean indicating receiver detected frame loss
- `failsafe` - Boolean indicating receiver is in failsafe mode

#### Status Messages
```json
{
  "type": "status", 
  "timestamp": 12345,
  "connected": false
}
```

**Fields:**
- `type` - Always "status" for connection status
- `timestamp` - Milliseconds since ESP32 startup  
- `connected` - Boolean indicating SBUS signal presence

**Status Behavior:**
- `"connected": false` sent immediately when signal lost
- `"connected": false` repeated every 5 seconds while disconnected
- `"connected": true` sent when signal restored

### SBUS Retransmission

The device automatically retransmits received SBUS frames on GPIO 21 (TX pin). This enables:

- **Signal splitting** - Send same SBUS to multiple devices
- **Signal conditioning** - Clean up noisy SBUS signals  
- **Monitoring passthrough** - Monitor data without interrupting signal chain
- **Protocol bridging** - Convert between SBUS and other formats

## Integration Examples

### Python Data Capture
```python
import serial
import json

# Open serial connection
ser = serial.Serial('/dev/ttyUSB0', 115200)

while True:
    line = ser.readline().decode('utf-8').strip()
    try:
        data = json.loads(line)
        if data['type'] == 'channels':
            print(f"Channel 1: {data['channels'][0]}")
        elif data['type'] == 'status':
            print(f"SBUS Connected: {data['connected']}")
    except json.JSONDecodeError:
        pass
```

### Node.js Processing
```javascript
const SerialPort = require('serialport');
const Readline = require('@serialport/parser-readline');

const port = new SerialPort('/dev/ttyUSB0', { baudRate: 115200 });
const parser = port.pipe(new Readline({ delimiter: '\n' }));

parser.on('data', (line) => {
  try {
    const data = JSON.parse(line);
    if (data.type === 'channels') {
      console.log('Throttle:', data.channels[2]);
      console.log('Steering:', data.channels[0]);
    }
  } catch (e) {
    // Ignore invalid JSON
  }
});
```

## Technical Specifications

### SBUS Protocol Details
- **Baud rate:** 100,000 bps
- **Format:** 8E2 (8 data bits, even parity, 2 stop bits)
- **Logic:** Inverted (0V = logical 1, 3.3V = logical 0)
- **Frame rate:** ~70Hz (every 14ms)
- **Frame size:** 25 bytes
- **Channels:** 16 channels, 11-bit resolution each
- **Range:** 0-2047 (typical usable range 172-1811)

### Performance Characteristics
- **Latency:** <1ms from input to retransmission
- **CPU usage:** Minimal, interrupt-driven
- **Memory usage:** <2KB RAM
- **Update rate:** Up to 70Hz matching SBUS frame rate

## Troubleshooting

### No SBUS Data Received
- Check wiring connections
- Verify receiver is bound to transmitter
- Ensure receiver is powered and operational
- Check for correct GPIO pin assignment

### Compilation Errors
- Verify ESP32 Arduino core is installed
- Check board selection: `esp32:esp32:esp32c3`
- Ensure all required libraries are available

### Serial Output Issues  
- Confirm baud rate is set to 115200
- Check USB cable and driver installation
- Verify CDC on Boot is enabled in board options

### Signal Quality Issues
- Use twisted pair or shielded cable for SBUS connections
- Keep SBUS wires away from power lines
- Add 100Ω pull-up resistor if signal is weak

## API Reference

### Helper Functions

#### `sbusToMicros(uint16_t sbusValue)`
Converts SBUS channel value to standard servo microseconds (1000-2000μs).

#### `sbusToPercent(uint16_t sbusValue)`  
Converts SBUS channel value to percentage (-100% to +100%).

#### `createSBUSFrame(uint16_t channelValues[16])`
Creates custom SBUS frame from channel array for transmission.

### Configuration Constants
```cpp
#define SBUS_BAUD 100000              // SBUS baud rate
#define SBUS_TIMEOUT 100              // Connection timeout (ms)
#define LOST_MESSAGE_INTERVAL 5000    // Lost message repeat interval (ms)
```