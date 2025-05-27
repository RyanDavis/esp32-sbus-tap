#include <HardwareSerial.h>

// SBUS protocol constants
#define SBUS_FRAME_SIZE 25
#define SBUS_HEADER 0x0F
#define SBUS_FOOTER 0x00
#define SBUS_BAUD 100000
#define SBUS_CHANNELS 16

// SBUS frame structure
struct SBUSFrame {
  uint8_t header;
  uint8_t data[22];  // Channel data
  uint8_t flags;     // Digital channels and frame lost/failsafe flags
  uint8_t footer;
};

// Channel values (11-bit resolution, 0-2047)
uint16_t channels[SBUS_CHANNELS];
uint16_t outputChannels[SBUS_CHANNELS];  // Final output channel values
bool frameReceived = false;
bool frameLost = false;
bool failsafe = false;

// PC Override system
struct ChannelOverride {
  bool active;
  uint16_t value;
  unsigned long timestamp;
};

ChannelOverride channelOverrides[SBUS_CHANNELS];
const unsigned long OVERRIDE_TIMEOUT = 2000; // 2 seconds in milliseconds

// Buffer for incoming SBUS data
uint8_t sbusBuffer[SBUS_FRAME_SIZE];
uint8_t bufferIndex = 0;

// Use Serial1 for SBUS with custom pins
HardwareSerial sbusSerial(1);

// SBUS status tracking
bool sbusConnected = true;
unsigned long lastSbusMessage = 0;
unsigned long lastLostMessage = 0;
const unsigned long SBUS_TIMEOUT = 100; // 100ms timeout for SBUS LOST detection
const unsigned long LOST_MESSAGE_INTERVAL = 5000; // Send SBUS LOST message every 5 seconds

// JSON command processing
String jsonBuffer = "";

// Function prototypes
void readSBUS();
void parseSBUSFrame();
void printChannelData();
void printSbusStatus(bool connected);
void writeSBUS();
void createSBUSFrame(uint16_t channelValues[SBUS_CHANNELS]);
int sbusToMicros(uint16_t sbusValue);
float sbusToPercent(uint16_t sbusValue);
void processSerialCommands();
void handleJsonCommand();
void updateOutputChannels();
void initializeOverrides();
String extractJsonValue(const String& json, const String& key);
String extractJsonArray(const String& json, const String& key);
int countArrayElements(const String& arrayStr);
String getArrayElement(const String& arrayStr, int index);

void setup() {
  // Initialize main serial for debugging and commands
  Serial.begin(115200);
  while (!Serial) delay(10);
  
  // Initialize SBUS serial port
  // ESP32-C3 custom pins: RX=20, TX=21
  // SBUS uses inverted logic, so we need to configure accordingly
  sbusSerial.begin(SBUS_BAUD, SERIAL_8E2, 20, 21, true); // RX=20, TX=21, inverted=true
  
  // Initialize channel overrides
  initializeOverrides();
  
  Serial.println("{\"type\":\"ready\",\"message\":\"SBUS Monitor with PC Control Ready\"}");
}

void loop() {
  readSBUS();
  processSerialCommands();
  updateOutputChannels();
  
  if (frameReceived) {
    frameReceived = false;
    lastSbusMessage = millis();
    
    // Check if we were previously disconnected
    if (!sbusConnected) {
      sbusConnected = true;
      printSbusStatus(true);
    }
    
    printChannelData();
    
    // Retransmit the SBUS frame with potentially modified channels
    createSBUSFrame(outputChannels);
    writeSBUS();
  }
  
  // Check for SBUS timeout
  if (millis() - lastSbusMessage > SBUS_TIMEOUT) {
    if (sbusConnected) {
      sbusConnected = false;
      lastLostMessage = millis();
      printSbusStatus(false);
    } else {
      // Send periodic LOST messages every 5 seconds
      if (millis() - lastLostMessage > LOST_MESSAGE_INTERVAL) {
        lastLostMessage = millis();
        printSbusStatus(false);
      }
    }
    // Reset buffer if no data received for a while
    bufferIndex = 0;
  }
}

void initializeOverrides() {
  for (int i = 0; i < SBUS_CHANNELS; i++) {
    channelOverrides[i].active = false;
    channelOverrides[i].value = 992; // Center value
    channelOverrides[i].timestamp = 0;
    outputChannels[i] = 992;
  }
}

