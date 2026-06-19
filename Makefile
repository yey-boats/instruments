# Yey Boats Instruments development pipeline.
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
# VERSION holds the repo-configured MAJOR.MINOR.0 source of truth. MAJOR.MINOR
# are authoritative; the third component (BUILD) is owned by CI
# (github.run_number). Local builds stamp MAJOR.MINOR.0 so a bench binary is
# always identifiable as a local build (run number 0). CI overrides
# YEYBOATS_VERSION with MAJOR.MINOR.<run_number>.
VERSION_BASE    := $(shell cut -d. -f1-2 VERSION)
PROJECT_VERSION ?= $(VERSION_BASE).0

# Lab default for the current setup: SignalK on nav-server over SSH,
# device reachable only via OTA + BLE. Override on the command line if
# you spin up a different host or rotate creds.
REMOTE_HOST ?= nav-server
REMOTE_USER ?= $(if $(findstring @,$(REMOTE_HOST)),$(firstword $(subst @, ,$(REMOTE_HOST))),nav-server)
REMOTE_DIR  ?= /home/$(REMOTE_USER)/yeydisp-signalk
REMOTE_SK_HOST ?= $(lastword $(subst @, ,$(REMOTE_HOST)))

# The SignalK manager plugin + its deployment scripts now live in the
# sibling repo yey-boats/Instruments-manager. Clone it next to this repo
# (../signalk-espdisp-manager) or override MANAGER_DIR to point elsewhere.
MANAGER_DIR ?= ../signalk-espdisp-manager

PIO ?= pio
CLANG_FORMAT ?= clang-format

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
	@printf "  PROJECT_VERSION firmware version MAJOR.MINOR.BUILD (default: $(PROJECT_VERSION); CI sets BUILD=run_number)\n"

setup:  ## First-time setup: copy secrets.h template, verify PlatformIO
	@command -v $(PIO) >/dev/null 2>&1 || { \
	  echo "PlatformIO not found. Install with: pip install platformio" >&2; exit 1; }
	@if [ -n "$$YEYBOATS_OTA_PASSWORD" ] || [ -n "$$YEYBOATS_WIFI_SSID" ] || [ -n "$$YEYBOATS_WIFI_PASS" ]; then \
	  python3 tools/write_secrets_header.py; \
	elif [ ! -f include/secrets.h ]; then \
	  cp include/secrets.h.example include/secrets.h; \
	fi
	@echo "Setup complete. Set YEYBOATS_OTA_PASSWORD to bake in ArduinoOTA auth."

version:  ## Print project version and verify metadata
	@python3 tools/check_version.py

version-check:  ## Verify project/plugin version metadata is consistent
	@python3 tools/check_version.py

version-set:  ## Set project MAJOR.MINOR (use VERSION=0.3; BUILD is CI-owned)
	@test -n "$(VERSION)" || { echo "Usage: make version-set VERSION=0.3 (MAJOR.MINOR; BUILD is set by CI)" >&2; exit 1; }
	python3 tools/set_version.py $(VERSION)

build: setup version-check  ## Build firmware
	YEYBOATS_VERSION=$(PROJECT_VERSION) $(PIO) run -e $(ENV)

test: version-check  ## Run host-side unit tests
	$(PIO) test -e native

sim:  ## Render dashboard at all resolutions via headless LVGL harness -> docs/sim-shots/
	bash tools/sim_render.sh

sys-test:  ## Run unattended system tests against YEYBOATS_HOST (set env var)
	@if [ -z "$$YEYBOATS_HOST" ]; then \
		echo "YEYBOATS_HOST not set. Example: YEYBOATS_HOST=yey-boats-instruments.local make sys-test"; \
		exit 1; \
	fi
	pytest tests/system/unattended

sys-test-attended:  ## Open the attended-test checklist
	@echo "Open tests/system/attended/README.md and step through each numbered file."
	@command -v open >/dev/null && open tests/system/attended/README.md || true

