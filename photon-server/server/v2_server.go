package main

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"net"
	"net/http"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"

	"server/dbc"
)

const (
	v2Protocol     = 2
	v2ReadLimit    = 64 << 10 // largest message a client may send
	v2WriteTimeout = 5 * time.Second
	v2SendQueue    = 64 // messages queued for a client before it is too slow
	v2HelloTimeout = 5 * time.Second
	v2IdleTimeout  = 5 * time.Second // a client silent this long is dropped
	v2StopTimeout  = 2 * time.Second
)

// v2Current is the running protocol v2 hub, for the metadata. It is nil when
// protocol v2 is not running.
var v2Current atomic.Pointer[v2Hub]

// v2Hub is the protocol v2 server: its listeners, the clients past hello, and
// the catalog they are sent.
type v2Hub struct {
	gate     *ClientGate
	verifier *accessVerifier // Cloudflare Access check; nil when that listener is off
	webDir   string          // static bundle served at /; empty for none
	liveRun  func() string   // the frame session ID, or "" when there is none

	helloTimeout time.Duration
	idleTimeout  time.Duration

	lanListening atomic.Bool
	cfListening  atomic.Bool
	authFailures atomic.Uint64 // hello, token and Access JWT rejections

	mu         sync.Mutex
	clients    map[*v2Client]struct{} // clients past hello
	catalog    []byte                 // the current catalog message, or nil
	catalogSHA string
}

// newV2Hub builds the protocol v2 hub. Its LAN login is the gate's password.
// Cloudflare Access is checked only when both cfg.CFTeam and cfg.CFAud are set.
// liveRun gives the frame session ID for hello_ack, or "" when there is none; nil
// means "".
func newV2Hub(cfg *Config, gate *ClientGate, liveRun func() string) *v2Hub {
	h := &v2Hub{
		gate:         gate,
		webDir:       cfg.WebDir,
		liveRun:      liveRun,
		helloTimeout: v2HelloTimeout,
		idleTimeout:  v2IdleTimeout,
		clients:      make(map[*v2Client]struct{}),
	}
	if h.liveRun == nil {
		h.liveRun = func() string { return "" }
	}
	if cfg.CFTeam != "" && cfg.CFAud != "" {
		h.verifier = newAccessVerifier(cfg.CFTeam, cfg.CFAud)
	}
	return h
}

// SetCatalog makes c the catalog that clients get. Every client past hello is
// sent it when its SHA is new. A nil c clears the catalog; the clients are not
// told, and the next hello_ack carries no catalog.
func (h *v2Hub) SetCatalog(c *dbc.Catalog) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if c == nil {
		h.catalog, h.catalogSHA = nil, ""
		return
	}
	if h.catalog != nil && c.SHA == h.catalogSHA {
		return
	}
	msg := catalogJSON(c)
	if msg == nil {
		return
	}
	h.catalog, h.catalogSHA = msg, c.SHA
	for cl := range h.clients {
		cl.enqueue(msg)
	}
}

// handler serves the WebSocket at /v2 and, when there is a web dir, the bundle
// at /. cf says whether the listener sits behind Cloudflare Access.
func (h *v2Hub) handler(cf bool) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /v2", func(w http.ResponseWriter, r *http.Request) {
		h.serveWS(w, r, cf)
	})
	if h.webDir != "" {
		mux.Handle("/", http.FileServer(http.Dir(h.webDir)))
	}
	return mux
}

// serveWS runs one protocol v2 connection from upgrade to close: admission, the
// login and hello, then messages until the client leaves or goes quiet.
func (h *v2Hub) serveWS(w http.ResponseWriter, r *http.Request, cf bool) {
	ip := remoteHost(r.RemoteAddr)
	if err := h.gate.AdmitIP(ip); err != nil {
		http.Error(w, "busy", http.StatusServiceUnavailable)
		return
	}
	defer h.gate.ReleaseIP(ip)

	email := ""
	if cf {
		var err error
		if email, err = h.verifyAccess(r); err != nil {
			h.authFailures.Add(1)
			log.Printf("[photon-server] v2 access rejected from %s: %v", ip, err)
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
	}

	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{})
	if err != nil {
		return // Accept has already answered the request
	}
	defer conn.CloseNow()
	conn.SetReadLimit(v2ReadLimit)

	user, ok := h.admitHello(r.Context(), conn, ip, email, cf)
	if !ok {
		return
	}
	c := &v2Client{conn: conn, send: make(chan []byte, v2SendQueue), done: make(chan struct{})}
	h.register(c, user)
	go c.writeLoop()
	defer close(c.done)
	defer h.unregister(c)
	c.readLoop(r.Context(), h.idleTimeout)
}