void updateOutputChannels() {
  unsigned long currentTime = millis();
  
  for (int i = 0; i < SBUS_CHANNELS; i++) {
    // Check if override has expired
    if (channelOverrides[i].active && 
        (currentTime - channelOverrides[i].timestamp > OVERRIDE_TIMEOUT)) {
      channelOverrides[i].active = false;
      
      // Send notification that override expired
      Serial.print("{\"type\":\"override_expired\",\"channel\":");
      Serial.print(i + 1);
      Serial.print(",\"timestamp\":");
      Serial.print(currentTime);
      Serial.println("}");
    }
    
    // Set output channel value based on override status
    if (channelOverrides[i].active) {
      outputChannels[i] = channelOverrides[i].value;
    } else {
      outputChannels[i] = channels[i]; // Pass through from input
    }
  }
}

void processSerialCommands() {
  while (Serial.available()) {
    char incomingChar = Serial.read();
    
    if (incomingChar == '\n' || incomingChar == '\r') {
      if (jsonBuffer.length() > 0) {
        handleJsonCommand();
        jsonBuffer = "";
      }
    } else {
      jsonBuffer += incomingChar;
      
      // Prevent buffer overflow
      if (jsonBuffer.length() > 500) {
        jsonBuffer = "";
        Serial.println("{\"type\":\"error\",\"message\":\"Command too long\"}");
      }
    }
  }
}