flash: setup  ## Flash via USB (uses PORT, auto-detected if not set)
	@test -n "$(PORT)" || { echo "No USB serial port found. Set PORT=<path>." >&2; exit 1; }
	YEYBOATS_VERSION=$(PROJECT_VERSION) $(PIO) run -e $(ENV) -t upload --upload-port $(PORT)

discover:  ## Discover the device (mDNS, then BLE); prints IP. NAME=<device_id> for exact match.
	@python3 tools/discover_device.py $(if $(NAME),--name $(NAME),)

discover-json:  ## Same as discover but prints full JSON record.
	@python3 tools/discover_device.py --json $(if $(NAME),--name $(NAME),)

# ota target: explicit DEVICE_IP wins. Otherwise we run tools/discover_device.py
# (mDNS _yeyboats._tcp -> _arduino._tcp -> BLE NUS `ip` query). Both transports
# are unauthenticated - any spoofed responder on the LAN/BLE can steer the
# upload, and if OTA_PASSWORD is empty in secrets.h the attacker captures
# firmware.bin (including any baked-in WiFi PSK). For unattended/lab use,
# pin DEVICE_IP=<addr> or pass NAME=<device_id> for an exact-match lookup
# that errors on ambiguity.
ota: setup  ## Flash via WiFi. DEVICE_IP=<ip> pins; NAME=<device_id> scopes discovery.
	$(eval OTA_TARGET := $(if $(DEVICE_IP),$(DEVICE_IP),$(shell python3 tools/discover_device.py $(if $(NAME),--name $(NAME),))))
	@test -n "$(OTA_TARGET)" || { echo "OTA target not resolved: pass DEVICE_IP=<ip> or fix discovery (see 'make discover')" >&2; exit 1; }
	@echo "[ota] target=$(OTA_TARGET)"
	YEYBOATS_VERSION=$(PROJECT_VERSION) $(PIO) run -e $(if $(filter %-debug,$(ENV)),esp32-4848s040-debug,$(ENV))
	bash tools/ota_flash.sh $(OTA_TARGET) \
	  .pio/build/$(if $(filter %-debug,$(ENV)),esp32-4848s040-debug,$(ENV))/firmware.bin \
	  $(if $(REMOTE),--remote $(REMOTE),)

# ota-verify: the same flash, but post-verifies via /api/state.device.build
# instead of trusting espota.py's exit code. espota.py reports
# "Error Uploading" whenever the device reboots before sending its final
# ACK (which is most successful flashes), so the bare `make ota` target
# has a 100% false-failure rate even when the new FW is on the device.
# This target wraps espota in tools/ota_flash.sh, ignores its exit code,
# waits for the device to reboot, and confirms the new firmware is
# actually running by matching __DATE__ __TIME__ baked into the .bin
# against device.build in /api/state. Pass REMOTE=user@host to flash
# through a relay (used when this host can't reach the device subnet,
# e.g. via compulab on the yey-net AP).
ota-verify: setup  ## OTA + verify via /api/state. REMOTE=user@host to flash through a relay.
	$(eval OTA_TARGET := $(if $(DEVICE_IP),$(DEVICE_IP),$(shell python3 tools/discover_device.py $(if $(NAME),--name $(NAME),))))
	@test -n "$(OTA_TARGET)" || { echo "OTA target not resolved" >&2; exit 1; }
	YEYBOATS_VERSION=$(PROJECT_VERSION) $(PIO) run -e $(if $(filter %-debug,$(ENV)),esp32-4848s040-debug,$(ENV))
	bash tools/ota_flash.sh $(OTA_TARGET) \
	  .pio/build/$(if $(filter %-debug,$(ENV)),esp32-4848s040-debug,$(ENV))/firmware.bin \
	  $(if $(REMOTE),--remote $(REMOTE),)

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

logs:  ## Listen for UDP log broadcasts on port 9999 (debug FW only)
	python3 tools/lab-logger/loglistener.py

# Lab logger persistence: ship tools/lab-logger/ to REMOTE via SSH and
# install the systemd unit + logrotate config there. REMOTE defaults to
# compulab@192.168.2.11 (the lab box hosting the yey-net AP). The remote
# user needs sudo (interactive password prompt is fine; `-t` allocates
# a pty so sudo can read the password).
REMOTE ?= compulab@192.168.2.11

