#!/usr/bin/env bash
#
# One-time lab-host rename migration: espdisp-* -> yeydisp-* on the SignalK /
# AP server (mythra-nav). Run it ON that host as the `compulab` user (it uses
# `sudo` for the root-owned bits). Idempotent and safe to re-run.
#
# It does NOT change anything the device depends on for connectivity:
#   - WiFi SSID stays `esp-lab` / passphrase `esp-lab-2026`
#   - the static DHCP lease 28:37:2f:8a:02:90 -> 10.42.0.67 stays (it is
#     hard-coded in the AP setup script's heredoc, regenerated verbatim)
#   - the SignalK docker-compose mounts are relative (./config, ./plugins),
#     so they move with the deploy dir and the manager data is preserved
#
# What it renames:
#   - deploy dir   ~/espdisp-signalk            -> ~/yeydisp-signalk
#   - systemd      espdisp-{signalk,lab-ap,watch}.service -> yeydisp-*.service
#   - AP setup     /usr/local/sbin/espdisp-lab-ap-setup.sh -> yeydisp-*,
#                  /etc/espdisp-lab -> /etc/yeydisp-lab,
#                  /etc/NetworkManager/conf.d/99-espdisp-lab-ap.conf -> 99-yeydisp-*
#   - AP logs      /var/log/espdisp-lab-*.log   -> /var/log/yeydisp-lab-*.log
#
# Brief downtime: the esp-lab AP and the SignalK container each restart once,
# so the device drops off the network for ~10-30s and then re-registers.
#
# Usage:   scp tools/migrate-lab-rename.sh compulab@mythra-nav:/tmp/
#          ssh -t compulab@mythra-nav 'bash /tmp/migrate-lab-rename.sh'
# Dry run: bash /tmp/migrate-lab-rename.sh --dry-run

set -euo pipefail

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

run() {                       # run a privileged command (or print it in dry-run)
    if [ "$DRY" = 1 ]; then printf '  [dry] %s\n' "$*"; else eval "$@"; fi
}
say() { printf '\n==> %s\n' "$*"; }

OLD_DEPLOY="$HOME/espdisp-signalk"
NEW_DEPLOY="$HOME/yeydisp-signalk"

say "preflight"
command -v docker-compose >/dev/null 2>&1 || { echo "docker-compose not found" >&2; exit 1; }
if [ "$DRY" = 0 ]; then
    sudo -v || { echo "need sudo on this host (run from a TTY: ssh -t ...)" >&2; exit 1; }
fi

# ----------------------------------------------------------------------------
say "1/5  stop services (esp-lab AP + SignalK drop briefly)"
for u in espdisp-watch espdisp-signalk espdisp-lab-ap; do
    run "sudo systemctl stop ${u}.service 2>/dev/null || true"
done

# ----------------------------------------------------------------------------
say "2/5  SignalK deploy dir (relative compose mounts move with it)"
if [ -d "$OLD_DEPLOY" ] && [ ! -e "$NEW_DEPLOY" ]; then
    run "mv '$OLD_DEPLOY' '$NEW_DEPLOY'"
else
    echo "  deploy dir already migrated or missing — skipping"
fi

# ----------------------------------------------------------------------------
say "3/5  AP setup script + config (keeps esp-lab SSID + the 10.42.0.67 lease)"
# The script self-generates /etc/<dir>, the NM conf, the unit and the configs,
# so we rename + sed it and let it rebuild everything under yeydisp on run.
if test -f /usr/local/sbin/espdisp-lab-ap-setup.sh; then
    run "sudo mv /usr/local/sbin/espdisp-lab-ap-setup.sh /usr/local/sbin/yeydisp-lab-ap-setup.sh"
fi
if test -f /usr/local/sbin/yeydisp-lab-ap-setup.sh; then
    # s/espdisp-lab/yeydisp-lab/g rewrites the conf dir, NM conf, log names,
    # self-path and unit name. It does NOT match `esp-lab` (the SSID) or
    # `esp-lab-2026` (the passphrase), which have no `espdisp-lab` substring.
    run "sudo sed -i 's/espdisp-lab/yeydisp-lab/g' /usr/local/sbin/yeydisp-lab-ap-setup.sh"