void handleJsonCommand() {
  // Simple JSON parsing without library
  String command = extractJsonValue(jsonBuffer, "command");
  
  if (command.length() == 0) {
    Serial.println("{\"type\":\"error\",\"message\":\"Missing command field\"}");
    return;
  }
  
  if (command == "set_channel") {
    String channelStr = extractJsonValue(jsonBuffer, "channel");
    String valueStr = extractJsonValue(jsonBuffer, "value");
    
    if (channelStr.length() == 0 || valueStr.length() == 0) {
      Serial.println("{\"type\":\"error\",\"message\":\"Missing channel or value field\"}");
      return;
    }
    
    int channel = channelStr.toInt();
    int value = valueStr.toInt();
    
    // Validate channel number (1-16)
    if (channel < 1 || channel > SBUS_CHANNELS) {
      Serial.println("{\"type\":\"error\",\"message\":\"Invalid channel number (1-16)\"}");
      return;
    }
    
    // Validate value range (0-2047 for 11-bit SBUS)
    if (value < 0 || value > 2047) {
      Serial.println("{\"type\":\"error\",\"message\":\"Invalid value range (0-2047)\"}");
      return;
    }
    
    // Set the override
    int channelIndex = channel - 1;
    channelOverrides[channelIndex].active = true;
    channelOverrides[channelIndex].value = value;
    channelOverrides[channelIndex].timestamp = millis();
    
    // Send confirmation
    Serial.print("{\"type\":\"channel_set\",\"channel\":");
    Serial.print(channel);
    Serial.print(",\"value\":");
    Serial.print(value);
    Serial.print(",\"timestamp\":");
    Serial.print(channelOverrides[channelIndex].timestamp);
    Serial.println("}");
  }
  
  else if (command == "set_channels") {
    String channelsArray = extractJsonArray(jsonBuffer, "channels");
    
    if (channelsArray.length() == 0) {
      Serial.println("{\"type\":\"error\",\"message\":\"Missing channels array\"}");
      return;
    }
    
    unsigned long currentTime = millis();
    int channelsSet = 0;
    int arrayCount = countArrayElements(channelsArray);
    
    // Process each channel in the array
    for (int i = 0; i < arrayCount; i++) {
      String channelObj = getArrayElement(channelsArray, i);
      
      String channelStr = extractJsonValue(channelObj, "channel");
      String valueStr = extractJsonValue(channelObj, "value");
      
      if (channelStr.length() == 0 || valueStr.length() == 0) {
        Serial.print("{\"type\":\"error\",\"message\":\"Missing channel or value in element ");
        Serial.print(i);
        Serial.println("\"}");
        continue;
      }
      
      int channel = channelStr.toInt();
      int value = valueStr.toInt();
      
      // Validate channel number (1-16)
      if (channel < 1 || channel > SBUS_CHANNELS) {
        Serial.print("{\"type\":\"error\",\"message\":\"Invalid channel number ");
        Serial.print(channel);
        Serial.println(" (1-16)\"}");
        continue;
      }
      
      // Validate value range (0-2047 for 11-bit SBUS)
      if (value < 0 || value > 2047) {
        Serial.print("{\"type\":\"error\",\"message\":\"Invalid value ");
        Serial.print(value);
        Serial.print(" for channel ");
        Serial.print(channel);
        Serial.println(" (0-2047)\"}");
        continue;
      }
      
      // Set the override
      int channelIndex = channel - 1;
      channelOverrides[channelIndex].active = true;
      channelOverrides[channelIndex].value = value;
      channelOverrides[channelIndex].timestamp = currentTime;
      channelsSet++;
    }
    
    // Send confirmation
    Serial.print("{\"type\":\"channels_set\",\"count\":");
    Serial.print(channelsSet);
    Serial.print(",\"timestamp\":");
    Serial.print(currentTime);
    Serial.println("}");
  }
  
  else if (command == "clear_channel") {
    String channelStr = extractJsonValue(jsonBuffer, "channel");
    
    if (channelStr.length() == 0) {
      Serial.println("{\"type\":\"error\",\"message\":\"Missing channel field\"}");
      return;
    }
    
    int channel = channelStr.toInt();
    
    // Validate channel number (1-16)
    if (channel < 1 || channel > SBUS_CHANNELS) {
      Serial.println("{\"type\":\"error\",\"message\":\"Invalid channel number (1-16)\"}");
      return;
    }
    
    // Clear the override
    int channelIndex = channel - 1;
    channelOverrides[channelIndex].active = false;
    
    // Send confirmation
    Serial.print("{\"type\":\"channel_cleared\",\"channel\":");
    Serial.print(channel);
    Serial.print(",\"timestamp\":");
    Serial.print(millis());
    Serial.println("}");
  }
  
  else if (command == "clear_all") {
    // Clear all overrides
    for (int i = 0; i < SBUS_CHANNELS; i++) {
      channelOverrides[i].active = false;
    }
    
    Serial.print("{\"type\":\"all_cleared\",\"timestamp\":");
    Serial.print(millis());
    Serial.println("}");
  }
  
  else if (command == "status") {
    // Send current override status
    Serial.print("{\"type\":\"override_status\",\"timestamp\":");
    Serial.print(millis());
    Serial.print(",\"overrides\":[");
    
    bool first = true;
    for (int i = 0; i < SBUS_CHANNELS; i++) {
      if (channelOverrides[i].active) {
        if (!first) Serial.print(",");
        Serial.print("{\"channel\":");
        Serial.print(i + 1);
        Serial.print(",\"value\":");
        Serial.print(channelOverrides[i].value);
        Serial.print(",\"remaining_ms\":");
        Serial.print(OVERRIDE_TIMEOUT - (millis() - channelOverrides[i].timestamp));
        Serial.print("}");
        first = false;
      }
    }
    
    Serial.println("]}");
  }
  
  else if (command == "help") {
    Serial.println("{\"type\":\"help\",\"commands\":[");
    Serial.println("  {\"command\":\"set_channel\",\"params\":{\"channel\":\"1-16\",\"value\":\"0-2047\"},\"description\":\"Override a channel for 2 seconds\"},");
    Serial.println("  {\"command\":\"set_channels\",\"params\":{\"channels\":[{\"channel\":\"1-16\",\"value\":\"0-2047\"}]},\"description\":\"Override multiple channels for 2 seconds\"},");
    Serial.println("  {\"command\":\"clear_channel\",\"params\":{\"channel\":\"1-16\"},\"description\":\"Clear override for specific channel\"},");
    Serial.println("  {\"command\":\"clear_all\",\"params\":{},\"description\":\"Clear all channel overrides\"},");
    Serial.println("  {\"command\":\"status\",\"params\":{},\"description\":\"Get current override status\"},");
    Serial.println("  {\"command\":\"help\",\"params\":{},\"description\":\"Show this help message\"}");
    Serial.println("]}");
  }
  
  else {
    Serial.println("{\"type\":\"error\",\"message\":\"Unknown command\"}");
  }
}

