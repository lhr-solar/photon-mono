package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"server/dbc"
)

// testCatalog is the car's DBC set, loaded once.
var testCatalog = sync.OnceValues(func() (*dbc.Catalog, error) {
	return dbc.Load("../dbc/spec/dbc", DefaultCANConfig().SampleRates)
})

func loadTestCatalog(t *testing.T) *dbc.Catalog {
	t.Helper()
	c, err := testCatalog()
	if err != nil {
		t.Fatalf("load DBCs: %v", err)
	}
	return c
}

// newTestHub returns a LAN hub whose password is "secret" and whose live run is
// "run-test".
func newTestHub(t *testing.T) *v2Hub {
	t.Helper()
	return newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), func() string { return "run-test" })
}

// serveHub serves h on a test server and returns the WebSocket URL of /v2.
func serveHub(t *testing.T, h *v2Hub, cf bool) string {
	t.Helper()
	srv := httptest.NewServer(h.handler(cf))
	t.Cleanup(srv.Close)
	return "ws" + strings.TrimPrefix(srv.URL, "http") + "/v2"
}

// dialV2 opens a WebSocket to url with the given request headers.
func dialV2(t *testing.T, url string, hdr http.Header) *wsConn {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := wsDial(ctx, url, hdr)
	if err != nil {
		t.Fatalf("dial %s: %v", url, err)
	}
	// The catalog is bigger than the default 1 MiB limit allows for later growth.
	conn.SetReadLimit(4 << 20)
	t.Cleanup(func() { _ = conn.CloseNow() })
	return conn
}

// sendV2 writes v as one text message.
func sendV2(t *testing.T, conn *wsConn, v any) {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := conn.Write(ctx, wsText, b); err != nil {
		t.Fatalf("write: %v", err)
	}
}

// readMsg returns the next message, which must be text.
func readMsg(t *testing.T, conn *wsConn) []byte {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	typ, data, err := conn.Read(ctx)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if typ != wsText {
		t.Fatalf("got a binary message, want text")
	}
	return data
}

// readInto decodes the next message into v.
func readInto(t *testing.T, conn *wsConn, v any) {
	t.Helper()
	data := readMsg(t, conn)
	if err := json.Unmarshal(data, v); err != nil {
		t.Fatalf("decode %q: %v", data, err)
	}
}

// closeError reads until the connection ends and returns the error that ended it.
func closeError(t *testing.T, conn *wsConn) error {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for {
		if _, _, err := conn.Read(ctx); err != nil {
			return err
		}
	}
}

// expectCloseStatus checks that the connection ends with the given close status.
func expectCloseStatus(t *testing.T, conn *wsConn, want wsStatusCode) {
	t.Helper()
	err := closeError(t, conn)
	if got := wsCloseStatus(err); got != want {
		t.Fatalf("connection ended with status %d (%v), want %d", got, err, want)
	}
}

// hello is a hello message with the given protocol and token.
func hello(protocol int, token string) map[string]any {
	return map[string]any{"type": "hello", "protocol": protocol, "client": "test", "token": token}
}

// helloAck is the hello_ack the server sends, as the tests read it.
type helloAck struct {
	Type         string   `json:"type"`
	Protocol     int      `json:"protocol"`
	Server       string   `json:"server"`
	User         string   `json:"user"`
	ServerTimeNS int64    `json:"server_time_ns"`
	CatalogSHA   string   `json:"catalog_sha"`
	LiveRun      string   `json:"live_run"`
	Levels       []int    `json:"levels"`
	Features     []string `json:"features"`
}

// wireCatalog is the catalog message as the tests read it.
type wireCatalog struct {
	Type     string   `json:"type"`
	SHA      string   `json:"sha"`
	Buses    []string `json:"buses"`
	Messages []struct {
		ID       uint32 `json:"id"`
		Name     string `json:"name"`
		Bus      int    `json:"bus"`
		Length   uint8  `json:"length"`
		Selector int    `json:"selector"`
		Signals  []struct {
			Index        int     `json:"index"`
			Name         string  `json:"name"`
			Start        uint16  `json:"start"`
			Bits         uint8   `json:"bits"`
			LittleEndian bool    `json:"little_endian"`
			Signed       bool    `json:"signed"`
			Kind         string  `json:"kind"`
			Factor       float64 `json:"factor"`
			Offset       float64 `json:"offset"`
			Unit         string  `json:"unit"`
			Mux          int16   `json:"mux"`
		} `json:"signals"`
	} `json:"messages"`
}

