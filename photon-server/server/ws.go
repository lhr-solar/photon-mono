package main

// A small WebSocket (RFC 6455) implementation for protocol v2: the server
// handshake, a client dial for tests and tools, whole-message reads and writes,
// and the close handshake. No extensions (no compression) and no subprotocols.

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"
)

// wsMessageType is the type of a data message.
type wsMessageType int

const (
	wsText   wsMessageType = 1
	wsBinary wsMessageType = 2
)

// wsStatusCode is a close status (RFC 6455 §7.4).
type wsStatusCode int

const (
	wsStatusNormalClosure   wsStatusCode = 1000
	wsStatusGoingAway       wsStatusCode = 1001
	wsStatusProtocolError   wsStatusCode = 1002
	wsStatusNoStatusRcvd    wsStatusCode = 1005 // never sent on the wire
	wsStatusInvalidPayload  wsStatusCode = 1007
	wsStatusPolicyViolation wsStatusCode = 1008
	wsStatusMessageTooBig   wsStatusCode = 1009
)

const (
	opContinuation = 0x0
	opText         = 0x1
	opBinary       = 0x2
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xA

	wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
	// wsDefaultReadLimit is the largest message a connection reads unless
	// SetReadLimit says otherwise.
	wsDefaultReadLimit = 1 << 20
	// wsControlTimeout bounds writing a pong or close frame.
	wsControlTimeout = 5 * time.Second
	// wsCloseWait is how long Close waits for the peer to answer a close frame.
	wsCloseWait = 2 * time.Second
)

// wsCloseError is the close frame the peer sent.
type wsCloseError struct {
	Code   wsStatusCode
	Reason string
}

func (e *wsCloseError) Error() string {
	return fmt.Sprintf("websocket closed by peer: status %d %q", e.Code, e.Reason)
}

// wsCloseStatus is the status of the close frame that ended a read, or -1 when
// err is not one.
func wsCloseStatus(err error) wsStatusCode {
	var ce *wsCloseError
	if errors.As(err, &ce) {
		return ce.Code
	}
	return -1
}

var errWSClosed = errors.New("websocket: connection closed")

// wsConn is one WebSocket connection. One goroutine may read while others
// write; writes are serialized. Close and CloseNow are safe from any goroutine.
type wsConn struct {
	conn   net.Conn
	br     *bufio.Reader
	server bool // frames from the peer must be masked, ours must not

	rmu   sync.Mutex // held by the reader
	limit int64
	rerr  error // sticky: once a read fails, the stream is not readable again

	wmu       sync.Mutex
	closeSent bool

	closeAt atomic.Int64 // Unix ns by which the peer must answer our close; 0 until Close

	closeOnce sync.Once
}

func newWSConn(conn net.Conn, br *bufio.Reader, server bool) *wsConn {
	return &wsConn{conn: conn, br: br, server: server, limit: wsDefaultReadLimit}
}

// wsAccept upgrades an HTTP request to a WebSocket. When the request carries an
// Origin, its host must be the request's Host (same-origin pages); native
// clients send none. On failure it has already answered the request.
func wsAccept(w http.ResponseWriter, r *http.Request) (*wsConn, error) {
	if r.Method != http.MethodGet {
		http.Error(w, "websocket upgrade needs GET", http.StatusMethodNotAllowed)
		return nil, errors.New("websocket: method is not GET")
	}
	if !headerHasToken(r.Header, "Connection", "upgrade") || !headerHasToken(r.Header, "Upgrade", "websocket") {
		w.Header().Set("Upgrade", "websocket")
		http.Error(w, "websocket upgrade required", http.StatusUpgradeRequired)
		return nil, errors.New("websocket: not an upgrade request")
	}
	if r.Header.Get("Sec-WebSocket-Version") != "13" {
		w.Header().Set("Sec-WebSocket-Version", "13")
		http.Error(w, "websocket version 13 required", http.StatusUpgradeRequired)
		return nil, errors.New("websocket: version is not 13")
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if raw, err := base64.StdEncoding.DecodeString(key); err != nil || len(raw) != 16 {
		http.Error(w, "bad Sec-WebSocket-Key", http.StatusBadRequest)
		return nil, errors.New("websocket: bad key")
	}
	if origin := r.Header.Get("Origin"); origin != "" {
		u, err := url.Parse(origin)
		if err != nil || !strings.EqualFold(u.Host, r.Host) {
			http.Error(w, "cross-origin websocket refused", http.StatusForbidden)
			return nil, fmt.Errorf("websocket: origin %q is not host %q", origin, r.Host)
		}
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "websocket not supported here", http.StatusInternalServerError)
		return nil, errors.New("websocket: response cannot be hijacked")
	}
	conn, brw, err := hj.Hijack()
	if err != nil {
		return nil, err
	}
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n"
	_ = conn.SetWriteDeadline(time.Now().Add(wsControlTimeout))
	if _, err := io.WriteString(conn, resp); err != nil {
		conn.Close()
		return nil, err
	}
	_ = conn.SetWriteDeadline(time.Time{})
	return newWSConn(conn, brw.Reader, true), nil
}