// Simple JSON parsing functions without external library
String extractJsonValue(const String& json, const String& key) {
  String searchKey = "\"" + key + "\":";
  int keyIndex = json.indexOf(searchKey);
  if (keyIndex == -1) return "";
  
  int valueStart = keyIndex + searchKey.length();
  
  // Skip whitespace
  while (valueStart < json.length() && (json.charAt(valueStart) == ' ' || json.charAt(valueStart) == '\t')) {
    valueStart++;
  }
  
  if (valueStart >= json.length()) return "";
  
  int valueEnd = valueStart;
  bool inString = false;
  
  if (json.charAt(valueStart) == '"') {
    // String value
    inString = true;
    valueStart++; // Skip opening quote
    valueEnd = valueStart;
    
    while (valueEnd < json.length() && json.charAt(valueEnd) != '"') {
      valueEnd++;
    }
  } else {
    // Number value
    while (valueEnd < json.length() && 
           json.charAt(valueEnd) != ',' && 
           json.charAt(valueEnd) != '}' && 
           json.charAt(valueEnd) != ' ' &&
           json.charAt(valueEnd) != '\t') {
      valueEnd++;
    }
  }
  
  return json.substring(valueStart, valueEnd);
}

String extractJsonArray(const String& json, const String& key) {
  String searchKey = "\"" + key + "\":";
  int keyIndex = json.indexOf(searchKey);
  if (keyIndex == -1) return "";
  
  int arrayStart = json.indexOf('[', keyIndex);
  if (arrayStart == -1) return "";
  
  int bracketCount = 0;
  int arrayEnd = arrayStart;
  
  for (int i = arrayStart; i < json.length(); i++) {
    char c = json.charAt(i);
    if (c == '[') bracketCount++;
    else if (c == ']') bracketCount--;
    
    if (bracketCount == 0) {
      arrayEnd = i;
      break;
    }
  }
  
  return json.substring(arrayStart + 1, arrayEnd); // Return content without brackets
}

int countArrayElements(const String& arrayStr) {
  if (arrayStr.length() == 0) return 0;
  
  int count = 0;
  int braceCount = 0;
  bool inString = false;
  
  for (int i = 0; i < arrayStr.length(); i++) {
    char c = arrayStr.charAt(i);
    
    if (c == '"' && (i == 0 || arrayStr.charAt(i-1) != '\\')) {
      inString = !inString;
    } else if (!inString) {
      if (c == '{') {
        if (braceCount == 0) count++;
        braceCount++;
      } else if (c == '}') {
        braceCount--;
      }
    }
  }
  
  return count;
}

String getArrayElement(const String& arrayStr, int index) {
  if (arrayStr.length() == 0) return "";
  
  int currentIndex = 0;
  int braceCount = 0;
  int elementStart = -1;
  bool inString = false;
  
  for (int i = 0; i < arrayStr.length(); i++) {
    char c = arrayStr.charAt(i);
    
    if (c == '"' && (i == 0 || arrayStr.charAt(i-1) != '\\')) {
      inString = !inString;
    } else if (!inString) {
      if (c == '{') {
        if (braceCount == 0) {
          if (currentIndex == index) {
            elementStart = i;
          }
        }
        braceCount++;
      } else if (c == '}') {
        braceCount--;
        if (braceCount == 0 && currentIndex == index) {
          return arrayStr.substring(elementStart, i + 1);
        }
        if (braceCount == 0) {
          currentIndex++;
        }
      }
    }
  }
  
  return "";
}

void readSBUS() {
  while (sbusSerial.available()) {
    uint8_t incomingByte = sbusSerial.read();
    
    // Look for frame header
    if (bufferIndex == 0 && incomingByte == SBUS_HEADER) {
      sbusBuffer[bufferIndex++] = incomingByte;
    }
    // Continue filling buffer if we've started a frame
    else if (bufferIndex > 0 && bufferIndex < SBUS_FRAME_SIZE) {
      sbusBuffer[bufferIndex++] = incomingByte;
      
      // Check if we have a complete frame
      if (bufferIndex == SBUS_FRAME_SIZE) {
        if (sbusBuffer[SBUS_FRAME_SIZE - 1] == SBUS_FOOTER) {
          // Valid frame received
          parseSBUSFrame();
          frameReceived = true;
        }
        bufferIndex = 0; // Reset for next frame
      }
    }
    else {
      bufferIndex = 0; // Reset if we get out of sync
    }
  }
}

