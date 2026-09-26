#!/bin/bash
# Oracle public NIC -> MACBOOK WireGuard 10.66.0.2
# Replaces ssh -R for video/fan-out/observer. UDP :6200 stays UDP
# (no udp-rtp-to-tcp hop). UDP :6200/:6800 are photon-video-relay
# (RTP tee + PVU1 live). Loopback UI :8080 and photon-sshd :2222 stay SSH.
#
# TCP :6100 is NOT DNATed. photon-raw-relay terminates the car on Oracle and
# copies raw CANP to 10.66.0.2:6100 (store) plus PHAU clients. Clients decode
# without a MacBook round trip.
set -euo pipefail

PEER="${PHOTON_WG_PEER:-10.66.0.2}"
IFACE="${PHOTON_PUBLIC_IFACE:-ens3}"
WG="${PHOTON_WG_IFACE:-wg0}"
TCP_PORTS=(6200 6500 6700 5400)
UDP_PORTS=()

usage() {
  echo "usage: $0 apply|remove|status" >&2
  exit 2
}

enable_forward() {
  sysctl -w net.ipv4.ip_forward=1 >/dev/null
  sysctl -w net.ipv4.conf.all.forwarding=1 >/dev/null
  sysctl -w "net.ipv4.conf.${IFACE}.forwarding=1" >/dev/null
  sysctl -w "net.ipv4.conf.${WG}.forwarding=1" >/dev/null
  # Match MACBOOK WireGuard MTU. Jumbo 8920 on Oracle plus 1472-byte RTP
  # made outer WG packets miss the internet 1500 path and Live buffered.
  ip link set "$WG" mtu 1420 2>/dev/null || true
  cat >/etc/sysctl.d/99-photon-wg-pipe.conf <<'EOF'
net.ipv4.ip_forward=1
net.ipv4.conf.all.forwarding=1
EOF
}

nat_has() {
  iptables -t nat -C "$@" 2>/dev/null
}

filter_has() {
  iptables -C "$@" 2>/dev/null
}

nat_add() {
  nat_has "$@" || iptables -t nat -A "$@"
}

filter_add() {
  filter_has "$@" || iptables -A "$@"
}

nat_del() {
  while nat_has "$@"; do
    iptables -t nat -D "$@" || break
  done
}

apply() {
  enable_forward
  filter_add FORWARD -i "$WG" -j ACCEPT
  filter_add FORWARD -o "$WG" -j ACCEPT
  nat_add POSTROUTING -o "$WG" -j MASQUERADE
  # Raw relay owns public TCP :6100 / :6110. Video relay owns UDP :6200 / :6800.
  # Drop leftover DNAT so PREROUTING does not steal the car before userspace.
  nat_del PREROUTING -i "$IFACE" -p tcp --dport 6100 -j DNAT --to-destination "${PEER}:6100"
  nat_del PREROUTING -i "$IFACE" -p tcp --dport 6110 -j DNAT --to-destination "${PEER}:6110"
  nat_del PREROUTING -i "$IFACE" -p udp --dport 6200 -j DNAT --to-destination "${PEER}:6200"
  nat_del PREROUTING -i "$IFACE" -p udp --dport 6800 -j DNAT --to-destination "${PEER}:6800"
  local p
  for p in "${TCP_PORTS[@]}"; do
    nat_add PREROUTING -i "$IFACE" -p tcp --dport "$p" -j DNAT --to-destination "${PEER}:${p}"
  done
  for p in "${UDP_PORTS[@]}"; do
    nat_add PREROUTING -i "$IFACE" -p udp --dport "$p" -j DNAT --to-destination "${PEER}:${p}"
  done
  if command -v netfilter-persistent >/dev/null 2>&1; then
    netfilter-persistent save >/dev/null
  fi
  echo "photon-wg-pipe applied peer=${PEER} iface=${IFACE}"
}

remove() {
  local p
  nat_del PREROUTING -i "$IFACE" -p tcp --dport 6100 -j DNAT --to-destination "${PEER}:6100"
  nat_del PREROUTING -i "$IFACE" -p tcp --dport 6110 -j DNAT --to-destination "${PEER}:6110"
  nat_del PREROUTING -i "$IFACE" -p udp --dport 6200 -j DNAT --to-destination "${PEER}:6200"
  nat_del PREROUTING -i "$IFACE" -p udp --dport 6800 -j DNAT --to-destination "${PEER}:6800"
  for p in "${TCP_PORTS[@]}"; do
    nat_del PREROUTING -i "$IFACE" -p tcp --dport "$p" -j DNAT --to-destination "${PEER}:${p}"
  done
  for p in "${UDP_PORTS[@]}"; do
    nat_del PREROUTING -i "$IFACE" -p udp --dport "$p" -j DNAT --to-destination "${PEER}:${p}"
  done
  nat_del POSTROUTING -o "$WG" -j MASQUERADE
  if command -v netfilter-persistent >/dev/null 2>&1; then
    netfilter-persistent save >/dev/null
  fi
  echo "photon-wg-pipe removed"
}

status() {
  echo "ip_forward=$(sysctl -n net.ipv4.ip_forward)"
  echo "peer ${PEER} via ${IFACE} -> ${WG}"
  iptables -t nat -S PREROUTING
  iptables -t nat -S POSTROUTING
}

cmd="${1:-}"
case "$cmd" in
  apply|remove|status) "$cmd" ;;
  *) usage ;;
esac
