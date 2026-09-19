#!/bin/bash
# Cut Oracle STORE + WG DNAT from MACBOOK 10.66.0.2 to LHRSolarOfficePC 10.66.0.4.
# FPGA dest stays 150.136.85.207. 10.66.0.2 peer is left up for rollback.
#
#   CONFIRM=1 sudo ./retarget-store-officepc.sh apply
#   CONFIRM=1 sudo ./retarget-store-officepc.sh rollback
set -euo pipefail

NEW="${PHOTON_WG_PEER_OFFICE:-10.66.0.4}"
OLD="${PHOTON_WG_PEER_MACBOOK:-10.66.0.2}"
RAW="/etc/systemd/system/photon-raw-relay.service"
VID="/etc/systemd/system/photon-video-relay.service"
PIPE="/usr/local/sbin/photon-wg-pipe.sh"

need_confirm() {
  if [[ "${CONFIRM:-}" != "1" ]]; then
    echo "refusing: set CONFIRM=1 (this moves live store off MACBOOK)" >&2
    exit 2
  fi
}

ping_peer() {
  local ip="$1"
  ping -c 2 -W 2 "$ip" >/dev/null
}

apply() {
  need_confirm
  ping_peer "$NEW" || { echo "10.66.0.4 not reachable; office WG down" >&2; exit 1; }
  sed -i "s/${OLD}:6100/${NEW}:6100/" "$RAW"
  sed -i "s/${OLD}:6200/${NEW}:6200/" "$VID"
  systemctl daemon-reload
  PHOTON_WG_PEER="$NEW" "$PIPE" apply
  systemctl restart photon-raw-relay photon-video-relay
  echo "store now $NEW (MACBOOK $OLD still a WG peer)"
  "$PIPE" status
}

rollback() {
  need_confirm
  sed -i "s/${NEW}:6100/${OLD}:6100/" "$RAW"
  sed -i "s/${NEW}:6200/${OLD}:6200/" "$VID"
  systemctl daemon-reload
  PHOTON_WG_PEER="$OLD" "$PIPE" apply
  systemctl restart photon-raw-relay photon-video-relay
  echo "store rolled back to $OLD"
  "$PIPE" status
}

cmd="${1:-}"
case "$cmd" in
  apply|rollback) "$cmd" ;;
  *) echo "usage: CONFIRM=1 $0 apply|rollback" >&2; exit 2 ;;
esac