// verifyAccess checks the Cloudflare Access token on r and returns its email.
func (h *v2Hub) verifyAccess(r *http.Request) (string, error) {
	if h.verifier == nil {
		return "", errors.New("cloudflare access is not configured")
	}
	return h.verifier.Verify(r.Header.Get("Cf-Access-Jwt-Assertion"))
}

// admitHello reads the first message, which must be a hello that passes the
// login, and returns the user it names. A connection that fails is closed here.
func (h *v2Hub) admitHello(ctx context.Context, conn *websocket.Conn, ip, email string, cf bool) (string, bool) {
	rctx, cancel := context.WithTimeout(ctx, h.helloTimeout)
	typ, data, err := conn.Read(rctx)
	cancel()
	var hello struct {
		Type     string `json:"type"`
		Protocol int    `json:"protocol"`
		Token    string `json:"token"`
	}
	if err != nil || typ != websocket.MessageText || json.Unmarshal(data, &hello) != nil || hello.Type != "hello" {
		h.reject(conn, ip, "hello expected")
		return "", false
	}
	if hello.Protocol != v2Protocol {
		_ = writeV2(conn, encodeV2(v2Error{Type: "error", Code: "protocol", Message: "protocol 2 required"}))
		h.reject(conn, ip, "protocol 2 required")
		return "", false
	}
	if cf {
		return email, true
	}
	if err := h.gate.CheckToken(ip, hello.Token); err != nil {
		h.reject(conn, ip, "bad token")
		return "", false
	}
	return "lan:" + ip, true
}

// reject closes a connection that failed its hello or its login, with policy
// violation 1008 and the reason.
func (h *v2Hub) reject(conn *websocket.Conn, ip, reason string) {
	h.authFailures.Add(1)
	log.Printf("[photon-server] v2 connection from %s closed: %s", ip, reason)
	_ = conn.Close(websocket.StatusPolicyViolation, reason)
}

// register makes c a client past hello. Its hello_ack and then the current
// catalog, if there is one, are queued under h.mu, so a catalog that changes
// later cannot overtake them.
func (h *v2Hub) register(c *v2Client, user string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	c.enqueue(encodeV2(v2HelloAck{
		Type:         "hello_ack",
		Protocol:     v2Protocol,
		Server:       "photon-server",
		User:         user,
		ServerTimeNS: time.Now().UnixNano(),
		CatalogSHA:   h.catalogSHA,
		LiveRun:      h.liveRun(),
		Levels:       []int{},
		Features:     []string{"catalog"},
	}))
	if h.catalog != nil {
		c.enqueue(h.catalog)
	}
	h.clients[c] = struct{}{}
}

func (h *v2Hub) unregister(c *v2Client) {
	h.mu.Lock()
	defer h.mu.Unlock()
	delete(h.clients, c)
}

// closeClients closes every client past hello. It waits for the close handshakes
// until ctx is done, then drops the connections that are left.
func (h *v2Hub) closeClients(ctx context.Context) {
	h.mu.Lock()
	clients := make([]*v2Client, 0, len(h.clients))
	for c := range h.clients {
		clients = append(clients, c)
	}
	h.mu.Unlock()

	var wg sync.WaitGroup
	for _, c := range clients {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_ = c.conn.Close(websocket.StatusGoingAway, "server stopping")
		}()
	}
	done := make(chan struct{})
	go func() {
		wg.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-ctx.Done():
	}
	for _, c := range clients {
		_ = c.conn.CloseNow()
	}
}

// v2Client is one connection past hello. From registration on, only its writer
// goroutine writes to conn; everything else queues on send.
type v2Client struct {
	conn    *websocket.Conn
	send    chan []byte   // text messages waiting for the writer
	done    chan struct{} // closed when the connection's handler returns
	closing sync.Once     // closes a client that fell behind, once
}

// enqueue queues msg for the writer. A client whose queue is full cannot keep
// up, so it is closed, and nothing waits for that to finish.
func (c *v2Client) enqueue(msg []byte) {
	if msg == nil {
		return
	}
	select {
	case c.send <- msg:
	default:
		c.closing.Do(func() {
			go func() { _ = c.conn.Close(websocket.StatusPolicyViolation, "too slow") }()
		})
	}
}

// enqueueError queues an error message with a code and a readable message.
func (c *v2Client) enqueueError(code, message string) {
	c.enqueue(encodeV2(v2Error{Type: "error", Code: code, Message: message}))
}

// writeLoop sends queued messages until the handler returns. A failed write means
// the connection is gone, so it is closed, which also ends the reads.
func (c *v2Client) writeLoop() {
	for {
		select {
		case <-c.done:
			return
		case msg := <-c.send:
			if err := writeV2(c.conn, msg); err != nil {
				_ = c.conn.CloseNow()
				return
			}
		}
	}
}