fi
# Drop the stale espdisp copies; the renamed script regenerates the yeydisp ones.
run "sudo rm -f /etc/systemd/system/espdisp-lab-ap.service \
        /etc/NetworkManager/conf.d/99-espdisp-lab-ap.conf"
run "sudo rm -rf /etc/espdisp-lab"
# Move existing AP logs for continuity (the script writes yeydisp-lab-*.log next).
for lg in hostapd dnsmasq; do
    if test -f "/var/log/espdisp-lab-${lg}.log"; then
        run "sudo mv /var/log/espdisp-lab-${lg}.log /var/log/yeydisp-lab-${lg}.log"
    fi
done

# ----------------------------------------------------------------------------
say "4/5  systemd units: signalk + watch (rename + rewrite paths)"
migrate_unit() {                       # $1 = service basename without prefix
    local u="$1" old="/etc/systemd/system/espdisp-$1.service" new="/etc/systemd/system/yeydisp-$1.service"
    test -f "$old" || { echo "  espdisp-$u.service absent — skipping"; return; }
    run "sudo systemctl disable espdisp-$u.service 2>/dev/null || true"
    if [ "$DRY" = 1 ]; then
        printf '  [dry] sudo sed ... %s > %s ; rm %s\n' "$old" "$new" "$old"
    else
        sudo sed \
            -e "s#${OLD_DEPLOY}#${NEW_DEPLOY}#g" \
            -e 's#/tmp/espdisp-watch.sh#/tmp/yeydisp-watch.sh#g' \
            -e 's#/var/log/espdisp#/var/log/yeydisp#g' \
            -e 's#Description=espdisp#Description=yeydisp#g' \
            "$old" | sudo tee "$new" >/dev/null
        sudo rm -f "$old"
    fi
}
migrate_unit signalk
migrate_unit watch

# ----------------------------------------------------------------------------
say "5/5  reload, enable, start, verify"
run "sudo systemctl daemon-reload"
# Bring the AP up via its renamed self-installing script (also (re)writes +
# enables yeydisp-lab-ap.service). Preserves esp-lab + the static lease.
run "sudo /usr/local/sbin/yeydisp-lab-ap-setup.sh"
run "sudo systemctl enable yeydisp-signalk.service 2>/dev/null || true"
run "sudo systemctl start  yeydisp-signalk.service"
# yeydisp-watch needs /tmp/yeydisp-watch.sh (gitignored runtime drop). It is
# currently inactive (the /tmp script was cleared); redeploy with:
#   scp tools/yeydisp-watch.sh compulab@mythra-nav:/tmp/ && sudo systemctl start yeydisp-watch
run "sudo systemctl enable yeydisp-watch.service 2>/dev/null || true"

if [ "$DRY" = 1 ]; then echo; echo "dry-run complete — no changes made."; exit 0; fi

sleep 6
echo
docker ps --format '{{.Names}}' | grep -qx signalk-server \
    && echo "  OK   signalk-server container up" \
    || echo "  WARN signalk-server not up — sudo systemctl status yeydisp-signalk"
iw dev wlan-ap0 info 2>/dev/null | grep -q "ssid esp-lab" \
    && echo "  OK   esp-lab AP up on wlan-ap0" \
    || echo "  WARN esp-lab AP down — sudo systemctl status yeydisp-lab-ap"
systemctl list-unit-files 2>/dev/null | grep -qi espdisp \
    && { echo "  NOTE espdisp-* units still present:"; systemctl list-unit-files | grep -i espdisp; } \
    || echo "  OK   no espdisp-* systemd units remain"
echo
echo "Done. The device reconnects to esp-lab (keeps 10.42.0.67) and re-registers within ~30s."
echo "Verify the manager:  curl -s -o /dev/null -w '%{http_code}\\n' http://localhost:3000/signalk"
