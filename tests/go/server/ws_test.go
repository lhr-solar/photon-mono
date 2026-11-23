package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// wsEcho is a server that echoes every message and records how the read ended.
func wsEcho(t *testing.T, limit int64) (url string, ended chan error) {
	t.Helper()
	ended = make(chan error, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := wsAccept(w, r)
		if err != nil {
			return
		}
		defer c.CloseNow()
		if limit > 0 {
			c.SetReadLimit(limit)
		}
		for {
			typ, p, err := c.Read(context.Background())
			if err != nil {
				ended <- err
				return
			}
			if err := c.Write(context.Background(), typ, p); err != nil {
				ended <- err
				return
			}
		}
	}))
	t.Cleanup(srv.Close)
	return "ws" + strings.TrimPrefix(srv.URL, "http"), ended
}

func dialWS(t *testing.T, url string) *wsConn {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	c, _, err := wsDial(ctx, url, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { _ = c.CloseNow() })
	return c
}

// rawFrame writes one frame on c's socket as a client, with the given first
// byte (FIN, RSV, opcode) and masking.
func rawFrame(t *testing.T, c *wsConn, b0 byte, masked bool, payload []byte) {
	t.Helper()
	hdr := []byte{b0}
	m := byte(0)
	if masked {
		m = 0x80
	}
	switch n := len(payload); {
	case n <= 125:
		hdr = append(hdr, m|byte(n))
	default:
		hdr = append(hdr, m|126)
		hdr = binary.BigEndian.AppendUint16(hdr, uint16(n))
	}
	body := append([]byte(nil), payload...)
	if masked {
		key := [4]byte{1, 2, 3, 4}
		hdr = append(hdr, key[:]...)
		for i := range body {
			body[i] ^= key[i&3]
		}
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if _, err := c.conn.Write(append(hdr, body...)); err != nil {
		t.Fatalf("raw write: %v", err)
	}
}

func readWS(t *testing.T, c *wsConn) (wsMessageType, []byte, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return c.Read(ctx)
}

func TestWSEchoTextAndBinary(t *testing.T) {
	url, _ := wsEcho(t, 0)
	c := dialWS(t, url)
	big := strings.Repeat("x", 70_000) // needs the 64-bit length form
	for _, m := range []struct {
		typ wsMessageType
		p   string
	}{{wsText, "hello"}, {wsBinary, "\x00\x01\x02"}, {wsText, big}} {
		if err := c.Write(context.Background(), m.typ, []byte(m.p)); err != nil {
			t.Fatal(err)
		}
		typ, p, err := readWS(t, c)
		if err != nil || typ != m.typ || string(p) != m.p {
			t.Fatalf("echo: typ=%d len=%d err=%v, want typ=%d len=%d", typ, len(p), err, m.typ, len(m.p))
		}
	}
}

// A message in three fragments, with a ping between them, comes back whole,
// and the ping is answered with a pong carrying its payload.
func TestWSFragmentsAndPingInBetween(t *testing.T) {
	url, _ := wsEcho(t, 0)
	c := dialWS(t, url)
	rawFrame(t, c, opText, true, []byte("frag"))
	rawFrame(t, c, 0x80|opPing, true, []byte("p1"))
	rawFrame(t, c, opContinuation, true, []byte("men"))
	rawFrame(t, c, 0x80|opContinuation, true, []byte("ted"))

	// The pong comes first; read it raw, before the echoed message.
	_ = c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	var h [2]byte
	if _, err := c.br.Read(h[:]); err != nil || h[0] != 0x80|opPong || int(h[1]) != 2 {
		t.Fatalf("pong header %x, err %v", h, err)
	}
	var p [2]byte
	if _, err := c.br.Read(p[:]); err != nil || string(p[:]) != "p1" {
		t.Fatalf("pong payload %q, err %v", p, err)
	}
	typ, msg, err := readWS(t, c)
	if err != nil || typ != wsText || string(msg) != "fragmented" {
		t.Fatalf("got typ=%d %q err=%v, want text \"fragmented\"", typ, msg, err)
	}
}

func TestWSProtocolErrorsCloseTheConnection(t *testing.T) {
	for _, tc := range []struct {
		name  string
		send  func(t *testing.T, c *wsConn)
		limit int64
		want  wsStatusCode
	}{
		{"unmasked client frame", func(t *testing.T, c *wsConn) { rawFrame(t, c, 0x80|opText, false, []byte("x")) }, 0, wsStatusProtocolError},
		{"reserved bit", func(t *testing.T, c *wsConn) { rawFrame(t, c, 0x80|0x40|opText, true, []byte("x")) }, 0, wsStatusProtocolError},
		{"unknown opcode", func(t *testing.T, c *wsConn) { rawFrame(t, c, 0x80|0x3, true, nil) }, 0, wsStatusProtocolError},
		{"fragmented ping", func(t *testing.T, c *wsConn) { rawFrame(t, c, opPing, true, nil) }, 0, wsStatusProtocolError},
		{"continuation first", func(t *testing.T, c *wsConn) { rawFrame(t, c, 0x80|opContinuation, true, []byte("x")) }, 0, wsStatusProtocolError},
		{"bad UTF-8", func(t *testing.T, c *wsConn) { rawFrame(t, c, 0x80|opText, true, []byte{0xff, 0xfe}) }, 0, wsStatusInvalidPayload},
		{"too big", func(t *testing.T, c *wsConn) {
			rawFrame(t, c, opText, true, []byte("12345"))
			rawFrame(t, c, 0x80|opContinuation, true, []byte("678901"))
		}, 10, wsStatusMessageTooBig},
	} {
		t.Run(tc.name, func(t *testing.T) {
			url, ended := wsEcho(t, tc.limit)
			c := dialWS(t, url)
			tc.send(t, c)
			_, _, err := readWS(t, c)
			if got := wsCloseStatus(err); got != tc.want {
				t.Fatalf("client read ended with status %d (%v), want %d", got, err, tc.want)
			}
			select {
			case err := <-ended:
				if err == nil {
					t.Fatal("server read did not fail")
				}
			case <-time.After(5 * time.Second):
				t.Fatal("server read did not end")
			}
		})
	}
}

// Close with no reader running sends the close frame, waits for the echo and
// closes the socket; the peer sees the status and reason.
func TestWSCloseHandshake(t *testing.T) {
	got := make(chan error, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := wsAccept(w, r)
		if err != nil {
			return
		}
		defer c.CloseNow()
		_, _, err = c.Read(context.Background())
		got <- err
	}))
	defer srv.Close()
	c := dialWS(t, "ws"+strings.TrimPrefix(srv.URL, "http"))

	start := time.Now()
	if err := c.Close(wsStatusPolicyViolation, "bye"); err != nil {
		t.Fatalf("close: %v", err)
	}
	if time.Since(start) >= wsCloseWait {
		t.Fatal("Close waited out the whole close wait; the echo was not seen")
	}
	err := <-got
	if ce, ok := err.(*wsCloseError); !ok || ce.Code != wsStatusPolicyViolation || ce.Reason != "bye" {
		t.Fatalf("server saw %v, want close 1008 \"bye\"", err)
	}
	if err := c.Write(context.Background(), wsText, []byte("late")); err == nil {
		t.Fatal("write after Close succeeded")
	}
}