// wsDial opens a client WebSocket to a ws:// URL with extra request headers.
// On a refused upgrade it returns the HTTP response along with the error.
func wsDial(ctx context.Context, rawURL string, hdr http.Header) (*wsConn, *http.Response, error) {
	u, err := url.Parse(rawURL)
	if err != nil {
		return nil, nil, err
	}
	if u.Scheme != "ws" {
		return nil, nil, fmt.Errorf("websocket: dial supports ws:// only, not %q", u.Scheme)
	}
	host := u.Host
	if u.Port() == "" {
		host = net.JoinHostPort(u.Hostname(), "80")
	}
	var d net.Dialer
	conn, err := d.DialContext(ctx, "tcp", host)
	if err != nil {
		return nil, nil, err
	}
	fail := func(resp *http.Response, err error) (*wsConn, *http.Response, error) {
		conn.Close()
		return nil, resp, err
	}
	if dl, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(dl)
	}

	var nonce [16]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return fail(nil, err)
	}
	key := base64.StdEncoding.EncodeToString(nonce[:])
	req, err := http.NewRequest(http.MethodGet, "http://"+u.Host+u.RequestURI(), nil)
	if err != nil {
		return fail(nil, err)
	}
	for k, vs := range hdr {
		req.Header[k] = append([]string(nil), vs...)
	}
	req.Header.Set("Upgrade", "websocket")
	req.Header.Set("Connection", "Upgrade")
	req.Header.Set("Sec-WebSocket-Key", key)
	req.Header.Set("Sec-WebSocket-Version", "13")
	if err := req.Write(conn); err != nil {
		return fail(nil, err)
	}
	br := bufio.NewReader(conn)
	resp, err := http.ReadResponse(br, req)
	if err != nil {
		return fail(nil, err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		return fail(resp, fmt.Errorf("websocket: upgrade refused: %s", resp.Status))
	}
	if !headerHasToken(resp.Header, "Upgrade", "websocket") || !headerHasToken(resp.Header, "Connection", "upgrade") ||
		resp.Header.Get("Sec-WebSocket-Accept") != wsAcceptKey(key) {
		return fail(resp, errors.New("websocket: bad upgrade response"))
	}
	_ = conn.SetDeadline(time.Time{})
	return newWSConn(conn, br, false), resp, nil
}

// SetReadLimit sets the largest message Read accepts. A bigger one closes the
// connection with status 1009.
func (c *wsConn) SetReadLimit(n int64) {
	c.rmu.Lock()
	c.limit = n
	c.rmu.Unlock()
}

// Read returns the next data message. Pings are answered and pongs skipped on
// the way. ctx's deadline and cancellation end the wait; after any error the
// connection cannot be read again, and the caller should close it. A close
// frame from the peer is answered and returned as a *wsCloseError.
func (c *wsConn) Read(ctx context.Context) (wsMessageType, []byte, error) {
	c.rmu.Lock()
	defer c.rmu.Unlock()
	stop := watchDeadline(ctx, c.conn.SetReadDeadline)
	if at := c.closeAt.Load(); at != 0 {
		// Close is waiting for the peer; a new Read must not wait longer.
		_ = c.conn.SetReadDeadline(time.Unix(0, at))
	}
	typ, p, err := c.readMessage()
	stop()
	if err != nil && ctx.Err() != nil && wsCloseStatus(err) < 0 {
		err = ctx.Err()
	}
	return typ, p, err
}

