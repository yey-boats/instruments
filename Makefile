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
PROJECT_VERSION ?= $(shell cat VERSION)

# Lab default for the current setup: SignalK on nav-server over SSH,
# device reachable only via OTA + BLE. Override on the command line if
# you spin up a different host or rotate creds.
REMOTE_HOST ?= nav-server
REMOTE_USER ?= $(if $(findstring @,$(REMOTE_HOST)),$(firstword $(subst @, ,$(REMOTE_HOST))),nav-server)
REMOTE_DIR  ?= /home/$(REMOTE_USER)/espdisp-signalk
REMOTE_SK_HOST ?= $(lastword $(subst @, ,$(REMOTE_HOST)))

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
	@printf "  PROJECT_VERSION firmware/plugin version (default: $(PROJECT_VERSION))\n"

setup:  ## First-time setup: copy secrets.h template, verify PlatformIO
	@command -v $(PIO) >/dev/null 2>&1 || { \
	  echo "PlatformIO not found. Install with: pip install platformio" >&2; exit 1; }
	@test -f include/secrets.h || cp include/secrets.h.example include/secrets.h
	@echo "Setup complete. Edit include/secrets.h if you want to bake in WiFi credentials."

version:  ## Print project version and verify metadata
	@python3 tools/check_version.py

version-check:  ## Verify project/plugin version metadata is consistent
	@python3 tools/check_version.py

version-set:  ## Set project/plugin version (use VERSION=0.1.0)
	@test -n "$(VERSION)" || { echo "Usage: make version-set VERSION=0.1.0" >&2; exit 1; }
	python3 tools/set_version.py $(VERSION)

build: version-check  ## Build firmware
	ESPDISP_VERSION=$(PROJECT_VERSION) $(PIO) run -e $(ENV)

test: version-check  ## Run host-side unit tests
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
	ESPDISP_VERSION=$(PROJECT_VERSION) $(PIO) run -e $(ENV) -t upload --upload-port $(PORT)

discover:  ## Discover the device (mDNS, then BLE); prints IP. NAME=<device_id> for exact match.
	@python3 tools/discover_device.py $(if $(NAME),--name $(NAME),)

discover-json:  ## Same as discover but prints full JSON record.
	@python3 tools/discover_device.py --json $(if $(NAME),--name $(NAME),)

# ota target: explicit DEVICE_IP wins. Otherwise we run tools/discover_device.py
# (mDNS _espdisp._tcp -> _arduino._tcp -> BLE NUS `ip` query). Both transports
# are unauthenticated - any spoofed responder on the LAN/BLE can steer the
# upload, and if OTA_PASSWORD is empty in secrets.h the attacker captures
# firmware.bin (including any baked-in WiFi PSK). For unattended/lab use,
# pin DEVICE_IP=<addr> or pass NAME=<device_id> for an exact-match lookup
# that errors on ambiguity.
ota: setup  ## Flash via WiFi. DEVICE_IP=<ip> pins; NAME=<device_id> scopes discovery.
	$(eval OTA_TARGET := $(if $(DEVICE_IP),$(DEVICE_IP),$(shell python3 tools/discover_device.py $(if $(NAME),--name $(NAME),))))
	@test -n "$(OTA_TARGET)" || { echo "OTA target not resolved: pass DEVICE_IP=<ip> or fix discovery (see 'make discover')" >&2; exit 1; }
	@echo "[ota] target=$(OTA_TARGET)"
	ESPDISP_VERSION=$(PROJECT_VERSION) $(PIO) run -e ota -t upload --upload-port $(OTA_TARGET)

# flash-auto: USB if plugged in. OTA fallback is opt-in via OTA=1 because
# discovery is unauthenticated; we don't want a `make flash-auto` invocation
# meant for USB to silently push firmware over the network to whoever wins
# the discovery race.
flash-auto: setup  ## USB if a serial port is present; pass OTA=1 to allow OTA fallback.
	@if [ -n "$(PORT)" ]; then \
	  echo "[flash-auto] USB port $(PORT)"; \
	  $(MAKE) flash PORT=$(PORT); \
	elif [ "$(OTA)" = "1" ]; then \
	  echo "[flash-auto] no USB port; OTA=1 set - using OTA discovery"; \
	  $(MAKE) ota $(if $(NAME),NAME=$(NAME),) $(if $(DEVICE_IP),DEVICE_IP=$(DEVICE_IP),); \
	else \
	  echo "no USB serial port found. Re-run with OTA=1 to allow network flashing (uses unauthenticated discovery - see 'make ota' help)." >&2; \
	  exit 1; \
	fi