// Close while a Read is running returns at once; that Read ends when the peer
// answers.
func TestWSCloseWhileReading(t *testing.T) {
	url, _ := wsEcho(t, 0)
	c := dialWS(t, url)
	readDone := make(chan error, 1)
	go func() {
		_, _, err := c.Read(context.Background())
		readDone <- err
	}()
	time.Sleep(50 * time.Millisecond) // let the Read start
	if err := c.Close(wsStatusGoingAway, "stopping"); err != nil {
		t.Fatalf("close: %v", err)
	}
	select {
	case err := <-readDone:
		if wsCloseStatus(err) != wsStatusGoingAway {
			t.Fatalf("read ended with %v, want the echoed 1001", err)
		}
	case <-time.After(wsCloseWait + time.Second):
		t.Fatal("read did not end after Close")
	}
}

func TestWSAcceptRefusesBadRequests(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if c, err := wsAccept(w, r); err == nil {
			c.CloseNow()
		}
	}))
	defer srv.Close()
	url := "ws" + strings.TrimPrefix(srv.URL, "http")

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, resp, err := wsDial(ctx, url, http.Header{"Origin": []string{"https://evil.example"}})
	if err == nil || resp == nil || resp.StatusCode != http.StatusForbidden {
		t.Fatalf("cross-origin dial: err=%v resp=%v, want 403", err, resp)
	}
	r, err := http.Get(srv.URL)
	if err != nil {
		t.Fatal(err)
	}
	r.Body.Close()
	if r.StatusCode != http.StatusUpgradeRequired {
		t.Fatalf("plain GET got %d, want 426", r.StatusCode)
	}

	// Same origin is fine.
	host := strings.TrimPrefix(srv.URL, "http://")
	c, _, err := wsDial(ctx, url, http.Header{"Origin": []string{"http://" + host}})
	if err != nil {
		t.Fatalf("same-origin dial: %v", err)
	}
	c.CloseNow()
}

// A raw handshake checks the Sec-WebSocket-Accept value from RFC 6455 §1.3.
func TestWSAcceptKey(t *testing.T) {
	if got := wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="); got != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("accept key %q", got)
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if c, err := wsAccept(w, r); err == nil {
			c.CloseNow()
		}
	}))
	defer srv.Close()
	conn, err := net.Dial("tcp", strings.TrimPrefix(srv.URL, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_, _ = conn.Write([]byte("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n" +
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"))
	resp, err := http.ReadResponse(bufio.NewReader(conn), nil)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != 101 || resp.Header.Get("Sec-WebSocket-Accept") != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("handshake: %d %q", resp.StatusCode, resp.Header.Get("Sec-WebSocket-Accept"))
	}
}

// Turning decoding off sends connected clients an empty catalog.
func TestV2CatalogClearedSendsEmptyCatalog(t *testing.T) {
	h := newTestHub(t)
	h.SetCatalog(loadTestCatalog(t))
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	readMsg(t, conn) // hello_ack
	readMsg(t, conn) // catalog
	h.SetCatalog(nil)
	var cat struct {
		Type     string            `json:"type"`
		SHA      string            `json:"sha"`
		Messages []json_rawMessage `json:"messages"`
	}
	readInto(t, conn, &cat)
	if cat.Type != "catalog" || cat.SHA != "" || len(cat.Messages) != 0 {
		t.Fatalf("got %+v, want an empty catalog", cat)
	}
}

// Shutdown closes connected clients with 1001 and returns promptly.
func TestV2StopClosesClients(t *testing.T) {
	h := newTestHub(t)
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	readMsg(t, conn) // hello_ack
	ctx, cancel := context.WithTimeout(context.Background(), v2StopTimeout)
	defer cancel()
	start := time.Now()
	go h.closeClients(ctx)
	expectCloseStatus(t, conn, wsStatusGoingAway)
	if time.Since(start) > v2StopTimeout {
		t.Fatal("clients were not closed within the stop timeout")
	}
}

type json_rawMessage = []byte