void parseSBUSFrame() {
  // Extract 16 channels from the 22 data bytes
  // Each channel is 11 bits, packed into the data array
  
  channels[0]  = ((sbusBuffer[1]       | sbusBuffer[2]  << 8)                 & 0x07FF);
  channels[1]  = ((sbusBuffer[2]  >> 3 | sbusBuffer[3]  << 5)                 & 0x07FF);
  channels[2]  = ((sbusBuffer[3]  >> 6 | sbusBuffer[4]  << 2 | sbusBuffer[5] << 10) & 0x07FF);
  channels[3]  = ((sbusBuffer[5]  >> 1 | sbusBuffer[6]  << 7)                 & 0x07FF);
  channels[4]  = ((sbusBuffer[6]  >> 4 | sbusBuffer[7]  << 4)                 & 0x07FF);
  channels[5]  = ((sbusBuffer[7]  >> 7 | sbusBuffer[8]  << 1 | sbusBuffer[9] << 9)  & 0x07FF);
  channels[6]  = ((sbusBuffer[9]  >> 2 | sbusBuffer[10] << 6)                 & 0x07FF);
  channels[7]  = ((sbusBuffer[10] >> 5 | sbusBuffer[11] << 3)                 & 0x07FF);
  channels[8]  = ((sbusBuffer[12]      | sbusBuffer[13] << 8)                 & 0x07FF);
  channels[9]  = ((sbusBuffer[13] >> 3 | sbusBuffer[14] << 5)                 & 0x07FF);
  channels[10] = ((sbusBuffer[14] >> 6 | sbusBuffer[15] << 2 | sbusBuffer[16] << 10) & 0x07FF);
  channels[11] = ((sbusBuffer[16] >> 1 | sbusBuffer[17] << 7)                 & 0x07FF);
  channels[12] = ((sbusBuffer[17] >> 4 | sbusBuffer[18] << 4)                 & 0x07FF);
  channels[13] = ((sbusBuffer[18] >> 7 | sbusBuffer[19] << 1 | sbusBuffer[20] << 9)  & 0x07FF);
  channels[14] = ((sbusBuffer[20] >> 2 | sbusBuffer[21] << 6)                 & 0x07FF);
  channels[15] = ((sbusBuffer[21] >> 5 | sbusBuffer[22] << 3)                 & 0x07FF);
  
  // Extract flags from byte 23
  uint8_t flags = sbusBuffer[23];
  frameLost = (flags & 0x04) != 0;
  failsafe = (flags & 0x08) != 0;
}

void printChannelData() {
  Serial.print("{\"type\":\"channels\",\"timestamp\":");
  Serial.print(millis());
  Serial.print(",\"input_channels\":[");
  
  for (int i = 0; i < SBUS_CHANNELS; i++) {
    Serial.print(channels[i]);
    if (i < SBUS_CHANNELS - 1) Serial.print(",");
  }
  
  Serial.print("],\"output_channels\":[");
  
  for (int i = 0; i < SBUS_CHANNELS; i++) {
    Serial.print(outputChannels[i]);
    if (i < SBUS_CHANNELS - 1) Serial.print(",");
  }
  
  Serial.print("],\"overrides\":[");
  
  bool first = true;
  for (int i = 0; i < SBUS_CHANNELS; i++) {
    if (channelOverrides[i].active) {
      if (!first) Serial.print(",");
      Serial.print(i + 1);
      first = false;
    }
  }
  
  Serial.print("],\"frameLost\":");
  Serial.print(frameLost ? "true" : "false");
  Serial.print(",\"failsafe\":");
  Serial.print(failsafe ? "true" : "false");
  Serial.println("}");
}

void printSbusStatus(bool connected) {
  Serial.print("{\"type\":\"status\",\"timestamp\":");
  Serial.print(millis());
  Serial.print(",\"connected\":");
  Serial.print(connected ? "true" : "false");
  Serial.println("}");
}

