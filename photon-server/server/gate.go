package main

import (
	"crypto/rand"
	"crypto/subtle"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"strings"
	"sync"
	"time"
)

const (
	authMagic             = "PHAU"
	authPasswordMax       = 64
	authReadTimeout       = 2 * time.Second
	maxClientConns        = 16
	maxClientConnsPerIP   = 6
	maxAcceptsPerSec      = 8
	maxAcceptsPerIPPerSec = 3
	maxAuthFailsPerMin    = 8
	authFailBlock         = 30 * time.Second
)

var (
	errAuthRejected = errors.New("auth rejected")
	errClientBusy   = errors.New("too many clients")
)

// ClientGate is the client-facing door: password first, then a slot.
// Identity is the TCP peer IP. MAC is not used — it is trivial to spoof
// and would let someone mint unlimited "new" clients.
type ClientGate struct {
	want []byte
	mu   sync.Mutex
	open int
	per  map[string]int
	hits []time.Time
	ips  map[string]*ipGate
}

type ipGate struct {
	hits    []time.Time
	fails   []time.Time
	blocked time.Time
	lastLog time.Time
}

// Report a bounded diagnostic without exposing credentials. Previously every
// rejection looked like a transport EOF to the replay backend.
func (g *ClientGate) logRejectLocked(ip string, st *ipGate, now time.Time, reason string) {
	if now.Sub(st.lastLog) < time.Second {
		return
	}
	st.lastLog = now
	log.Printf("[photon-server] client gate rejected ip=%s reason=%s open=%d ip_open=%d accepts=%d ip_accepts=%d", ip, reason, g.open, g.per[ip], len(g.hits), len(st.hits))
}

func NewClientGate(password string) *ClientGate {
	return &ClientGate{
		want: []byte(password),
		per:  make(map[string]int),
		ips:  make(map[string]*ipGate),
	}
}

// EnsurePassword returns a pinned secret, or a random 10-character one.
func EnsurePassword(password string) (string, bool) {
	p := strings.TrimSpace(password)
	if p != "" {
		if len(p) > authPasswordMax {
			p = p[:authPasswordMax]
		}
		return p, false
	}
	const alphabet = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789"
	raw := make([]byte, 10)
	if _, err := rand.Read(raw); err != nil {
		return "photon", true
	}
	out := make([]byte, 10)
	for i, b := range raw {
		out[i] = alphabet[int(b)%len(alphabet)]
	}
	return string(out), true
}

func peerIP(addr net.Addr) string {
	if addr == nil {
		return ""
	}
	host, _, err := net.SplitHostPort(addr.String())
	if err != nil {
		return addr.String()
	}
	return host
}

func isLoopbackIP(ip string) bool {
	parsed := net.ParseIP(ip)
	return parsed != nil && parsed.IsLoopback()
}

// Oracle DNAT+MASQUERADE makes every public client look like 10.66.0.1.
// Same per-IP skip as loopback: the hop is trusted, the global slot cap still applies.
func isTrustedHopIP(ip string) bool {
	if isLoopbackIP(ip) {
		return true
	}
	parsed := net.ParseIP(ip)
	if parsed == nil {
		return false
	}
	_, wgNet, err := net.ParseCIDR("10.66.0.0/24")
	if err != nil {
		return false
	}
	return wgNet.Contains(parsed)
}