func TestV2HelloAckThenCatalog(t *testing.T) {
	h := newTestHub(t)
	cat := loadTestCatalog(t)
	h.SetCatalog(cat)
	conn := dialV2(t, serveHub(t, h, false), nil)

	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)
	if ack.Type != "hello_ack" || ack.Protocol != 2 || ack.Server != "photon-server" {
		t.Fatalf("hello_ack = %+v", ack)
	}
	if ack.User != "lan:127.0.0.1" {
		t.Fatalf("user = %q, want lan:127.0.0.1", ack.User)
	}
	if ack.CatalogSHA != cat.SHA || ack.LiveRun != "run-test" || ack.ServerTimeNS == 0 {
		t.Fatalf("hello_ack = %+v, want catalog_sha %q and live_run run-test", ack, cat.SHA)
	}
	if ack.Levels == nil || len(ack.Levels) != 0 || len(ack.Features) != 1 || ack.Features[0] != "catalog" {
		t.Fatalf("levels %v features %v, want [] and [catalog]", ack.Levels, ack.Features)
	}

	var msg wireCatalog
	readInto(t, conn, &msg)
	if msg.Type != "catalog" || msg.SHA != cat.SHA {
		t.Fatalf("catalog type %q sha %q, want catalog %q", msg.Type, msg.SHA, cat.SHA)
	}
	if len(msg.Buses) != len(cat.Buses) {
		t.Fatalf("buses %v, want %v", msg.Buses, cat.Buses)
	}
	for i := range cat.Buses {
		if msg.Buses[i] != cat.Buses[i] {
			t.Fatalf("buses %v, want %v", msg.Buses, cat.Buses)
		}
	}
	if len(msg.Messages) != len(cat.Messages) {
		t.Fatalf("%d messages, want %d", len(msg.Messages), len(cat.Messages))
	}

	// Every message and signal matches the DBC, and signal indexes run 0..N-1.
	index := 0
	for i, m := range cat.Messages {
		got := msg.Messages[i]
		if got.ID != m.ID || got.Name != m.Name || got.Length != m.Length || got.Selector != m.Selector {
			t.Fatalf("message %d = %+v, want %s (id %d, length %d, selector %d)", i, got, m.Name, m.ID, m.Length, m.Selector)
		}
		wantBus := -1
		for b, name := range cat.Buses {
			if name == m.Bus {
				wantBus = b
			}
		}
		if got.Bus != wantBus {
			t.Fatalf("message %s bus %d, want %d", m.Name, got.Bus, wantBus)
		}
		if len(got.Signals) != len(m.Signals) {
			t.Fatalf("message %s has %d signals, want %d", m.Name, len(got.Signals), len(m.Signals))
		}
		for j := range m.Signals {
			s, gs := &m.Signals[j], got.Signals[j]
			if gs.Index != index {
				t.Fatalf("%s.%s index %d, want %d", m.Name, s.Name, gs.Index, index)
			}
			index++
			if gs.Name != s.Name || gs.Start != s.Start || gs.Bits != s.Bits || gs.LittleEndian != s.LittleEndian ||
				gs.Signed != s.Signed || gs.Factor != s.Factor || gs.Offset != s.Offset || gs.Unit != s.Unit || gs.Mux != s.Mux {
				t.Fatalf("%s.%s = %+v, want the DBC's %+v", m.Name, s.Name, gs, s)
			}
			if gs.Kind != map[dbc.Kind]string{dbc.Integer: "int", dbc.Float32: "f32", dbc.Float64: "f64"}[s.Kind] {
				t.Fatalf("%s.%s kind %q", m.Name, s.Name, gs.Kind)
			}
		}
	}
	if index == 0 {
		t.Fatal("catalog has no signals")
	}

	// A known message from the spec: wire ID 81 is BPSCommand, 12 bytes, on CarCAN.
	found := false
	for _, m := range msg.Messages {
		if m.ID != 81 {
			continue
		}
		found = true
		if m.Name != "BPSCommand" || m.Length != 12 || m.Bus < 0 || msg.Buses[m.Bus] != "CarCAN" {
			t.Fatalf("wire ID 81 = %+v, want BPSCommand, 12 bytes, on CarCAN", m)
		}
	}
	if !found {
		t.Fatal("no message with wire ID 81")
	}
}

func TestV2BadTokenCloses1008(t *testing.T) {
	h := newTestHub(t)
	conn := dialV2(t, serveHub(t, h, false), nil)

	sendV2(t, conn, hello(2, "wrong"))
	expectCloseStatus(t, conn, wsStatusPolicyViolation)
	if n := h.authFailures.Load(); n != 1 {
		t.Fatalf("auth failures = %d, want 1", n)
	}
}

// A client that never says hello is dropped once helloTimeout passes.
func TestV2NoHelloClosesAfterTimeout(t *testing.T) {
	h := newTestHub(t)
	h.helloTimeout = 100 * time.Millisecond
	conn := dialV2(t, serveHub(t, h, false), nil)

	start := time.Now()
	closeError(t, conn)
	if elapsed := time.Since(start); elapsed > 4*time.Second {
		t.Fatalf("connection stayed open for %v after a %v hello timeout", elapsed, h.helloTimeout)
	}
}

func TestV2ProtocolOneGetsErrorThenClose(t *testing.T) {
	h := newTestHub(t)
	conn := dialV2(t, serveHub(t, h, false), nil)

	sendV2(t, conn, hello(1, "secret"))
	var e struct {
		Type    string `json:"type"`
		Code    string `json:"code"`
		Message string `json:"message"`
	}
	readInto(t, conn, &e)
	if e.Type != "error" || e.Code != "protocol" || e.Message != "protocol 2 required" {
		t.Fatalf("error = %+v, want protocol 2 required", e)
	}
	expectCloseStatus(t, conn, wsStatusPolicyViolation)
}