// readLoop handles messages from a client past hello until it leaves or sends
// nothing for idle. Any message, a ping included, counts as activity.
func (c *v2Client) readLoop(ctx context.Context, idle time.Duration) {
	for {
		rctx, cancel := context.WithTimeout(ctx, idle)
		typ, data, err := c.conn.Read(rctx)
		cancel()
		if err != nil {
			return
		}
		c.handle(typ, data)
	}
}

// handle answers one message from a client past hello. Only ping is supported
// so far; any other type gets an error that says so.
func (c *v2Client) handle(typ websocket.MessageType, data []byte) {
	if typ == websocket.MessageBinary {
		c.enqueueError("unexpected_binary", "binary messages are not supported yet")
		return
	}
	var msg struct {
		Type string          `json:"type"`
		TMS  json.RawMessage `json:"t_ms"`
	}
	if err := json.Unmarshal(data, &msg); err != nil {
		c.enqueueError("bad_json", "message is not a JSON object")
		return
	}
	switch msg.Type {
	case "ping":
		c.enqueue(encodeV2(v2Pong{Type: "pong", TMS: msg.TMS}))
	case "":
		c.enqueueError("unknown_type", "message has no type")
	default:
		c.enqueueError("unknown_type", msg.Type+" is not supported yet")
	}
}

// v2HelloAck is the reply to a good hello.
type v2HelloAck struct {
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

// v2Pong echoes the t_ms of a ping exactly as the client sent it.
type v2Pong struct {
	Type string          `json:"type"`
	TMS  json.RawMessage `json:"t_ms"`
}

// v2Error is an error the client can act on, by its code.
type v2Error struct {
	Type    string `json:"type"`
	Code    string `json:"code"`
	Message string `json:"message"`
}

// encodeV2 returns v as JSON, or nil after logging why it cannot be encoded.
func encodeV2(v any) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		log.Printf("[photon-server] v2 message not encoded: %v", err)
		return nil
	}
	return b
}

// writeV2 sends one text message on conn, giving up after v2WriteTimeout.
func writeV2(conn *websocket.Conn, msg []byte) error {
	ctx, cancel := context.WithTimeout(context.Background(), v2WriteTimeout)
	defer cancel()
	return conn.Write(ctx, websocket.MessageText, msg)
}

// startV2 opens the protocol v2 listeners: the LAN one on cfg.Host:cfg.V2Port,
// and the Cloudflare one on 127.0.0.1:cfg.V2CFPort when Access is configured. A
// listener that cannot start is logged and left off. Cancelling ctx ends every
// connection. The returned stop shuts the listeners and closes the clients.
func startV2(ctx context.Context, cfg *Config, h *v2Hub) (stop func()) {
	var servers []*http.Server
	serve := func(name, addr string, cf bool) {
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			log.Printf("[photon-server] v2 %s listener not started on %s: %v", name, addr, err)
			return
		}
		srv := &http.Server{
			Handler:           h.handler(cf),
			BaseContext:       func(net.Listener) context.Context { return ctx },
			ReadHeaderTimeout: 10 * time.Second,
		}
		servers = append(servers, srv)
		go func() { _ = srv.Serve(ln) }()
		if cf {
			h.cfListening.Store(true)
		} else {
			h.lanListening.Store(true)
		}
		log.Printf("[photon-server] v2 %s listening on %s (WebSocket /v2)", name, addr)
	}

	if cfg.V2Port != 0 {
		serve("LAN", net.JoinHostPort(cfg.Host, strconv.Itoa(cfg.V2Port)), false)
	} else {
		log.Printf("[photon-server] v2 LAN listener off (-v2-port is 0)")
	}
	switch {
	case cfg.V2CFPort == 0:
		log.Printf("[photon-server] v2 Cloudflare listener off (-v2-cf-port is 0)")
	case h.verifier == nil:
		log.Printf("[photon-server] v2 Cloudflare listener off (set -cf-team and -cf-aud)")
	default:
		serve("Cloudflare", net.JoinHostPort("127.0.0.1", strconv.Itoa(cfg.V2CFPort)), true)
	}

	return func() {
		stopCtx, cancel := context.WithTimeout(context.Background(), v2StopTimeout)
		defer cancel()
		for _, srv := range servers {
			_ = srv.Shutdown(stopCtx)
		}
		h.closeClients(stopCtx)
	}
}

// remoteHost is the host part of an HTTP peer address.
func remoteHost(remoteAddr string) string {
	host, _, err := net.SplitHostPort(remoteAddr)
	if err != nil {
		return remoteAddr
	}
	return host
}