func (g *ClientGate) Admit(conn net.Conn) error {
	if g == nil {
		return nil
	}
	ip := peerIP(conn.RemoteAddr())
	now := time.Now()
	g.mu.Lock()
	defer g.mu.Unlock()
	st := g.ipLocked(ip)
	if now.Before(st.blocked) {
		g.logRejectLocked(ip, st, now, "authentication cooldown")
		return errClientBusy
	}
	g.hits = trimSince(g.hits, now.Add(-time.Second))
	st.hits = trimSince(st.hits, now.Add(-time.Second))
	if len(g.hits) >= maxAcceptsPerSec || (!isTrustedHopIP(ip) && len(st.hits) >= maxAcceptsPerIPPerSec) {
		g.logRejectLocked(ip, st, now, "connection rate")
		return errClientBusy
	}
	if g.open >= maxClientConns {
		g.logRejectLocked(ip, st, now, "connection slots")
		return errClientBusy
	}
	// SSH -R (127.0.0.1) and Oracle WG MASQUERADE (10.66.0.1) are one hop,
	// not distinct clients. The per-IP cap is for real remote IPs.
	if !isTrustedHopIP(ip) && g.per[ip] >= maxClientConnsPerIP {
		g.logRejectLocked(ip, st, now, "connection slots")
		return errClientBusy
	}
	g.hits = append(g.hits, now)
	st.hits = append(st.hits, now)
	g.open++
	g.per[ip]++
	return nil
}

func (g *ClientGate) Release(conn net.Conn) {
	if g == nil || conn == nil {
		return
	}
	ip := peerIP(conn.RemoteAddr())
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.open > 0 {
		g.open--
	}
	if n := g.per[ip]; n <= 1 {
		delete(g.per, ip)
	} else {
		g.per[ip] = n - 1
	}
}

func (g *ClientGate) Auth(conn net.Conn) error {
	if g == nil {
		return nil
	}
	_ = conn.SetReadDeadline(time.Now().Add(authReadTimeout))
	got, err := ReadClientAuth(conn)
	_ = conn.SetReadDeadline(time.Time{})
	ip := peerIP(conn.RemoteAddr())
	if err != nil || subtle.ConstantTimeCompare(g.want, []byte(got)) != 1 {
		g.noteFail(ip)
		if err == nil {
			err = errAuthRejected
		}
		return err
	}
	return nil
}

func (g *ClientGate) noteFail(ip string) {
	now := time.Now()
	g.mu.Lock()
	defer g.mu.Unlock()
	st := g.ipLocked(ip)
	st.fails = trimSince(st.fails, now.Add(-time.Minute))
	g.logRejectLocked(ip, st, now, "authentication failed")
	st.fails = append(st.fails, now)
	if len(st.fails) >= maxAuthFailsPerMin {
		st.blocked = now.Add(authFailBlock)
	}
}

func (g *ClientGate) ipLocked(ip string) *ipGate {
	st := g.ips[ip]
	if st == nil {
		st = &ipGate{}
		g.ips[ip] = st
	}
	return st
}

func trimSince(in []time.Time, since time.Time) []time.Time {
	i := 0
	for i < len(in) && in[i].Before(since) {
		i++
	}
	if i == 0 {
		return in
	}
	return append([]time.Time(nil), in[i:]...)
}

func WriteClientAuth(w io.Writer, password string) error {
	if password == "" || len(password) > authPasswordMax {
		return fmt.Errorf("invalid password")
	}
	buf := make([]byte, 5+len(password))
	copy(buf[:4], authMagic)
	buf[4] = byte(len(password))
	copy(buf[5:], password)
	_, err := w.Write(buf)
	return err
}

func ReadClientAuth(r io.Reader) (string, error) {
	hdr := make([]byte, 5)
	if _, err := io.ReadFull(r, hdr); err != nil {
		return "", err
	}
	if string(hdr[:4]) != authMagic {
		return "", errAuthRejected
	}
	n := int(hdr[4])
	if n < 1 || n > authPasswordMax {
		return "", errAuthRejected
	}
	body := make([]byte, n)
	if _, err := io.ReadFull(r, body); err != nil {
		return "", err
	}
	return string(body), nil
}

func admitAndAuth(gate *ClientGate, conn net.Conn) bool {
	if gate == nil {
		return true
	}
	if err := gate.Admit(conn); err != nil {
		return false
	}
	if err := gate.Auth(conn); err != nil {
		gate.Release(conn)
		return false
	}
	return true
}