func TestV2PingEchoesT(t *testing.T) {
	h := newTestHub(t) // no catalog, so nothing else arrives after hello_ack
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)

	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 1700000000123})
	var pong struct {
		Type string          `json:"type"`
		TMS  json.RawMessage `json:"t_ms"`
	}
	readInto(t, conn, &pong)
	if pong.Type != "pong" || string(pong.TMS) != "1700000000123" {
		t.Fatalf("pong = %s %s, want pong 1700000000123", pong.Type, pong.TMS)
	}
}

func TestV2UnknownTypeGetsError(t *testing.T) {
	h := newTestHub(t)
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)

	sendV2(t, conn, map[string]any{"type": "runs_list"})
	var e struct {
		Type string `json:"type"`
		Code string `json:"code"`
	}
	readInto(t, conn, &e)
	if e.Type != "error" || e.Code != "unknown_type" {
		t.Fatalf("reply = %+v, want error unknown_type", e)
	}
}

func TestV2BinaryAfterHelloGetsError(t *testing.T) {
	h := newTestHub(t)
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := conn.Write(ctx, wsBinary, []byte{1, 2, 3}); err != nil {
		t.Fatalf("write: %v", err)
	}
	var e struct {
		Code string `json:"code"`
	}
	readInto(t, conn, &e)
	if e.Code != "unexpected_binary" {
		t.Fatalf("code = %q, want unexpected_binary", e.Code)
	}
}

// A new catalog reaches every client past hello. The same SHA again does not.
func TestV2SetCatalogPushesNewSHA(t *testing.T) {
	h := newTestHub(t)
	cat := loadTestCatalog(t)
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)

	h.SetCatalog(cat)
	var first struct {
		Type string `json:"type"`
		SHA  string `json:"sha"`
	}
	readInto(t, conn, &first)
	if first.Type != "catalog" || first.SHA != cat.SHA {
		t.Fatalf("first catalog = %+v, want %q", first, cat.SHA)
	}

	h.SetCatalog(cat) // same SHA: nothing is sent
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 1})
	var pong struct {
		Type string `json:"type"`
	}
	readInto(t, conn, &pong)
	if pong.Type != "pong" {
		t.Fatalf("after an unchanged catalog got %q, want pong", pong.Type)
	}

	alt := *cat
	alt.SHA = "alt-sha-test"
	h.SetCatalog(&alt)
	var second struct {
		Type string `json:"type"`
		SHA  string `json:"sha"`
	}
	readInto(t, conn, &second)
	if second.Type != "catalog" || second.SHA != "alt-sha-test" {
		t.Fatalf("second catalog = %+v, want alt-sha-test", second)
	}
}

// After the catalog is cleared, a new client gets no catalog.
func TestV2ClearedCatalogIsNotSent(t *testing.T) {
	h := newTestHub(t)
	h.SetCatalog(loadTestCatalog(t))
	h.SetCatalog(nil)
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)
	if ack.CatalogSHA != "" {
		t.Fatalf("catalog_sha = %q after clearing, want empty", ack.CatalogSHA)
	}
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 2})
	var pong struct {
		Type string `json:"type"`
	}
	readInto(t, conn, &pong)
	if pong.Type != "pong" {
		t.Fatalf("got %q after hello_ack, want pong", pong.Type)
	}
}

// Behind Cloudflare Access a request needs a valid token, or it gets HTTP 403.
func TestV2CFNeedsAccessToken(t *testing.T) {
	h := newTestHub(t)
	jwks := newTestJWKS(t)
	h.verifier = newTestVerifier(t, jwks)
	url := serveHub(t, h, true)

	for _, tc := range []struct {
		name  string
		token string
	}{
		{"no header", ""},
		{"bad token", "not-a-token"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			hdr := http.Header{}
			if tc.token != "" {
				hdr.Set("Cf-Access-Jwt-Assertion", tc.token)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			_, resp, err := wsDial(ctx, url, hdr)
			if err == nil {
				t.Fatal("dial succeeded without a valid Access token")
			}
			if resp == nil || resp.StatusCode != http.StatusForbidden {
				t.Fatalf("dial error %v, want HTTP 403", err)
			}
		})
	}
}

// Behind Cloudflare Access the hello needs no LAN token; the user is the email.
func TestV2CFHelloAckUsesEmail(t *testing.T) {
	h := newTestHub(t)
	jwks := newTestJWKS(t)
	jwks.set("kid-1", &testKey1().PublicKey)
	h.verifier = newTestVerifier(t, jwks)
	tok := signToken(t, testKey1(), "kid-1", goodClaims(time.Now(), "dev@example.com"))

	conn := dialV2(t, serveHub(t, h, true), http.Header{"Cf-Access-Jwt-Assertion": []string{tok}})
	sendV2(t, conn, hello(2, ""))
	var ack helloAck
	readInto(t, conn, &ack)
	if ack.Type != "hello_ack" || ack.User != "dev@example.com" {
		t.Fatalf("hello_ack = %+v, want user dev@example.com", ack)
	}
}