// Write sends one message as a single frame. ctx's deadline and cancellation
// bound the write.
func (c *wsConn) Write(ctx context.Context, typ wsMessageType, p []byte) error {
	if typ != wsText && typ != wsBinary {
		return fmt.Errorf("websocket: bad message type %d", typ)
	}
	return c.writeFrame(ctx, byte(typ), p, false)
}

// Close starts the close handshake: it sends a close frame with code and
// reason and gives the peer wsCloseWait to answer. When no Read is running it
// waits for the answer itself and then closes the connection. When a Read is
// running, that Read returns once the peer answers or the wait is over, and
// the reader's owner must call CloseNow.
func (c *wsConn) Close(code wsStatusCode, reason string) error {
	err := c.sendClose(code, reason)
	at := time.Now().Add(wsCloseWait)
	c.closeAt.CompareAndSwap(0, at.UnixNano())
	_ = c.conn.SetReadDeadline(time.Unix(0, c.closeAt.Load()))
	if c.rmu.TryLock() {
		for c.rerr == nil {
			_, _, _ = c.readMessage()
		}
		c.rmu.Unlock()
		if cerr := c.CloseNow(); err == nil {
			err = cerr
		}
	}
	return err
}

// CloseNow closes the connection without a handshake.
func (c *wsConn) CloseNow() error {
	var err error
	c.closeOnce.Do(func() { err = c.conn.Close() })
	return err
}

// readMessage reads frames until a whole data message, answering control
// frames on the way. c.rmu must be held.
func (c *wsConn) readMessage() (wsMessageType, []byte, error) {
	if c.rerr != nil {
		return 0, nil, c.rerr
	}
	var (
		msgOp byte
		msg   []byte
	)
	for {
		fin, op, payload, err := c.readFrame(int64(len(msg)))
		if err != nil {
			c.rerr = err
			return 0, nil, err
		}
		switch op {
		case opPing:
			ctx, cancel := context.WithTimeout(context.Background(), wsControlTimeout)
			_ = c.writeFrame(ctx, opPong, payload, true)
			cancel()
			continue
		case opPong:
			continue
		case opClose:
			return 0, nil, c.peerClosed(payload)
		case opText, opBinary:
			if msgOp != 0 {
				return 0, nil, c.fail(wsStatusProtocolError, "new message inside a fragmented one")
			}
			msgOp, msg = op, payload
		case opContinuation:
			if msgOp == 0 {
				return 0, nil, c.fail(wsStatusProtocolError, "continuation without a message")
			}
			msg = append(msg, payload...)
		default:
			return 0, nil, c.fail(wsStatusProtocolError, fmt.Sprintf("unknown opcode %#x", op))
		}
		if !fin {
			continue
		}
		if msgOp == opText && !utf8.Valid(msg) {
			return 0, nil, c.fail(wsStatusInvalidPayload, "text message is not UTF-8")
		}
		return wsMessageType(msgOp), msg, nil
	}
}

// readFrame reads one frame. have is the size of the message assembled so far,
// so the read limit covers the whole message.
func (c *wsConn) readFrame(have int64) (fin bool, op byte, payload []byte, err error) {
	var h [2]byte
	if _, err = io.ReadFull(c.br, h[:]); err != nil {
		return
	}
	fin = h[0]&0x80 != 0
	op = h[0] & 0x0F
	if h[0]&0x70 != 0 {
		return false, 0, nil, c.fail(wsStatusProtocolError, "reserved bits set")
	}
	if masked := h[1]&0x80 != 0; masked != c.server {
		return false, 0, nil, c.fail(wsStatusProtocolError, "wrong masking for this direction")
	}
	n := uint64(h[1] & 0x7F)
	switch n {
	case 126:
		var b [2]byte
		if _, err = io.ReadFull(c.br, b[:]); err != nil {
			return
		}
		n = uint64(binary.BigEndian.Uint16(b[:]))
	case 127:
		var b [8]byte
		if _, err = io.ReadFull(c.br, b[:]); err != nil {
			return
		}
		n = binary.BigEndian.Uint64(b[:])
		if n>>63 != 0 {
			return false, 0, nil, c.fail(wsStatusProtocolError, "frame length too large")
		}
	}
	if op >= opClose {
		if !fin || n > 125 {
			return false, 0, nil, c.fail(wsStatusProtocolError, "bad control frame")
		}
	} else if n > uint64(c.limit) || have+int64(n) > c.limit {
		return false, 0, nil, c.fail(wsStatusMessageTooBig, "message too big")
	}
	var key [4]byte
	if c.server {
		if _, err = io.ReadFull(c.br, key[:]); err != nil {
			return
		}
	}
	payload = make([]byte, n)
	if _, err = io.ReadFull(c.br, payload); err != nil {
		return
	}
	if c.server {
		for i := range payload {
			payload[i] ^= key[i&3]
		}
	}
	return fin, op, payload, nil
}