# Unattended deploy: ship tools/lab-logger/ to REMOTE and install. Pulls
# the remote sudo password from $REMOTE_SUDO_PASS (env or .env.test.local,
# which is already gitignored). With the password set, no prompts and no
# tty required - safe to run from CI, cron, or this assistant. Without
# it we install a passwordless-sudo dropfile on first contact (one-time;
# the bootstrap step is the one place we accept an interactive prompt
# via ssh -tt). Subsequent runs are silent.
lab-logger-deploy:  ## Install UDP log listener + logrotate on REMOTE, fully unattended
	@test -n "$(REMOTE)" || { echo "REMOTE not set" >&2; exit 1; }
	@bash tools/lab-logger/deploy.sh $(REMOTE)

lab-logger-status:  ## Show systemd status + last 20 log lines on REMOTE
	@ssh $(REMOTE) 'sudo -n systemctl --no-pager --full status yeydisp-loglistener 2>/dev/null || sudo -S systemctl --no-pager --full status yeydisp-loglistener; echo ---; sudo -n tail -n 20 /var/log/yeydisp/device.log 2>/dev/null || true'

lab-logger-tail:  ## Stream live logs from REMOTE
	@ssh -t $(REMOTE) 'sudo -n tail -F /var/log/yeydisp/device.log'

# One-shot orchestrator: build debug FW, flash via OTA, deploy lab-logger,
# show recent logs. Each step is idempotent and unattended (provided
# REMOTE_SUDO_PASS is set the first time, see lab-logger-deploy).
lab-up:  ## Build debug FW + OTA flash (verified) + deploy lab-logger
	@$(MAKE) ota-verify ENV=esp32-4848s040-debug $(if $(DEVICE_IP),DEVICE_IP=$(DEVICE_IP),) $(if $(NAME),NAME=$(NAME),) $(if $(REMOTE),REMOTE=$(REMOTE),)
	@$(MAKE) lab-logger-deploy REMOTE=$(REMOTE)
	@echo "[lab-up] done. Recent logs:"
	@$(MAKE) lab-logger-status REMOTE=$(REMOTE)

demo-up:  ## Start SignalK locally in Docker (from the Instruments-manager repo)
	@test -d "$(MANAGER_DIR)/deploy" || { echo "Manager repo not found at $(MANAGER_DIR). Clone yey-boats/Instruments-manager next to this repo (or set MANAGER_DIR)." >&2; exit 1; }
	@SK_HOST=$(SK_HOST) SK_PORT=$(SK_PORT) $(MANAGER_DIR)/deploy/scripts/run.sh

demo-down:  ## Stop local simulator + SignalK (from the Instruments-manager repo)
	@test -d "$(MANAGER_DIR)/deploy" || { echo "Manager repo not found at $(MANAGER_DIR). Clone yey-boats/Instruments-manager next to this repo (or set MANAGER_DIR)." >&2; exit 1; }
	@$(MANAGER_DIR)/deploy/scripts/stop.sh

demo-up-remote:  ## Start SignalK on REMOTE_HOST (default nav-server) via SSH+Docker
	@test -d "$(MANAGER_DIR)/deploy" || { echo "Manager repo not found at $(MANAGER_DIR). Clone yey-boats/Instruments-manager next to this repo (or set MANAGER_DIR)." >&2; exit 1; }
	@REMOTE_HOST=$(REMOTE_HOST) REMOTE_DIR=$(REMOTE_DIR) \
	  SK_HOST=$(REMOTE_SK_HOST) SK_PORT=$(SK_PORT) \
	  $(MANAGER_DIR)/deploy/scripts/run-remote.sh

demo-down-remote:  ## Stop the remote SignalK container + local simulator
	@test -d "$(MANAGER_DIR)/deploy" || { echo "Manager repo not found at $(MANAGER_DIR). Clone yey-boats/Instruments-manager next to this repo (or set MANAGER_DIR)." >&2; exit 1; }
	@REMOTE_HOST=$(REMOTE_HOST) $(MANAGER_DIR)/deploy/scripts/stop-remote.sh

