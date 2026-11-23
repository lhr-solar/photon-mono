package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"log"
	"net"
	"net/http"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"server/dbc"
)

const (
	v2Protocol     = 2
	v2ReadLimit    = 64 << 10 // largest message a client may send
	v2WriteTimeout = 5 * time.Second
	v2SendQueue    = 64 // control messages queued for a client before it is too slow
	v2LiveQueue    = 4  // live messages queued for a client; the pusher waits when full
	v2BulkQueue    = 16 // bulk messages queued for a client (video, later)
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
	gate        *ClientGate
	verifier    *accessVerifier // Cloudflare Access check; nil when that listener is off
	webDir      string          // static bundle served at /; empty for none
	liveRun     func() string   // the frame session ID, or "" when there is none
	ring        *liveRing       // the recent CAN frames that live clients read
	querier     *influxQuerier  // history queries; nil when InfluxDB is off
	tiles       *tileCache      // the sealed tiles of the live run
	tilesServed atomic.Uint64   // tile requests answered
	tileStalls  atomic.Uint64   // streaming tiles stopped because the client read nothing

	helloTimeout time.Duration
	idleTimeout  time.Duration

	lanListening atomic.Bool
	cfListening  atomic.Bool
	authFailures atomic.Uint64 // hello, token and Access JWT rejections
	gaps         atomic.Uint64 // gap messages sent to live clients that fell behind
	liveSamples  atomic.Uint64 // decoded samples sent to live clients

	mu         sync.Mutex
	clients    map[*v2Client]struct{} // clients past hello
	catalog    []byte                 // the current catalog message, or nil
	catalogSHA string
	cat        *dbc.Catalog // the catalog in use, or nil when decoding is off
	signals    []tileSignal // the signals of cat in catalog index order
}

