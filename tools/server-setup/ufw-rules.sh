#!/usr/bin/env bash
# Apply ufw firewall rules for the espdisp nav-server.
# Run once after `ufw enable`.  Idempotent: re-running applies cleanly.
#
# Network topology:
#   eth0       192.168.2.0/24   — LAN (dev laptops, WAN router)
#   wlan-ap0   10.42.0.0/24    — yey-net AP (ESP32 devices)
#
# Policy: deny all inbound by default; allow only what the lab needs.

set -euo pipefail

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    echo "ufw-rules.sh must run as root" >&2
    exit 1
fi

command -v ufw >/dev/null 2>&1 || { echo "ufw not installed (apt install ufw)"; exit 1; }

LAB_LAN="192.168.2.0/24"
ESP_LAN="10.42.0.0/24"

ufw --force reset
ufw default deny incoming
ufw default allow outgoing

# --- SSH (LAN only) ------------------------------------------------------
ufw allow in on eth0 from "$LAB_LAN" to any port 22 proto tcp comment 'SSH from LAN'

# --- SignalK HTTP + WebSocket (LAN + ESP subnet) -------------------------
ufw allow in on eth0   from "$LAB_LAN" to any port 3000 proto tcp comment 'SK HTTP/WS LAN'
ufw allow in on eth0   from "$ESP_LAN" to any port 3000 proto tcp comment 'SK HTTP/WS ESP'
ufw allow in on wlan-ap0 to any port 3000 proto tcp comment 'SK HTTP/WS wlan-ap0'

# --- NMEA 0183 over TCP (used by SignalK NMEA forwarder) -----------------
ufw allow in on eth0   from "$LAB_LAN" to any port 10110 proto tcp comment 'NMEA0183 TCP LAN'
ufw allow in on wlan-ap0 to any port 10110 proto tcp comment 'NMEA0183 TCP wlan-ap0'

# --- UDP broadcast / discovery (ESP32 <-> SK manager plugin) -------------
# 34300: SK discovery responder (device asks "who has SK?")
# 34301: device announcement listener (device says "I'm here")
ufw allow in on wlan-ap0 to any port 34300 proto udp comment 'SK discovery resp'
ufw allow in on wlan-ap0 to any port 34301 proto udp comment 'SK device announce'

# --- UDP log listener (lab-logger) on port 9999 --------------------------
ufw allow in on wlan-ap0 to any port 9999 proto udp comment 'espdisp UDP log'

# --- Forwarding: ESP subnet <-> LAN/WAN ----------------------------------
# ufw's FORWARD policy must be ACCEPT for the routed setup to work.
# This is separate from the INPUT chain above.
sed -i 's/^DEFAULT_FORWARD_POLICY="DROP"/DEFAULT_FORWARD_POLICY="ACCEPT"/' \
    /etc/default/ufw 2>/dev/null || true
# Also patch /etc/ufw/sysctl.conf to enable IP forwarding persistently.
grep -q 'net/ipv4/ip_forward' /etc/ufw/sysctl.conf && \
    sed -i 's|.*net/ipv4/ip_forward.*|net/ipv4/ip_forward=1|' /etc/ufw/sysctl.conf || \
    echo 'net/ipv4/ip_forward=1' >> /etc/ufw/sysctl.conf

ufw --force enable
ufw status verbose