# ---------------------------------------------------------------------------
# Server provisioning (one-time + re-deploy)
# ---------------------------------------------------------------------------
# Full initial setup of a fresh nav-server: Docker, SignalK systemd service,
# lab-logger, WiFi AP (hostapd+dnsmasq), and ufw firewall rules.
# Re-running is idempotent (each step is guarded).
#
# Common usage:
#   make server-setup REMOTE=compulab@192.168.2.11
#   make server-setup REMOTE=compulab@192.168.2.11 SETUP_FLAGS=--sk-only
#   make server-status REMOTE=compulab@192.168.2.11

SETUP_FLAGS ?=

server-setup:  ## Provision/re-provision REMOTE nav-server (Docker, SK, AP, logger, fw)
	@test -n "$(REMOTE)" || { echo "REMOTE not set (e.g. REMOTE=compulab@192.168.2.11)" >&2; exit 1; }
	@bash tools/server-setup/deploy.sh $(REMOTE) $(SETUP_FLAGS)

server-sk-only:  ## Re-deploy SK config/plugins and restart the SK service on REMOTE
	@test -n "$(REMOTE)" || { echo "REMOTE not set" >&2; exit 1; }
	@bash tools/server-setup/deploy.sh $(REMOTE) --sk-only

server-status:  ## Show service status (SK + AP + logger) on REMOTE
	@test -n "$(REMOTE)" || { echo "REMOTE not set" >&2; exit 1; }
	@ssh $(REMOTE) '\
	  echo "=== yeydisp-signalk ==="; \
	  sudo -n systemctl --no-pager status yeydisp-signalk 2>/dev/null | head -10; \
	  echo "=== yeydisp-lab-ap ==="; \
	  sudo -n systemctl --no-pager status yeydisp-lab-ap 2>/dev/null | head -6; \
	  echo "=== yeydisp-loglistener ==="; \
	  sudo -n systemctl --no-pager status yeydisp-loglistener 2>/dev/null | head -6; \
	  echo "=== recent device log ==="; \
	  sudo -n tail -n 20 /var/log/yeydisp/device.log 2>/dev/null || echo "(no log yet)"'

server-teardown:  ## Stop all lab services on REMOTE (SK + AP + logger) – non-destructive
	@test -n "$(REMOTE)" || { echo "REMOTE not set" >&2; exit 1; }
	@ssh $(REMOTE) '\
	  sudo -n systemctl stop yeydisp-signalk yeydisp-lab-ap yeydisp-loglistener 2>/dev/null; \
	  echo "stopped"'

sys-test-remote:  ## Run unattended system tests against the lab device + remote SK (sources .env.test)
	@test -f .env.test || { echo ".env.test missing - regenerate from this repo" >&2; exit 1; }
	@set -a; . ./.env.test; set +a; \
	  if [ -z "$$YEYBOATS_HOST" ]; then \
	    echo "YEYBOATS_HOST still unset after sourcing .env.test" >&2; exit 1; \
	  fi; \
	  pytest tests/system/unattended