monitor:  ## Open serial monitor at 115200 baud
	@test -n "$(PORT)" || { echo "No USB serial port found. Set PORT=<path>." >&2; exit 1; }
	$(PIO) device monitor --baud 115200 --port $(PORT)

ble:  ## Open BLE console (sends 'ip' + 'sk-status', then streams logs)
	python3 tools/ble_console.py

ble-cmd:  ## Send one BLE command (use CMD="sk-status")
	@test -n "$(CMD)" || { echo "Usage: make ble-cmd CMD='sk-status'" >&2; exit 1; }
	python3 tools/ble_console.py "$(CMD)"

provision:  ## Onboard a fresh MFD onto the lab (BLE; defaults from .env.test)
	python3 tools/provision_device.py $(PROVISION_ARGS)

logs:  ## Listen for UDP log broadcasts on port 9999
	python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', 9999)); print('listening on :9999  (^C to stop)'); \
	  $(_)\nwhile True:\n    d, a = s.recvfrom(2048)\n    print(f'[{a[0]}] {d.decode(\"utf-8\", \"replace\").rstrip()}')"

demo-up:  ## Start SignalK locally in Docker (legacy/local-host path)
	@SK_HOST=$(SK_HOST) SK_PORT=$(SK_PORT) ./signalk/scripts/run.sh

demo-down:  ## Stop local fake_boat + SignalK
	@./signalk/scripts/stop.sh

demo-up-remote:  ## Start SignalK on REMOTE_HOST (default nav-server) via SSH+Docker
	@REMOTE_HOST=$(REMOTE_HOST) REMOTE_DIR=$(REMOTE_DIR) \
	  SK_HOST=$(REMOTE_SK_HOST) SK_PORT=$(SK_PORT) \
	  ./signalk/scripts/run-remote.sh

demo-down-remote:  ## Stop the remote SignalK container + local fake_boat
	@REMOTE_HOST=$(REMOTE_HOST) ./signalk/scripts/stop-remote.sh

sys-test-remote:  ## Run unattended system tests against the lab device + remote SK (sources .env.test)
	@test -f .env.test || { echo ".env.test missing - regenerate from this repo" >&2; exit 1; }
	@set -a; . ./.env.test; set +a; \
	  if [ -z "$$ESPDISP_HOST" ]; then \
	    echo "ESPDISP_HOST still unset after sourcing .env.test" >&2; exit 1; \
	  fi; \
	  pytest tests/system/unattended

# The full transport: native BLE on this Mac (the lab AP is on a
# combo WiFi+BT chip on nav-server, so its BT is starved while hostapd
# runs — BLE has to live on the dev laptop) plus SSH-tunneled HTTP
# through nav-server to the device's port 80 (because the WAN router has no
# route to 10.42.0.0/24 yet). Tunnel auto-opens on `sys-test-mac`
# and is torn down after.
TUNNEL_PORT ?= 10067
DEVICE_LAN_IP ?= 10.42.0.67

sys-test-mac:  ## Run sys-tests from Mac: native BLE + SSH-tunneled HTTP via nav-server
	@test -f .env.test || { echo ".env.test missing" >&2; exit 1; }
	@pkill -f 'ssh.*$(TUNNEL_PORT):$(DEVICE_LAN_IP)' 2>/dev/null || true
	@ssh -fN -L $(TUNNEL_PORT):$(DEVICE_LAN_IP):80 $(REMOTE_HOST) \
	  && echo "tunnel up: localhost:$(TUNNEL_PORT) -> $(DEVICE_LAN_IP):80"
	@trap 'pkill -f "ssh.*$(TUNNEL_PORT):$(DEVICE_LAN_IP)" 2>/dev/null || true' EXIT; \
	  set -a; . ./.env.test; set +a; \
	  ESPDISP_HOST=localhost:$(TUNNEL_PORT) ESPDISP_BLE_NAME=espdisp \
	    pytest tests/system/unattended --espdisp-no-udp-discovery --espdisp-no-discovery

lint: version-check  ## Check C++ formatting and Python syntax
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
	@test "$$(cat VERSION)" = "$$(echo $(VERSION) | sed 's/^v//')" || { \
	  echo "VERSION file must match tag $(VERSION)" >&2; exit 1; }
	git tag -a $(VERSION) -m "Release $(VERSION)"
	@echo "Tag $(VERSION) created locally. Push with: git push origin $(VERSION)"

clean:  ## Remove build artifacts (keeps include/secrets.h)
	$(PIO) run -t clean 2>/dev/null || true
	rm -rf .pio

.PHONY: help setup version version-check version-set build test sys-test sys-test-remote \
        sys-test-mac sys-test-attended flash ota monitor ble ble-cmd provision logs \
        demo-up demo-down demo-up-remote demo-down-remote \
        lint format backup release-tag clean
