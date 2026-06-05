#!/usr/bin/env bash
# Run on nav-server as root.  Brings up `esp-lab` AP on virtual wlan-ap0
# and a dnsmasq DHCP server for 10.42.0.0/24.  Forwarding is pure
# routed (no NAT) — the Router/Starlink/etc. WAN router needs a static route
# `10.42.0.0/24 via 192.168.2.11` for return traffic.
#
# Usage:
#   sudo bash lab-ap-setup.sh <wpa2-psk>
#   ESP_LAB_PSK=<psk> sudo -E bash lab-ap-setup.sh
#
# The PSK is required.  Source `.env.test.local` in your shell first
# to pick it up from there.  The script refuses to write hostapd.conf
# with the obvious placeholder.
set -euo pipefail
export PATH=/usr/sbin:/sbin:/usr/local/sbin:$PATH

PSK="${1:-${ESP_LAB_PSK:-changeme}}"
if [ "$PSK" = "changeme" ] || [ "${#PSK}" -lt 8 ]; then
  echo "lab-ap-setup.sh: refusing to start AP with placeholder/short PSK." >&2
  echo "Pass it as the first argument, or export ESP_LAB_PSK before running:" >&2
  echo "    sudo bash $0 'your-wpa2-psk'" >&2
  exit 64
fi

CONF_DIR=/etc/espdisp-lab
LOG_DIR=/var/log
mkdir -p "$CONF_DIR"

# --- hostapd config ---------------------------------------------------
cat > "$CONF_DIR/hostapd.conf" <<EOF
interface=wlan-ap0
driver=nl80211
ssid=esp-lab
# 2.4 GHz / channel 6 — no regulatory-db requirement, works on the
# default world regdom (country 00).  ESP32-S3 only does 2.4 GHz
# anyway, so we'd never get the 5 GHz speedup with this client.
hw_mode=g
channel=6
ieee80211n=1
wmm_enabled=1
auth_algs=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=$PSK

# --- Disconnect-reduction settings -----------------------------------
# Disable inactivity timeout so idle ESP32 clients are not kicked
# (was causing periodic deauth -> WiFi reconnect -> "constant disconnects").
ap_max_inactivity=0
# Do not disassociate on low ACK rate; RF noise should not kick a client.
disassoc_low_ack=0
# Proxy ARP: AP answers ARP requests on behalf of its clients.
# Required for the routed (no-NAT) setup so the WAN router can reach
# 10.42.0.67 without the ESP32 needing to reply to every ARP itself.
proxy_arp=1
EOF
chmod 0600 "$CONF_DIR/hostapd.conf"  # PSK lives in this file at rest

# --- dnsmasq config (DHCP only, default gw 10.42.0.1 = nav-server) -----
# Static lease for the lab MFD (MAC 28:37:2f:8a:02:90) pins its IP at
# 10.42.0.67, matching what .env.test expects.  Add more dhcp-host
# lines as new devices join.
cat > "$CONF_DIR/dnsmasq.conf" <<'EOF'
interface=wlan-ap0
bind-interfaces
except-interface=lo
no-resolv
no-hosts
port=0
dhcp-range=10.42.0.50,10.42.0.150,255.255.255.0,12h
dhcp-option=3,10.42.0.1
dhcp-option=6,192.168.2.1,8.8.8.8
dhcp-host=28:37:2f:8a:02:90,10.42.0.67,esp32s3-8A0290
log-dhcp
EOF

# --- NM stay-away --------------------------------------------------------
mkdir -p /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/99-espdisp-lab-ap.conf <<'EOF'
[keyfile]
unmanaged-devices=interface-name:wlan-ap0
EOF
systemctl reload NetworkManager
sleep 1

# --- virtual iface -------------------------------------------------------
iw dev wlan-ap0 del 2>/dev/null || true
iw phy phy0 interface add wlan-ap0 type __ap
ip link set dev wlan-ap0 up
ip addr flush dev wlan-ap0 || true
ip addr add 10.42.0.1/24 dev wlan-ap0

# --- forwarding + iptables (ROUTED, no NAT) -----------------------------
sysctl -w net.ipv4.ip_forward=1 >/dev/null
# Remove any old MASQUERADE we may have added in earlier passes
iptables -t nat -C POSTROUTING -s 10.42.0.0/24 -o eth0 -j MASQUERADE 2>/dev/null \
  && iptables -t nat -D POSTROUTING -s 10.42.0.0/24 -o eth0 -j MASQUERADE || true
# Allow forwarded traffic in both directions
iptables -C FORWARD -s 10.42.0.0/24 -j ACCEPT 2>/dev/null \
  || iptables -I FORWARD 1 -s 10.42.0.0/24 -j ACCEPT
iptables -C FORWARD -d 10.42.0.0/24 -j ACCEPT 2>/dev/null \
  || iptables -I FORWARD 1 -d 10.42.0.0/24 -j ACCEPT

# --- (re)start hostapd + dnsmasq ---------------------------------------
pkill -f "$CONF_DIR/hostapd.conf"  2>/dev/null || true
pkill -f "$CONF_DIR/dnsmasq.conf"  2>/dev/null || true
sleep 1

setsid hostapd -B -f "$LOG_DIR/espdisp-lab-hostapd.log" "$CONF_DIR/hostapd.conf"
sleep 2
setsid dnsmasq -C "$CONF_DIR/dnsmasq.conf" \
  --log-facility="$LOG_DIR/espdisp-lab-dnsmasq.log"
sleep 1

# --- self-install + systemd unit so AP survives nav-server reboot ------
SELF="$(readlink -f "$0")"
PERSIST=/usr/local/sbin/espdisp-lab-ap-setup.sh
if [ "$SELF" != "$PERSIST" ]; then
  install -m 0755 "$SELF" "$PERSIST"
fi
UNIT=/etc/systemd/system/espdisp-lab-ap.service
cat > "$UNIT" <<EOF
[Unit]
Description=esp-lab AP (hostapd + dnsmasq on wlan-ap0)
After=network-online.target NetworkManager.service
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$PERSIST
ExecStop=/bin/bash -c 'pkill -f $CONF_DIR/hostapd.conf; pkill -f $CONF_DIR/dnsmasq.conf; /usr/sbin/iw dev wlan-ap0 del 2>/dev/null || true'

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable espdisp-lab-ap.service >/dev/null 2>&1 || true
echo "installed: $PERSIST + $UNIT"

# --- status report -----------------------------------------------------
echo "=== wlan-ap0 ==="
ip -br addr | grep wlan-ap0 || true
iw dev wlan-ap0 info 2>&1 | grep -E "ssid|type|channel|txpower" || true
echo "=== running procs ==="
pgrep -af "$CONF_DIR" || echo "(no procs matched)"
echo "=== hostapd tail ==="
tail -15 "$LOG_DIR/espdisp-lab-hostapd.log" 2>&1 || true
echo "=== iptables forward ==="
iptables -S FORWARD | grep 10.42 || true
echo "=== iptables nat (should have NO 10.42 line for routed mode) ==="
iptables -t nat -S POSTROUTING | grep 10.42 || echo "(none — routed mode)"