# Manager plugin load test against the lab SignalK. The plugin + its load
# harness now live in the sibling Instruments-manager repo ($(MANAGER_DIR)).
# Sources .env.test for SIGNALK_URL/USERNAME/PASSWORD; npm doesn't inherit
# those from a bare `npm run` so we have to spell it out here. Overrides:
#   make load-test DEVICES=100 DURATION=60
DEVICES  ?= 20
DURATION ?= 30
load-test:  ## Plugin load test against $SIGNALK_URL (runs from the Instruments-manager repo)
	@test -f .env.test || { echo ".env.test missing" >&2; exit 1; }
	@test -f "$(MANAGER_DIR)/tools/load-test.js" || { echo "Manager repo not found at $(MANAGER_DIR). Clone yey-boats/Instruments-manager next to this repo (or set MANAGER_DIR)." >&2; exit 1; }
	@set -a; . ./.env.test; set +a; \
	  : "$${SIGNALK_URL:=http://$${SK_HOST:-localhost}:$${SK_PORT:-3000}}"; \
	  cd "$(MANAGER_DIR)" && \
	  SIGNALK_URL="$$SIGNALK_URL" \
	  SIGNALK_USERNAME="$$SIGNALK_USERNAME" \
	  SIGNALK_PASSWORD="$$SIGNALK_PASSWORD" \
	  node tools/load-test.js --devices $(DEVICES) --duration-sec $(DURATION)

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
	  YEYBOATS_HOST=localhost:$(TUNNEL_PORT) YEYBOATS_BLE_NAME=yey-d \
	    pytest tests/system/unattended --yeydisp-no-udp-discovery --yeydisp-no-discovery

proto:  ## Regenerate protocol code (C++ + TS) from the schema
	python3 proto/gen/gen_cpp.py
	cd proto/js && npm install --silent && node ../gen/gen_ts.mjs

lint: version-check  ## Check C++ formatting and Python syntax
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
	  echo "$(CLANG_FORMAT) not found. Install clang-format or run with CLANG_FORMAT=/path/to/clang-format." >&2; exit 127; }
	@find src include test \( -name '*.cpp' -o -name '*.h' \) ! -name 'build_version.h' -print | xargs $(CLANG_FORMAT) --dry-run --Werror
	@find tools -name '*.py' -print | xargs python3 -m py_compile

pre-commit: lint  ## Run the same checks used by the local hook and CI

hooks-install:  ## Install repository git hooks
	git config core.hooksPath .githooks
	@echo "git hooks installed: core.hooksPath=.githooks"

format:  ## Auto-format C++ sources in place
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
	  echo "$(CLANG_FORMAT) not found. Install clang-format or run with CLANG_FORMAT=/path/to/clang-format." >&2; exit 127; }
	@find src include test \( -name '*.cpp' -o -name '*.h' \) ! -name 'build_version.h' -print | xargs $(CLANG_FORMAT) -i

backup:  ## Dump the device flash to backup/full_flash_16MB.bin (chunked, resumable)
	@test -n "$(PORT)" || { echo "No USB serial port found. Set PORT=<path>." >&2; exit 1; }
	PORT=$(PORT) bash tools/dump_chunked.sh

release-tag:  ## (legacy) Tag a release locally. Prefer the "Cut Release" CI workflow.
	@echo "Note: prefer .github/workflows/release-cut.yml (Actions tab) - it bumps"
	@echo "      VERSION, commits, tags, and pushes from CI with no local git state."
	@test -n "$(VERSION)" || { echo "Usage: make release-tag VERSION=v0.1.0" >&2; exit 1; }
	@echo "$(VERSION)" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-[a-z0-9.]+)?$$' \
	  || { echo "Version must look like v0.1.0 or v0.1.0-rc1" >&2; exit 1; }
	@tag_mm=$$(echo $(VERSION) | sed 's/^v//' | cut -d. -f1-2); \
	  repo_mm=$$(cut -d. -f1-2 VERSION); \
	  test "$$tag_mm" = "$$repo_mm" || { \
	    echo "Tag MAJOR.MINOR ($$tag_mm) must match VERSION file ($$repo_mm); the tag's BUILD is authoritative" >&2; exit 1; }
	git tag -a $(VERSION) -m "Release $(VERSION)"
	@echo "Tag $(VERSION) created locally. Push with: git push origin $(VERSION)"

clean:  ## Remove build artifacts (keeps include/secrets.h)
	$(PIO) run -t clean 2>/dev/null || true
	rm -rf .pio

.PHONY: help setup version version-check version-set build test sim sys-test sys-test-remote \
        sys-test-mac sys-test-attended flash ota monitor ble ble-cmd provision logs \
        demo-up demo-down demo-up-remote demo-down-remote \
        server-setup server-sk-only server-status server-teardown \
        proto lint pre-commit hooks-install format backup release-tag clean
