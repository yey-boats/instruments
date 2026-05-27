# esp32-boat-mfd development pipeline.
# Run `make help` for the list of targets.
#
# Override defaults at invocation:
#   make ota DEVICE_IP=10.0.0.42
#   make flash PORT=/dev/cu.usbserial-110
#   make demo-up SK_HOST=192.168.1.50

ENV       ?= esp32-4848s040
PORT      ?= auto
DEVICE_IP ?=
SK_HOST   ?= localhost
SK_PORT   ?= 3000

PIO ?= pio

# Resolve PORT=auto to the first matching CH340 device on macOS / Linux.
ifeq ($(PORT),auto)
	PORT := $(shell ls /dev/cu.usbserial-* /dev/ttyUSB* 2>/dev/null | head -1)
endif

# ----- public targets -------------------------------------------------------

.DEFAULT_GOAL := help

help:  ## Show this help
	@printf "Available targets:\n\n"
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\nVariables (override with VAR=value):\n"
	@printf "  ENV         build env (default: $(ENV))\n"
	@printf "  PORT        serial port for USB flash (default: auto-detect)\n"
	@printf "  DEVICE_IP   target IP for OTA flash (required for 'make ota')\n"
	@printf "  SK_HOST     SignalK server host for 'make demo' (default: localhost)\n"

setup:  ## First-time setup: copy secrets.h template, verify PlatformIO
	@command -v $(PIO) >/dev/null 2>&1 || { \
	  echo "PlatformIO not found. Install with: pip install platformio" >&2; exit 1; }
	@test -f include/secrets.h || cp include/secrets.h.example include/secrets.h
	@echo "Setup complete. Edit include/secrets.h if you want to bake in WiFi credentials."

build:  ## Build firmware
	$(PIO) run -e $(ENV)

test:  ## Run host-side unit tests
	$(PIO) test -e native

sys-test:  ## Run unattended system tests against ESPDISP_HOST (set env var)
	@if [ -z "$$ESPDISP_HOST" ]; then \
		echo "ESPDISP_HOST not set. Example: ESPDISP_HOST=esp32-boat-mfd.local make sys-test"; \
		exit 1; \
	fi
	pytest tests/system/unattended

sys-test-attended:  ## Open the attended-test checklist
	@echo "Open tests/system/attended/README.md and step through each numbered file."
	@command -v open >/dev/null && open tests/system/attended/README.md || true

flash: setup  ## Flash via USB (uses PORT, auto-detected if not set)
	@test -n "$(PORT)" || { echo "No USB serial port found. Set PORT=<path>." >&2; exit 1; }
	$(PIO) run -e $(ENV) -t upload --upload-port $(PORT)

ota: setup  ## Flash via WiFi (requires DEVICE_IP)
	@test -n "$(DEVICE_IP)" || { echo "Usage: make ota DEVICE_IP=<ip>" >&2; exit 1; }
	$(PIO) run -e ota -t upload --upload-port $(DEVICE_IP)

monitor:  ## Open serial monitor at 115200 baud
	@test -n "$(PORT)" || { echo "No USB serial port found. Set PORT=<path>." >&2; exit 1; }
	$(PIO) device monitor --baud 115200 --port $(PORT)

ble:  ## Open BLE console (sends 'ip' + 'sk-status', then streams logs)
	python3 tools/ble_console.py

ble-cmd:  ## Send one BLE command (use CMD="sk-status")
	@test -n "$(CMD)" || { echo "Usage: make ble-cmd CMD='sk-status'" >&2; exit 1; }
	python3 tools/ble_console.py "$(CMD)"

logs:  ## Listen for UDP log broadcasts on port 9999
	python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', 9999)); print('listening on :9999  (^C to stop)'); \
	  $(_)\nwhile True:\n    d, a = s.recvfrom(2048)\n    print(f'[{a[0]}] {d.decode(\"utf-8\", \"replace\").rstrip()}')"

demo-up:  ## Start SignalK in Docker and push synthetic boat data
	@SK_HOST=$(SK_HOST) SK_PORT=$(SK_PORT) ./signalk/scripts/run.sh

demo-down:  ## Stop fake_boat and SignalK
	@./signalk/scripts/stop.sh

lint:  ## Check C++ formatting and Python syntax
	@find src include test -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror
	@python3 -m py_compile tools/*.py

format:  ## Auto-format C++ sources in place
	@find src include test -name '*.cpp' -o -name '*.h' | xargs clang-format -i

backup:  ## Dump the device flash to backup/full_flash_16MB.bin (chunked, resumable)
	@test -n "$(PORT)" || { echo "No USB serial port found. Set PORT=<path>." >&2; exit 1; }
	PORT=$(PORT) bash tools/dump_chunked.sh

release-tag:  ## Tag a release locally (use VERSION=v0.1.0). Does NOT push.
	@test -n "$(VERSION)" || { echo "Usage: make release-tag VERSION=v0.1.0" >&2; exit 1; }
	@echo "$(VERSION)" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-[a-z0-9.]+)?$$' \
	  || { echo "Version must look like v0.1.0 or v0.1.0-rc1" >&2; exit 1; }
	git tag -a $(VERSION) -m "Release $(VERSION)"
	@echo "Tag $(VERSION) created locally. Push with: git push origin $(VERSION)"

clean:  ## Remove build artifacts (keeps include/secrets.h)
	$(PIO) run -t clean 2>/dev/null || true
	rm -rf .pio

.PHONY: help setup build test flash ota monitor ble ble-cmd logs demo-up demo-down \
        lint format backup release-tag clean