// newV2Hub builds the protocol v2 hub. Its LAN login is the gate's password.
// Cloudflare Access is checked only when both cfg.CFTeam and cfg.CFAud are set.
// liveRun gives the frame session ID for hello_ack, or "" when there is none; nil
// means "". ring is the live frame ring the clients read; it must not be nil.
// querier answers the history requests; nil means history is off.
func newV2Hub(cfg *Config, gate *ClientGate, liveRun func() string, ring *liveRing, querier *influxQuerier) *v2Hub {
	h := &v2Hub{
		gate:         gate,
		webDir:       cfg.WebDir,
		liveRun:      liveRun,
		ring:         ring,
		querier:      querier,
		tiles:        newTileCache(),
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

// emptyCatalog tells clients that decoding is off: no DBCs, no signals.
var emptyCatalog = []byte(`{"type":"catalog","sha":"","buses":[],"messages":[]}`)

// SetCatalog makes c the catalog that clients get. Every client past hello is
// sent it when its SHA is new. A nil c means decoding is off: clients that had
// a catalog get an empty one, and later hellos get none. A new catalog clears
// every client's subscription, since its signal indexes change meaning.
func (h *v2Hub) SetCatalog(c *dbc.Catalog) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if c == nil {
		if h.catalog != nil {
			for cl := range h.clients {
				cl.enqueue(emptyCatalog)
			}
		}
		h.catalog, h.catalogSHA = nil, ""
		h.cat, h.signals = nil, nil
		h.dropSubscriptions()
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
	h.cat, h.signals = c, catalogSignals(c)
	h.dropSubscriptions()
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

	conn, err := wsAccept(w, r)
	if err != nil {
		return // Accept has already answered the request
	}
	defer conn.CloseNow()
	conn.SetReadLimit(v2ReadLimit)

	user, ok := h.admitHello(r.Context(), conn, ip, email, cf)
	if !ok {
		return
	}
	c := newV2Client(conn)
	c.hub = h
	h.register(c, user)
	go c.writeLoop()
	go c.pushLive(h)
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
func (h *v2Hub) admitHello(ctx context.Context, conn *wsConn, ip, email string, cf bool) (string, bool) {
	rctx, cancel := context.WithTimeout(ctx, h.helloTimeout)
	typ, data, err := conn.Read(rctx)
	cancel()
	var hello struct {
		Type     string `json:"type"`
		Protocol int    `json:"protocol"`
		Token    string `json:"token"`
	}
	if err != nil || typ != wsText || json.Unmarshal(data, &hello) != nil || hello.Type != "hello" {
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
func (h *v2Hub) reject(conn *wsConn, ip, reason string) {
	h.authFailures.Add(1)
	log.Printf("[photon-server] v2 connection from %s closed: %s", ip, reason)
	_ = conn.Close(wsStatusPolicyViolation, reason)
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
		Levels:       tileLevelMS,
		Features:     []string{"catalog", "live", "runs", "tiles"},
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

// closeClients closes every client past hello. Each one's reader ends when the
// peer answers the close or the close wait runs out, and its handler then
// returns; this waits for those handlers until ctx is done, then drops the
// connections that are left.
func (h *v2Hub) closeClients(ctx context.Context) {
	h.mu.Lock()
	clients := make([]*v2Client, 0, len(h.clients))
	for c := range h.clients {
		clients = append(clients, c)
	}
	h.mu.Unlock()

	for _, c := range clients {
		_ = c.conn.Close(wsStatusGoingAway, "server stopping")
	}
	for _, c := range clients {
		select {
		case <-c.done:
		case <-ctx.Done():
		}
	}
	for _, c := range clients {
		_ = c.conn.CloseNow()
	}
}

// v2Client is one connection past hello. From registration on, only its writer
// goroutine writes to conn; everything else queues on one of the three queues.
type v2Client struct {
	conn    *wsConn
	hub     *v2Hub        // the hub the client is on; set before its reader starts
	control chan outMsg   // replies and catalogs; sent first
	live    chan outMsg   // LiveSamples, Gap and tile pieces; the sender waits when full
	bulk    chan outMsg   // bulk data such as video; sent when the others are empty
	done    chan struct{} // closed when the connection's handler returns
	closing sync.Once     // closes a client whose control queue is full, once

	sub   atomic.Pointer[liveSub] // the message IDs the client subscribed to, or nil
	resub chan struct{}           // says the subscription changed; cap 1

	tileMu sync.Mutex
	tiles  map[uint32]*tileRequest // tile requests in flight, by qid
}

// outMsg is one message waiting for the writer. For a binary message the writer
// stamps the seq field of the header, so data must be a whole message.
type outMsg struct {
	typ  wsMessageType
	data []byte
}

// newV2Client wraps a connection that has passed hello. Nothing is queued yet.
func newV2Client(conn *wsConn) *v2Client {
	return &v2Client{
		conn:    conn,
		control: make(chan outMsg, v2SendQueue),
		live:    make(chan outMsg, v2LiveQueue),
		bulk:    make(chan outMsg, v2BulkQueue),
		done:    make(chan struct{}),
		resub:   make(chan struct{}, 1),
		tiles:   make(map[uint32]*tileRequest),
	}
}

// enqueue queues a text message on the control queue. A client whose queue is
// full cannot keep up, so it is closed, and nothing waits for that to finish.
func (c *v2Client) enqueue(msg []byte) {
	if msg == nil {
		return
	}
	select {
	case c.control <- outMsg{typ: wsText, data: msg}:
	default:
		c.closing.Do(func() {
			go func() { _ = c.conn.Close(wsStatusPolicyViolation, "too slow") }()
		})
	}
}

// enqueueLive queues a live message, waiting for room in the live queue. It
// returns false, without queueing, once the client is done.
func (c *v2Client) enqueueLive(m outMsg) bool {
	select {
	case c.live <- m:
		return true
	case <-c.done:
		return false
	}
}

// enqueueError queues an error message with a code and a readable message.
func (c *v2Client) enqueueError(code, message string) {
	c.enqueue(encodeV2(v2Error{Type: "error", Code: code, Message: message}))
}

// next returns the message to send next: control first, then live, then bulk.
// It waits while all three are empty, and returns false once the client is done.
func (c *v2Client) next() (outMsg, bool) {
	select {
	case m := <-c.control:
		return m, true
	default:
	}
	select {
	case m := <-c.control:
		return m, true
	case m := <-c.live:
		return m, true
	default:
	}
	select {
	case m := <-c.control:
		return m, true
	case m := <-c.live:
		return m, true
	case m := <-c.bulk:
		return m, true
	case <-c.done:
		return outMsg{}, false
	}
}

// writeLoop sends queued messages until the handler returns. Each binary message
// gets the next seq, in the order the messages go out. A failed write means the
// connection is gone, so it is closed, which also ends the reads.
func (c *v2Client) writeLoop() {
	var seq uint32
	for {
		m, ok := c.next()
		if !ok {
			return
		}
		if m.typ == wsBinary {
			binary.LittleEndian.PutUint32(m.data[4:8], seq)
			seq++
		}
		if err := writeWS(c.conn, m.typ, m.data); err != nil {
			_ = c.conn.CloseNow()
			return
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

// handle answers one message from a client past hello. Unknown types get an error
// that says so.
func (c *v2Client) handle(typ wsMessageType, data []byte) {
	if typ == wsBinary {
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
	case "subscribe":
		c.subscribe(data)
	case "runs_list":
		c.startRuns()
	case "tile_request":
		c.startTile(data)
	case "tile_cancel":
		c.cancelTile(data)
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
func writeV2(conn *wsConn, msg []byte) error {
	return writeWS(conn, wsText, msg)
}

// writeWS sends one message of type typ on conn, giving up after v2WriteTimeout.
func writeWS(conn *wsConn, typ wsMessageType, msg []byte) error {
	ctx, cancel := context.WithTimeout(context.Background(), v2WriteTimeout)
	defer cancel()
	return conn.Write(ctx, typ, msg)
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