// Helper function to convert SBUS values to microseconds (typical servo range)
int sbusToMicros(uint16_t sbusValue) {
  // SBUS range: 172-1811 typically maps to 1000-2000 microseconds
  return map(sbusValue, 172, 1811, 1000, 2000);
}

// Helper function to convert SBUS values to percentage (-100 to +100)
float sbusToPercent(uint16_t sbusValue) {
  // Center value is typically around 992
  return ((float)sbusValue - 992.0) / 819.0 * 100.0;
}

// Function to write SBUS frame to TX pin
void writeSBUS() {
  // Send the complete SBUS frame buffer
  sbusSerial.write(sbusBuffer, SBUS_FRAME_SIZE);
}

// Function to create a custom SBUS frame with channel values
void createSBUSFrame(uint16_t channelValues[SBUS_CHANNELS]) {
  // Clear buffer
  memset(sbusBuffer, 0, SBUS_FRAME_SIZE);
  
  // Set header and footer
  sbusBuffer[0] = SBUS_HEADER;
  sbusBuffer[24] = SBUS_FOOTER;
  
  // Pack 16 channels into 22 data bytes (11 bits per channel)
  sbusBuffer[1] = (uint8_t) (channelValues[0] & 0x07FF);
  sbusBuffer[2] = (uint8_t) ((channelValues[0] & 0x07FF) >> 8 | (channelValues[1] & 0x07FF) << 3);
  sbusBuffer[3] = (uint8_t) ((channelValues[1] & 0x07FF) >> 5 | (channelValues[2] & 0x07FF) << 6);
  sbusBuffer[4] = (uint8_t) ((channelValues[2] & 0x07FF) >> 2);
  sbusBuffer[5] = (uint8_t) ((channelValues[2] & 0x07FF) >> 10 | (channelValues[3] & 0x07FF) << 1);
  sbusBuffer[6] = (uint8_t) ((channelValues[3] & 0x07FF) >> 7 | (channelValues[4] & 0x07FF) << 4);
  sbusBuffer[7] = (uint8_t) ((channelValues[4] & 0x07FF) >> 4 | (channelValues[5] & 0x07FF) << 7);
  sbusBuffer[8] = (uint8_t) ((channelValues[5] & 0x07FF) >> 1);
  sbusBuffer[9] = (uint8_t) ((channelValues[5] & 0x07FF) >> 9 | (channelValues[6] & 0x07FF) << 2);
  sbusBuffer[10] = (uint8_t) ((channelValues[6] & 0x07FF) >> 6 | (channelValues[7] & 0x07FF) << 5);
  sbusBuffer[11] = (uint8_t) ((channelValues[7] & 0x07FF) >> 3);
  sbusBuffer[12] = (uint8_t) (channelValues[8] & 0x07FF);
  sbusBuffer[13] = (uint8_t) ((channelValues[8] & 0x07FF) >> 8 | (channelValues[9] & 0x07FF) << 3);
  sbusBuffer[14] = (uint8_t) ((channelValues[9] & 0x07FF) >> 5 | (channelValues[10] & 0x07FF) << 6);
  sbusBuffer[15] = (uint8_t) ((channelValues[10] & 0x07FF) >> 2);
  sbusBuffer[16] = (uint8_t) ((channelValues[10] & 0x07FF) >> 10 | (channelValues[11] & 0x07FF) << 1);
  sbusBuffer[17] = (uint8_t) ((channelValues[11] & 0x07FF) >> 7 | (channelValues[12] & 0x07FF) << 4);
  sbusBuffer[18] = (uint8_t) ((channelValues[12] & 0x07FF) >> 4 | (channelValues[13] & 0x07FF) << 7);
  sbusBuffer[19] = (uint8_t) ((channelValues[13] & 0x07FF) >> 1);
  sbusBuffer[20] = (uint8_t) ((channelValues[13] & 0x07FF) >> 9 | (channelValues[14] & 0x07FF) << 2);
  sbusBuffer[21] = (uint8_t) ((channelValues[14] & 0x07FF) >> 6 | (channelValues[15] & 0x07FF) << 5);
  sbusBuffer[22] = (uint8_t) ((channelValues[15] & 0x07FF) >> 3);
  
  // Flags byte (can be modified to set failsafe, frame lost, etc.)
  sbusBuffer[23] = 0x00; // No flags set
}