# Arduino ESP32-C3 Development Makefile

# Default values
BOARD ?= esp32:esp32:esp32c3
OPTIONS ?= :CDCOnBoot=cdc
PORT ?= /dev/ttyACM0
SKETCH ?= .

compile:
	@echo "Compiling sketch: $(SKETCH)"
	arduino-cli compile -b $(BOARD)$(OPTIONS) $(SKETCH)

upload:
	@echo "Uploading sketch: $(SKETCH) to $(PORT)"
	arduino-cli upload -b $(BOARD)$(OPTIONS) -p $(PORT) $(SKETCH)

monitor:
	@echo "Opening serial monitor on $(PORT)"
	arduino-cli monitor -p $(PORT)

# List connected boards
list-boards:
	@echo "Listing connected boards..."
	arduino-cli board list

# Install library
install-lib:
	@read -p "Enter library name: " lib; \
	arduino-cli lib install "$$lib"

# Search libraries
search-lib:
	@read -p "Enter search term: " term; \
	arduino-cli lib search "$$term"

help:
	@echo "Available commands:"
	@echo "  compile           - Compile sketch (SKETCH=path)"
	@echo "  upload            - Upload sketch (SKETCH=path PORT=device)"
	@echo "  monitor           - Open serial monitor (PORT=device)"
	@echo "  list-boards       - List connected boards"
	@echo "  install-lib       - Install library"
	@echo "  search-lib        - Search for libraries"
	@echo ""
	@echo "Variables:"
	@echo "  BOARD=$(BOARD)"
	@echo "  OPTIONS=$(OPTIONS)"
	@echo "  PORT=$(PORT)"
	@echo "  SKETCH=$(SKETCH)"