// peerClosed handles the peer's close frame: it answers with the same status
// and returns it as the read error.
func (c *wsConn) peerClosed(payload []byte) error {
	ce := &wsCloseError{Code: wsStatusNoStatusRcvd}
	switch {
	case len(payload) == 1:
		return c.fail(wsStatusProtocolError, "bad close frame")
	case len(payload) >= 2:
		ce.Code = wsStatusCode(binary.BigEndian.Uint16(payload))
		ce.Reason = string(payload[2:])
	}
	_ = c.sendClose(ce.Code, "")
	c.rerr = ce
	return ce
}

// fail closes the connection for a protocol error found while reading.
func (c *wsConn) fail(code wsStatusCode, reason string) error {
	_ = c.sendClose(code, reason)
	c.rerr = fmt.Errorf("websocket: %s", reason)
	return c.rerr
}

// sendClose sends a close frame once; later calls do nothing.
func (c *wsConn) sendClose(code wsStatusCode, reason string) error {
	var p []byte
	if code != wsStatusNoStatusRcvd {
		if len(reason) > 123 {
			reason = reason[:123]
		}
		p = binary.BigEndian.AppendUint16(nil, uint16(code))
		p = append(p, reason...)
	}
	ctx, cancel := context.WithTimeout(context.Background(), wsControlTimeout)
	defer cancel()
	return c.writeFrame(ctx, opClose, p, true)
}

// writeFrame writes one final frame. After a close frame has gone out, only
// control frames other than a second close may follow.
func (c *wsConn) writeFrame(ctx context.Context, op byte, p []byte, control bool) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.closeSent && (!control || op == opClose) {
		if op == opClose {
			return nil
		}
		return errWSClosed
	}
	stop := watchDeadline(ctx, c.conn.SetWriteDeadline)
	defer stop()

	hdr := make([]byte, 0, 14)
	hdr = append(hdr, 0x80|op)
	mask := byte(0)
	if !c.server {
		mask = 0x80
	}
	switch n := len(p); {
	case n <= 125:
		hdr = append(hdr, mask|byte(n))
	case n <= 0xFFFF:
		hdr = append(hdr, mask|126)
		hdr = binary.BigEndian.AppendUint16(hdr, uint16(n))
	default:
		hdr = append(hdr, mask|127)
		hdr = binary.BigEndian.AppendUint64(hdr, uint64(n))
	}
	body := p
	if !c.server {
		var key [4]byte
		if _, err := rand.Read(key[:]); err != nil {
			return err
		}
		hdr = append(hdr, key[:]...)
		body = make([]byte, len(p))
		for i := range p {
			body[i] = p[i] ^ key[i&3]
		}
	}
	bufs := net.Buffers{hdr, body}
	_, err := bufs.WriteTo(c.conn)
	if op == opClose {
		c.closeSent = true
	}
	return err
}

// watchDeadline applies ctx's deadline with set, and moves it into the past if
// ctx is cancelled first. The returned stop ends the watch.
func watchDeadline(ctx context.Context, set func(time.Time) error) (stop func()) {
	if dl, ok := ctx.Deadline(); ok {
		_ = set(dl)
	} else {
		_ = set(time.Time{})
	}
	cancel := context.AfterFunc(ctx, func() { _ = set(time.Unix(1, 0)) })
	return func() { cancel() }
}

// wsAcceptKey is the Sec-WebSocket-Accept value for a client key.
func wsAcceptKey(key string) string {
	sum := sha1.Sum([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

// headerHasToken reports whether a comma-separated header holds token,
// ignoring case.
func headerHasToken(h http.Header, name, token string) bool {
	for _, v := range h.Values(name) {
		for _, t := range strings.Split(v, ",") {
			if strings.EqualFold(strings.TrimSpace(t), token) {
				return true
			}
		}
	}
	return false
}
