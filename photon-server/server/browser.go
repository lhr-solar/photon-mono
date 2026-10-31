package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/csv"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"github.com/coder/websocket"
	"io"
	"log"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"server/cache"
	"server/influx"
	"server/recording"
	"server/wire"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type browserService struct {
	cfg        *Config
	recordings *recordingService
	video      *VideoStore
	http       *http.Server
	cancel     context.CancelFunc
	wg         sync.WaitGroup
	requests   chan struct{}
	pcanMu     sync.Mutex
	pcan       pcanState
	eventMu    sync.Mutex
}

func startBrowserService(cfg *Config, m *recordingService, v *VideoStore) (*browserService, error) {
	b := &browserService{cfg: cfg, recordings: m, video: v, requests: make(chan struct{}, 8)}
	if cfg.Ui == "off" || cfg.Ui == "-" {
		return b, nil
	}
	ctx, cancel := context.WithCancel(context.Background())
	b.cancel = cancel
	b.http = &http.Server{Addr: net.JoinHostPort(cfg.Host, strconv.Itoa(cfg.HTTPPort)), Handler: b.handler(ctx), ReadHeaderTimeout: 10 * time.Second}
	ln, err := net.Listen("tcp", b.http.Addr)
	if err != nil {
		cancel()
		return nil, err
	}
	go func() {
		if err := b.http.Serve(ln); err != nil && err != http.ErrServerClosed {
			log.Printf("browser HTTP: %v", err)
		}
	}()
	log.Printf("Go browser service on %s", b.http.Addr)
	return b, nil
}
func (b *browserService) Close() {
	if b.cancel != nil {
		b.cancel()
	}
	if b.http != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = b.http.Shutdown(ctx)
	}
	b.wg.Wait()
}
func jsonReply(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func (b *browserService) handler(ctx context.Context) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) { b.websocket(ctx, w, r) })
	mux.HandleFunc("POST /api/dbcs", b.uploadDBC)
	mux.HandleFunc("GET /api/dbcs", func(w http.ResponseWriter, r *http.Request) { jsonReply(w, 200, b.recordings.registry.List()) })
	mux.HandleFunc("GET /api/recordings", func(w http.ResponseWriter, r *http.Request) { jsonReply(w, 200, b.recordings.Catalog()) })
	mux.HandleFunc("/export.csv", b.exportCSV)
	mux.HandleFunc("GET /api/aggregates", b.aggregates)
	mux.HandleFunc("POST /api/pcan/frames", b.ingestPCAN)
	mux.HandleFunc("GET /api/pcan/control", b.pcanControl)
	mux.HandleFunc("POST /api/pcan/status", b.pcanStatus)
	mux.Handle("/crash", uiCrashHandler(uiCrashLogPath()))
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		dir := resolveUiDir()
		if b.cfg.Ui != "" && b.cfg.Ui != "auto" {
			dir = b.cfg.Ui
		}
		if dir == "" {
			http.Error(w, "Build Photon Next into build/ui first", 503)
			return
		}
		http.FileServer(http.Dir(dir)).ServeHTTP(w, r)
	})
	return uiPasswordGate(b.cfg.Password, mux)
}
func (b *browserService) uploadDBC(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4*1024*1024)
	data, err := io.ReadAll(r.Body)
	if err != nil {
		jsonReply(w, 413, map[string]any{"error": "DBC exceeds 4 MiB"})
		return
	}
	label := r.URL.Query().Get("name")
	if label == "" {
		label = "uploaded.dbc"
	}
	dbc, err := b.recordings.registry.Register(label, data)
	if err != nil {
		jsonReply(w, 422, map[string]any{"error": err.Error(), "diagnostics": dbc.Diagnostics})
		return
	}
	jsonReply(w, 200, dbc)
}
func (b *browserService) httpQuery(r *http.Request) (influx.Query, error) {
	v := r.URL.Query()
	fromText, toText := v.Get("from_ms"), v.Get("to_ms")
	if fromText == "" {
		fromText = v.Get("from")
	}
	if toText == "" {
		toText = v.Get("to")
	}
	session := v.Get("session_id")
	if session == "" {
		session = v.Get("session")
	}
	if store := b.recordings.Store(session); store != nil {
		meta := store.Snapshot()
		if fromText == "" {
			fromText = strconv.FormatUint(meta.MinMS, 10)
		}
		if toText == "" {
			toText = strconv.FormatUint(meta.MaxMS, 10)
		}
	}
	from, e1 := strconv.ParseUint(fromText, 10, 64)
	to, e2 := strconv.ParseUint(toText, 10, 64)
	if v.Get("to_ms") == "" && e2 == nil {
		to++
	}
	if e1 != nil || e2 != nil {
		return influx.Query{}, fmt.Errorf("from_ms and to_ms required")
	}
	limit, _ := strconv.Atoi(v.Get("limit"))
	offset, _ := strconv.Atoi(v.Get("offset"))
	q := influx.Query{Session: session, DecodeID: v.Get("decode_id"), Signal: v.Get("signal"), From: from, To: to, Limit: limit, Offset: offset}
	if s := v.Get("tap"); s != "" {
		n, err := strconv.ParseInt(s, 10, 32)
		if err != nil {
			return q, err
		}
		tap := int32(n)
		q.Tap = &tap
	}
	return q, nil
}
func (b *browserService) aggregates(w http.ResponseWriter, r *http.Request) {
	q, err := b.httpQuery(r)
	if err != nil {
		jsonReply(w, 400, map[string]string{"error": err.Error()})
		return
	}
	rows, err := b.recordings.Query(r.Context(), q)
	if err != nil {
		jsonReply(w, 503, map[string]string{"error": err.Error()})
		return
	}
	jsonReply(w, 200, map[string]any{"rows": rows, "offset": q.Offset, "more": len(rows) == 2000})
}
func (b *browserService) exportCSV(w http.ResponseWriter, r *http.Request) {
	q, err := b.httpQuery(r)
	if err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	w.Header().Set("Content-Type", "text/csv")
	w.Header().Set("Content-Disposition", "attachment; filename=photon.csv")
	csvw := csv.NewWriter(w)
	defer csvw.Flush()
	_ = csvw.Write([]string{"time_ms", "signal", "tap", "raw_min", "raw_max", "raw_sum", "count", "min", "max", "mean"})
	q.Limit = 2000
	for {
		rows, err := b.recordings.Query(r.Context(), q)
		if err != nil {
			log.Printf("CSV query: %v", err)
			return
		}
		for _, row := range rows {
			tap := ""
			if row.Tap != nil {
				tap = strconv.Itoa(int(*row.Tap))
			}
			if err = csvw.Write([]string{strconv.FormatUint(row.TimeMS, 10), row.Signal, tap, row.Min, row.Max, row.Sum, row.Count, fmt.Sprint(row.EngineeringMin), fmt.Sprint(row.EngineeringMax), fmt.Sprint(row.EngineeringMean)}); err != nil {
				return
			}
		}
		csvw.Flush()
		if len(rows) < 2000 || csvw.Error() != nil {
			return
		}
		q.Offset += len(rows)
	}
}

type browserControl struct {
	Type            string          `json:"type"`
	ProtocolVersion int             `json:"protocol_version"`
	Source          string          `json:"source"`
	Path            string          `json:"path"`
	SourceID        string          `json:"source_id"`
	ID              string          `json:"id"`
	DBCHash         string          `json:"dbc_hash"`
	DecodeID        string          `json:"decode_id"`
	SessionID       string          `json:"session_id"`
	Signal          string          `json:"signal"`
	SignalIDs       []uint32        `json:"signal_ids"`
	Mode            string          `json:"mode"`
	Cursor          uint64          `json:"cursor_ms"`
	Speed           float64         `json:"speed"`
	From            uint64          `json:"from_ms"`
	To              uint64          `json:"to_ms"`
	QueryID         uint32          `json:"query_id"`
	MaxPoints       int             `json:"max_points"`
	Credit          uint64          `json:"credit_bytes"`
	Tap             *int32          `json:"tap"`
	Offset          int             `json:"offset"`
	At              uint64          `json:"at_ms"`
	Prefetch        int             `json:"prefetch_gops"`
	Stream          uint8           `json:"stream_id"`
	Start           uint64          `json:"start_ms"`
	ClockOffset     int64           `json:"offset_ms"`
	Armed           bool            `json:"armed"`
	Raw             json.RawMessage `json:"-"`
}
type browserOutput struct {
	kind           byte
	payload        []byte
	text           any
	query          uint32
	generation     uint64
	history        bool
	completedJob   string
	completedQuery bool
}
type browserQuery struct {
	cancel     context.CancelFunc
	generation uint64
}
type browserClient struct {
	service                 *browserService
	conn                    *websocket.Conn
	ctx                     context.Context
	seq                     uint32
	wanted                  map[uint32]bool
	can                     bool
	hello                   bool
	session, decode         string
	defs                    *cache.SessionCache
	mode                    byte
	cursor                  uint64
	speed                   float64
	lastTick                time.Time
	out                     chan browserOutput
	queries                 map[uint32]browserQuery
	generation              uint64
	credit                  uint64
	pending                 []browserOutput
	videoNext               uint64
	videoOn                 bool
	lastVideo               uint64
	lastVideoQuery          uint32
	pcanSource              string
	lastHeartbeat           time.Time
	jobs                    map[string]context.CancelFunc
	replayIDs               []uint64
	replayPos               int
	replayTime, replayAfter uint64
	tsdbPlay                bool
	tsdbMin, tsdbMax        uint64
	tsdbDir                 string
}

func (b *browserService) websocket(serverCtx context.Context, w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	conn.SetReadLimit(4 * 1024 * 1024)
	ctx, cancel := context.WithCancel(serverCtx)
	defer cancel()
	defer conn.CloseNow()
	b.wg.Add(1)
	defer b.wg.Done()
	c := &browserClient{service: b, conn: conn, ctx: ctx, wanted: make(map[uint32]bool), mode: 3, speed: 1, out: make(chan browserOutput, 32), queries: make(map[uint32]browserQuery), credit: 4 * 1024 * 1024, lastTick: time.Now(), lastHeartbeat: time.Now()}
	defer func() {
		for _, q := range c.queries {
			q.cancel()
		}
		b.releasePCAN(c)
	}()
	c.jobs = make(map[string]context.CancelFunc)
	defer func() {
		for _, cancel := range c.jobs {
			cancel()
		}
	}()
	commands := make(chan browserControl, 16)
	readErr := make(chan error, 1)
	go func() {
		for {
			kind, data, err := conn.Read(ctx)
			if err != nil {
				readErr <- err
				return
			}
			if kind != websocket.MessageText {
				continue
			}
			var command browserControl
			if err = json.Unmarshal(data, &command); err != nil {
				readErr <- err
				return
			}
			command.Raw = append([]byte(nil), data...)
			select {
			case commands <- command:
			case <-ctx.Done():
				return
			}
		}
	}()
	live := b.recordings.Subscribe()
	defer b.recordings.Unsubscribe(live)
	ticker := time.NewTicker(16 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-readErr:
			return
		case command := <-commands:
			if err := c.control(command); err != nil {
				if c.sendJSON(map[string]any{"type": "error_alert", "code": command.Type, "query_id": command.QueryID, "message": err.Error()}) != nil {
					return
				}
			}
		case item := <-live:
			if c.pcanSource != "" {
				s := b.recordings.Store(item.Session)
				if s == nil || s.Snapshot().Source != c.pcanSource {
					continue
				}
			}
			if c.mode == 3 && c.session != "" && c.session != item.Session && !b.recordings.IsActive(c.session) {
				c.session = ""
				c.decode = ""
			}
			if !c.hello || c.mode != 3 || c.session != "" && c.session != item.Session {
				continue
			}
			if c.session == "" {
				c.session = item.Session
				c.defs = item.Defs
				if c.sendCatalog() != nil {
					return
				}
				if c.sendSelection() != nil {
					return
				}
			}
			if item.Defs != nil && (c.defs == nil || c.defs.DBCSHA != item.Defs.DBCSHA) {
				c.defs = item.Defs
				if c.sendCatalog() != nil {
					return
				}
				if c.sendSelection() != nil {
					return
				}
			}
			if c.emit(item.Batch, item.Defs) != nil {
				return
			}
			c.cursor = item.Batch.TimeMS
		case result := <-c.out:
			if result.completedJob != "" {
				if cancel := c.jobs[result.completedJob]; cancel != nil {
					cancel()
					delete(c.jobs, result.completedJob)
				}
			}
			if result.query != 0 {
				q, ok := c.queries[result.query]
				if !ok || q.generation != result.generation {
					continue
				}
			}
			if result.history && (len(c.pending) > 0 || uint64(len(result.payload)+12) > c.credit) {
				c.pending = append(c.pending, result)
			} else {
				if c.sendOutput(result) != nil {
					return
				}
			}
		case now := <-ticker.C:
			if err := c.tick(now); err != nil {
				return
			}
		}
	}
}
func (c *browserClient) sendJSON(v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(c.ctx, 5*time.Second)
	defer cancel()
	return c.conn.Write(ctx, websocket.MessageText, data)
}
func (c *browserClient) sendBinary(kind byte, p []byte) error {
	c.seq++
	ctx, cancel := context.WithTimeout(c.ctx, 5*time.Second)
	defer cancel()
	return c.conn.Write(ctx, websocket.MessageBinary, wire.BrowserFrame(kind, c.seq, p))
}
func (c *browserClient) sendOutput(o browserOutput) error {
	if o.completedQuery {
		defer func() {
			if q, ok := c.queries[o.query]; ok && q.generation == o.generation {
				q.cancel()
				delete(c.queries, o.query)
			}
		}()
	}
	if o.text != nil {
		return c.sendJSON(o.text)
	}
	if o.history {
		c.credit -= uint64(12 + len(o.payload))
	}
	return c.sendBinary(o.kind, o.payload)
}
func (c *browserClient) sendCatalog() error {
	if c.defs == nil && c.decode != "raw" {
		if d, ok := c.service.recordings.registry.Get(c.service.recordings.DefaultDBC()); ok {
			c.defs = d.Defs
		}
	}
	var signals []map[string]any
	if c.defs != nil {
		for _, series := range c.defs.Series() {
			d := series.Def
			signals = append(signals, map[string]any{"id": series.ID, "message_id": d.ID.CANID, "message": d.Message, "name": series.Name, "unit": d.Unit, "min": d.Min, "max": d.Max, "tap": series.Tap})
		}
	}
	return c.sendJSON(map[string]any{"type": "signal_catalog", "signals": signals, "dbc_id": func() string {
		if c.defs != nil {
			return c.defs.DBCSHA
		}
		return ""
	}()})
}
func (c *browserClient) ack() error {
	var dbcs []string
	for _, d := range c.service.recordings.registry.List() {
		dbcs = append(dbcs, d.Hash)
	}
	source := "daq_tcp"
	if c.pcanSource != "" {
		source = "pcan"
	}
	if c.mode != 3 {
		source = "log_replay"
	}
	hash := ""
	if c.defs != nil {
		hash = c.defs.DBCSHA
	}
	ack := map[string]any{"type": "hello_ack", "protocol_version": 1, "server": "photon-server", "features": []string{"server_replay", "daq_tcp", "log_replay", "pcan", "dbc", "dbc_upload", "redecode", "history", "aggregate_history", "catalog", "video", "video_locate", "video_clock", "analysis_compute"}, "dbc": map[string]any{"id": hash, "bundled": dbcs}, "active_source": map[string]any{"kind": source, "host": c.service.cfg.Host, "path": c.session}}
	if days := driveDays(c.service.cfg.DataDir); len(days) > 0 {
		ack["drive_days"] = days
	}
	return c.sendJSON(ack)
}
func (c *browserClient) selectRecording(id, decode string) error {
	s := c.service.recordings.Store(id)
	if s == nil {
		if sess, ok := findTsdbSession(c.service.cfg.DataDir, id); ok {
			return c.selectTsdb(sess)
		}
		return fmt.Errorf("unknown recording")
	}
	c.tsdbPlay = false
	m := s.Snapshot()
	if decode == "" {
		decode = m.DefaultDecode
	}
	if decode == "" && m.Raw {
		decode = "raw"
	}
	var defs *cache.SessionCache
	for _, d := range m.Decodes {
		if d.ID == decode {
			if dbc, ok := c.service.recordings.registry.Get(d.DBCHash); ok {
				defs = dbc.Defs
			}
		}
	}
	if defs == nil && decode != "raw" {
		return fmt.Errorf("recording has no selected decoding")
	}
	c.session = id
	c.decode = decode
	c.defs = defs
	_, _, minMS, _ := s.DecodeBounds(decode)
	c.cursor = minMS
	c.prepareReplay(s, c.cursor)
	c.replayPos = 0
	c.mode = 1
	if err := c.sendCatalog(); err != nil {
		return err
	}
	return c.sendSelection()
}

func (c *browserClient) sendSelection() error {
	s := c.service.recordings.Store(c.session)
	if s == nil {
		return nil
	}
	meta := s.Snapshot()
	decode := c.decode
	if decode == "" {
		decode = meta.DefaultDecode
	}
	hash := ""
	for _, d := range meta.Decodes {
		if d.ID == decode {
			hash = d.DBCHash
		}
	}
	_, _, minMS, maxMS := s.DecodeBounds(decode)
	return c.sendJSON(map[string]any{"type": "recording_selected", "session_id": meta.ID, "decode_id": decode, "dbc_hash": hash, "min_ms": minMS, "max_ms": maxMS, "has_raw": meta.Raw, "coarse_history": meta.Coarse})
}
func (c *browserClient) prepareReplay(s *recording.Store, at uint64) {
	start, end, _, _ := s.DecodeBounds(c.decode)
	c.replayTime = at
	c.replayAfter = 0
	c.replayIDs = s.TimePage(at, 0, start, end, 512)
	c.replayPos = 0
}
func (c *browserClient) control(q browserControl) error {
	if q.Type == "hello" {
		if q.ProtocolVersion != 1 {
			return fmt.Errorf("protocol v1 required")
		}
		c.hello = true
		if q.Source == "pcan" {
			if err := c.service.configurePCAN(c, q); err != nil {
				return err
			}
		}
		if q.SessionID != "" {
			if err := c.selectRecording(q.SessionID, q.DecodeID); err != nil {
				return err
			}
			if q.Cursor != 0 {
				restore := q
				restore.Type = "timeline_control"
				if restore.Mode == "" {
					restore.Mode = "pause"
				}
				if err := c.control(restore); err != nil {
					return err
				}
			}
		}
		if err := c.ack(); err != nil {
			return err
		}
		return c.sendCatalog()
	}
	if !c.hello {
		return fmt.Errorf("hello required")
	}
	switch q.Type {
	case "heartbeat":
		c.lastHeartbeat = time.Now()
		c.service.pcanMu.Lock()
		if c.service.pcan.owner == c {
			c.service.pcan.heartbeat = c.lastHeartbeat
		}
		c.service.pcanMu.Unlock()
		return nil
	case "subscribe_signals", "unsubscribe_signals":
		for _, id := range q.SignalIDs {
			if q.Type == "subscribe_signals" {
				c.wanted[id] = true
			} else {
				delete(c.wanted, id)
			}
		}
	case "subscribe_can":
		c.can = true
	case "unsubscribe_can":
		c.can = false
	case "catalog_list":
		var rows []map[string]any
		var videoRows []GopRow
		videoOffset := int64(0)
		if c.service.video != nil {
			videoRows = c.service.video.PointerRows()
			videoOffset = c.service.video.ManualOffset()
		}
		for _, m := range c.service.recordings.Catalog() {
			_, _, minMS, maxMS := c.service.recordings.Store(m.ID).DecodeBounds(m.DefaultDecode)
			hash := ""
			for _, d := range m.Decodes {
				if d.ID == m.DefaultDecode {
					hash = d.DBCHash
				}
			}
			hasVideo := false
			for _, row := range videoRows {
				start, end := int64(row.StartMs)+videoOffset, int64(row.EndMs)+videoOffset
				if start <= int64(maxMS) && end >= int64(minMS) {
					hasVideo = true
					break
				}
			}
			rows = append(rows, map[string]any{"id": m.ID, "path": m.ID, "min_ms": minMS, "max_ms": maxMS, "dbc_hash": hash, "sample_count": m.Records, "has_video": hasVideo, "decode_id": m.DefaultDecode, "decodes": m.Decodes, "source": m.Source, "has_raw": m.Raw})
		}
		rows = c.appendTsdbSessions(rows, videoRows, videoOffset)
		return c.sendJSON(map[string]any{"type": "catalog_result", "sessions": rows})
	case "select_source", "recording_select":
		if q.Source == "pcan" {
			return c.service.configurePCAN(c, q)
		}
		if q.Source == "daq_tcp" {
			c.mode = 3
			c.session = ""
			c.decode = ""
			c.tsdbPlay = false
			return c.ack()
		}
		id := q.SessionID
		if id == "" {
			id = q.Path
		}
		if err := c.selectRecording(id, q.DecodeID); err != nil {
			return err
		}
		return c.ack()
	case "timeline_control":
		if q.Mode == "live" {
			c.mode = 3
			c.session = ""
			c.decode = ""
			c.tsdbPlay = false
			return c.ack()
		}
		if c.session == "" {
			return fmt.Errorf("select a recording first")
		}
		s := c.service.recordings.Store(c.session)
		if s == nil && c.tsdbPlay {
			minMS, maxMS := c.tsdbMin, c.tsdbMax
			if q.Cursor != 0 {
				at := q.Cursor
				if at < minMS {
					at = minMS
				}
				if at > maxMS {
					at = maxMS
				}
				c.cursor = at
			}
			if q.Mode == "play" {
				c.mode = 2
			} else {
				c.mode = 1
			}
			if q.Speed != 0 {
				if q.Speed < 0.01 || q.Speed > 32 || math.IsNaN(q.Speed) || math.IsInf(q.Speed, 0) {
					return fmt.Errorf("speed must be 0.01..32")
				}
				c.speed = q.Speed
			}
			c.lastTick = time.Now()
			return c.ack()
		}
		if s == nil {
			return fmt.Errorf("unknown recording")
		}
		_, _, minMS, maxMS := s.DecodeBounds(c.decode)
		if q.Cursor != 0 {
			at := q.Cursor
			if at < minMS {
				at = minMS
			}
			if at > maxMS {
				at = maxMS
			}
			c.cursor = at
			c.prepareReplay(s, at)
			if err := c.snapshotAt(s, at); err != nil {
				return err
			}
		}
		if q.Mode == "play" {
			c.mode = 2
		} else {
			c.mode = 1
		}
		if q.Speed != 0 {
			if q.Speed < 0.01 || q.Speed > 32 || math.IsNaN(q.Speed) || math.IsInf(q.Speed, 0) {
				return fmt.Errorf("speed must be 0.01..32")
			}
			c.speed = q.Speed
		}
		c.lastTick = time.Now()
	case "dbc_apply", "select_dbc":
		hash := q.DBCHash
		if hash == "" {
			hash = q.ID
		}
		if hash == "" {
			hash = q.Path
		}
		source := q.SourceID
		if source == "" && c.mode == 3 {
			if s := c.service.recordings.Store(c.session); s != nil {
				source = s.Snapshot().Source
			}
		}
		d, err := c.service.recordings.Apply(source, hash)
		if err != nil {
			return err
		}
		if dbc, ok := c.service.recordings.registry.Get(d.DBCHash); ok {
			c.defs = dbc.Defs
		}
		if err := c.sendCatalog(); err != nil {
			return err
		}
		return c.sendJSON(map[string]any{"type": "dbc_applied", "decoding": d})
	case "recording_redecode":
		d, err := c.service.recordings.Redecode(q.SessionID, q.DBCHash)
		if err != nil {
			return err
		}
		return c.sendJSON(map[string]any{"type": "redecode_status", "decoding": d})
	case "redecode_cancel":
		c.service.recordings.CancelDecode(q.DecodeID)
	case "redecode_status":
		s := c.service.recordings.Store(q.SessionID)
		if s == nil {
			return fmt.Errorf("unknown recording")
		}
		return c.sendJSON(map[string]any{"type": "redecode_status", "decodings": s.Snapshot().Decodes})
	case "history_query", "aggregate_query":
		return c.startHistory(q)
	case "history_cancel":
		for id, job := range c.queries {
			if q.QueryID == 0 || q.QueryID == id {
				job.cancel()
				delete(c.queries, id)
			}
		}
		c.pending = nil
	case "history_credit":
		c.credit += q.Credit
		if c.credit > 64*1024*1024 {
			c.credit = 64 * 1024 * 1024
		}
	case "video_locate":
		return c.locateVideo(q)
	case "video_subscribe":
		c.videoOn = true
		if c.service.video != nil {
			seq, sps, pps, ok := c.service.video.ReplayStart(q.Start)
			if q.Mode == "live" {
				_, seq, ok = c.service.video.LatestKeyframe()
			}
			if ok {
				c.videoNext = seq
				for _, p := range [][]byte{sps, pps} {
					if len(p) > 0 {
						if err := c.sendVideo(VideoFrame{TimestampMs: q.Start, Flags: VideoFlagConfig, Payload: p}, 0); err != nil {
							return err
						}
					}
				}
			}
		}
	case "video_stop":
		c.videoOn = false
	case "video_clock":
		if err := c.service.video.SetManualOffset(q.ClockOffset); err != nil {
			return err
		}
		return c.sendJSON(map[string]any{"type": "video_clock", "offset_ms": q.ClockOffset})
	case "analysis_compute_request", "analysis_compute_cancel":
		return c.startJob(q)
	case "tx_arm_request", "tx_can_frame":
		return c.service.pcanCommand(c, q)
	case "event_create", "event_list", "threshold_set":
		return c.eventControl(q)
	default:
		return fmt.Errorf("unsupported control %s", q.Type)
	}
	return nil
}
func (c *browserClient) emit(b wire.Batch, defs *cache.SessionCache) error {
	b.Frames = append([]cache.Frame(nil), b.Frames...)
	if s := c.service.recordings.Store(c.session); s != nil {
		bus := s.Snapshot().Bus
		for i := range b.Frames {
			b.Frames[i].Bus = bus
		}
	}
	if c.can {
		if err := c.sendBinary(2, wire.CANMonitor(b.Frames)); err != nil {
			return err
		}
	}
	if defs != nil {
		for _, f := range b.Frames {
			p := wire.Telemetry(f.TimeMs, defs.Decode(f), c.wanted)
			if len(p) > 0 {
				if err := c.sendBinary(1, p); err != nil {
					return err
				}
			}
		}
	}
	return nil
}
func (c *browserClient) snapshotAt(s *recording.Store, at uint64) error {
	bus := s.Snapshot().Bus
	start, end, minMS, _ := s.DecodeBounds(c.decode)
	from := minMS
	if at > 60000 && at-60000 > from {
		from = at - 60000
	}
	ids := s.RangeIDs(from, at+1, end)
	if len(ids) > 512 {
		ids = ids[len(ids)-512:]
	}
	type key struct {
		id    uint32
		flags uint8
		tap   int32
	}
	latest := make(map[key]cache.Frame)
	for _, id := range ids {
		if id < start {
			continue
		}
		raw, err := s.Read(id)
		if err != nil {
			return err
		}
		batch, err := wire.ReadBatch(bytes.NewReader(raw))
		if err != nil {
			return err
		}
		for _, f := range batch.Frames {
			f.Bus = bus
			tap := int32(-1)
			if c.defs != nil {
				if v := c.defs.Decode(f); len(v) > 0 {
					tap = v[0].Tap
				}
			}
			latest[key{f.CANID, f.Flags, tap}] = f
		}
	}
	frames := make([]cache.Frame, 0, len(latest))
	for _, f := range latest {
		frames = append(frames, f)
	}
	sort.Slice(frames, func(i, j int) bool { return frames[i].TimeMs < frames[j].TimeMs })
	return c.emit(wire.Batch{Frames: frames, TimeMS: at}, c.defs)
}
func (c *browserClient) tick(now time.Time) error {
	if !c.hello {
		return nil
	}
	if c.pcanSource != "" && now.Sub(c.lastHeartbeat) > 3*time.Second {
		c.service.releasePCAN(c)
	}
	for len(c.pending) > 0 {
		o := c.pending[0]
		q, ok := c.queries[o.query]
		if !ok || q.generation != o.generation {
			c.pending = c.pending[1:]
			continue
		}
		if uint64(len(o.payload)+12) > c.credit {
			break
		}
		if err := c.sendOutput(o); err != nil {
			return err
		}
		c.pending = c.pending[1:]
	}
	delta := now.Sub(c.lastTick)
	c.lastTick = now
	s := c.service.recordings.Store(c.session)
	if s != nil {
		m := s.Snapshot()
		_, _, minMS, maxMS := s.DecodeBounds(c.decode)
		if c.mode == 2 {
			c.cursor += uint64(float64(delta.Milliseconds()) * c.speed)
			if c.cursor > maxMS {
				c.cursor = maxMS
			}
			for n := 0; n < 32; n++ {
				if c.replayPos >= len(c.replayIDs) {
					start, end, _, _ := s.DecodeBounds(c.decode)
					c.replayIDs = s.TimePage(c.replayTime, c.replayAfter, start, end, 512)
					c.replayPos = 0
					if len(c.replayIDs) == 0 {
						break
					}
				}
				raw, err := s.Read(c.replayIDs[c.replayPos])
				if err != nil {
					return err
				}
				batch, err := wire.ReadBatch(bytes.NewReader(raw))
				if err != nil {
					return err
				}
				if batch.TimeMS > c.cursor {
					break
				}
				if err := c.emit(batch, c.defs); err != nil {
					return err
				}
				c.replayTime = batch.TimeMS
				c.replayAfter = c.replayIDs[c.replayPos]
				c.replayPos++
			}
			if m.Raw && c.replayPos >= len(c.replayIDs) || !m.Raw && c.cursor >= maxMS {
				c.mode = 1
			}
		}
		if err := c.sendBinary(5, wire.Timeline(c.mode, c.cursor, minMS, maxMS, m.Records)); err != nil {
			return err
		}
	} else if c.tsdbPlay {
		if c.mode == 2 {
			c.cursor += uint64(float64(delta.Milliseconds()) * c.speed)
			if c.cursor > c.tsdbMax {
				c.cursor = c.tsdbMax
				c.mode = 1
			}
		}
		if err := c.sendBinary(5, wire.Timeline(c.mode, c.cursor, c.tsdbMin, c.tsdbMax, 0)); err != nil {
			return err
		}
	}
	if c.videoOn && c.service.video != nil {
		offset := c.service.video.ManualOffset()
		for n := 0; n < 32; n++ {
			f, ok, err := c.service.video.ReadSeq(c.videoNext)
			if err != nil {
				return err
			}
			if !ok {
				break
			}
			// Replay cursor is telemetry time. Frame stamps are video time.
			// Hold a frame only when the UI would not show it yet.
			if c.mode != 3 && !videoFrameDue(f.TimestampMs, c.cursor, offset) {
				break
			}
			if err = c.sendVideo(f, 0); err != nil {
				return err
			}
			c.videoNext++
		}
	}
	return nil
}

// videoFrameDue matches the UI: cursorMsForVideo(videoMs, offset) <= cursor.
// A negative manual offset means video time runs ahead of the playhead.
func videoFrameDue(videoMs, cursor uint64, offset int64) bool {
	var telemetry uint64
	if offset >= 0 {
		add := uint64(offset)
		if videoMs > ^uint64(0)-add {
			telemetry = ^uint64(0)
		} else {
			telemetry = videoMs + add
		}
	} else {
		sub := uint64(-offset)
		if videoMs > sub {
			telemetry = videoMs - sub
		}
	}
	return telemetry <= cursor
}

func (c *browserClient) sendVideo(f VideoFrame, query uint32) error {
	p := make([]byte, 12+len(f.Payload))
	binary.LittleEndian.PutUint64(p, f.TimestampMs)
	p[8] = f.Flags
	copy(p[12:], f.Payload)
	c.lastVideo = f.TimestampMs
	if query != 0 {
		return c.sendBinary(8, wire.BrowserFrame(4, query, p))
	}
	return c.sendBinary(4, p)
}
func (c *browserClient) locateVideo(q browserControl) error {
	if c.service.video == nil {
		return fmt.Errorf("video unavailable")
	}
	if q.Prefetch < 1 {
		q.Prefetch = 4
	}
	if q.Prefetch > 16 {
		q.Prefetch = 16
	}
	res := c.service.video.Locate(q.Stream, q.At, q.Prefetch)
	var buf bytes.Buffer
	if err := EncodeVideoLocateReply(&buf, res); err != nil {
		return err
	}
	body := buf.Bytes()[4:]
	if q.QueryID != 0 {
		if err := c.sendBinary(8, wire.BrowserFrame(7, q.QueryID, body)); err != nil {
			return err
		}
	} else {
		if err := c.sendBinary(7, body); err != nil {
			return err
		}
	}
	if !res.Found {
		return nil
	}
	for _, p := range [][]byte{res.SPS, res.PPS} {
		if len(p) > 0 {
			if err := c.sendVideo(VideoFrame{TimestampMs: res.Entry.StartMs, Flags: VideoFlagConfig, Payload: p}, q.QueryID); err != nil {
				return err
			}
		}
	}
	rows := append([]GopRow{res.Entry}, res.Prefetch...)
	frames, err := c.service.video.framesForRows(rows)
	if err != nil {
		return err
	}
	for _, f := range frames {
		if q.At == 0 && q.QueryID == c.lastVideoQuery && f.seq <= c.videoNext {
			continue
		}
		if err := c.sendVideo(f.frame, q.QueryID); err != nil {
			return err
		}
		c.videoNext = f.seq
	}
	c.lastVideoQuery = q.QueryID
	return c.sendJSON(map[string]any{"type": "video_status", "connected": true, "query_id": q.QueryID, "reason": "prefetch_complete", "last_ts_ms": c.lastVideo, "frames": len(frames), "dropped": 0, "mode": q.Mode})
}

type sequencedVideo struct {
	seq   uint64
	frame VideoFrame
}

func (s *VideoStore) framesForRows(rows []GopRow) ([]sequencedVideo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []sequencedVideo
	bytesRead := 0
	for _, e := range s.entries {
		matched := false
		for _, row := range rows {
			if uint64(e.segmentID) == uint64(row.SegmentID) && uint64(e.offset) >= row.ByteOffset && uint64(e.offset) < row.ByteOffset+uint64(row.ByteLength) {
				matched = true
				break
			}
		}
		if !matched {
			continue
		}
		bytesRead += int(e.size)
		if bytesRead > 16*1024*1024 {
			return nil, fmt.Errorf("video prefetch exceeds 16 MiB")
		}
		p, err := s.readPayloadLocked(e)
		if err != nil {
			return nil, err
		}
		out = append(out, sequencedVideo{e.seq, VideoFrame{TimestampMs: e.timestampMs, Flags: e.flags, Payload: p}})
	}
	return out, nil
}

// Plot history queries and delivery.
func (c *browserClient) startHistory(q browserControl) error {
	if q.QueryID == 0 {
		return fmt.Errorf("nonzero query_id required")
	}
	if old, ok := c.queries[q.QueryID]; ok {
		old.cancel()
		delete(c.queries, q.QueryID)
	}
	if len(c.queries) >= 8 {
		return fmt.Errorf("too many active queries")
	}
	if q.SessionID == "" {
		q.SessionID = c.session
	}
	if q.DecodeID == "" && q.SessionID == c.session {
		q.DecodeID = c.decode
	}
	if q.MaxPoints <= 0 || q.MaxPoints > 2000 {
		q.MaxPoints = 2000
	}
	if q.Credit > 0 {
		c.credit += q.Credit
		if c.credit > 64*1024*1024 {
			c.credit = 64 * 1024 * 1024
		}
	}
	ctx, cancel := context.WithCancel(c.ctx)
	c.generation++
	generation := c.generation
	c.queries[q.QueryID] = browserQuery{cancel, generation}
	defs := c.defs
	tsdbDir := ""
	if sess, ok := findTsdbSession(c.service.cfg.DataDir, q.SessionID); ok {
		tsdbDir = sess.Dir
	}
	if s := c.service.recordings.Store(q.SessionID); s != nil {
		m := s.Snapshot()
		if q.DecodeID == "" {
			q.DecodeID = m.DefaultDecode
		}
		for _, d := range m.Decodes {
			if d.ID == q.DecodeID {
				if dbc, ok := c.service.recordings.registry.Get(d.DBCHash); ok {
					defs = dbc.Defs
				}
			}
		}
	}
	go func() {
		select {
		case c.service.requests <- struct{}{}:
		case <-ctx.Done():
			return
		}
		defer func() { <-c.service.requests }()
		send := func(o browserOutput) bool {
			o.query = q.QueryID
			o.generation = generation
			select {
			case c.out <- o:
				return true
			case <-ctx.Done():
				return false
			}
		}
		fail := func(err error) {
			send(browserOutput{text: map[string]any{"type": "error_alert", "code": "history_query", "query_id": q.QueryID, "message": err.Error()}, completedQuery: true})
		}
		if tsdbDir != "" {
			ids := q.SignalIDs
			if len(ids) == 0 {
				fail(fmt.Errorf("signal_ids required"))
				return
			}
			rowsByID := queryTsdbHistory(tsdbDir, ids, q.From, q.To, q.MaxPoints)
			type series struct {
				id   uint32
				rows []influx.Row
			}
			var data []series
			var info []map[string]any
			for _, id := range ids {
				rows := rowsByID[id]
				if len(rows) == 0 {
					continue
				}
				data = append(data, series{id, rows})
				info = append(info, map[string]any{"signal_id": id, "level": historyLevel(rows[0].BucketMS), "bucket_ms": rows[0].BucketMS, "points": len(rows), "t_min": rows[0].TimeMS, "t_max": rows[len(rows)-1].TimeMS, "chunks": 1})
			}
			if !send(browserOutput{text: map[string]any{"type": "history_result", "query_id": q.QueryID, "level": 0, "bucket_ms": 1000, "from_ms": q.From, "to_ms": q.To, "series": info, "truncated": false}, completedQuery: len(data) == 0}) {
				return
			}
			for seq, s := range data {
				if !send(browserOutput{kind: 6, payload: historyPayload(q.QueryID, s.id, uint32(seq), s.rows), history: true, completedQuery: seq == len(data)-1}) {
					return
				}
			}
			return
		}
		if q.Type == "aggregate_query" {
			rows, err := c.service.recordings.Query(ctx, influx.Query{Session: q.SessionID, DecodeID: q.DecodeID, Signal: q.Signal, Tap: q.Tap, From: q.From, To: q.To, Limit: q.MaxPoints, Offset: q.Offset})
			if err != nil {
				fail(err)
				return
			}
			send(browserOutput{text: map[string]any{"type": "aggregate_result", "query_id": q.QueryID, "session_id": q.SessionID, "decode_id": q.DecodeID, "rows": rows, "offset": q.Offset, "more": len(rows) == q.MaxPoints}, completedQuery: true})
			return
		}
		if defs == nil {
			fail(fmt.Errorf("no signal definitions"))
			return
		}
		ids := q.SignalIDs
		if len(ids) == 0 {
			fail(fmt.Errorf("signal_ids required"))
			return
		}
		if len(ids) > 512 {
			fail(fmt.Errorf("too many signals"))
			return
		}
		lookup := make(map[uint32]cache.Series)
		for _, series := range defs.Series() {
			lookup[series.ID] = series
		}
		type series struct {
			id   uint32
			rows []influx.Row
		}
		var data []series
		var info []map[string]any
		truncated := false
		budget := 16000
		for _, id := range ids {
			if budget <= 0 {
				truncated = true
				break
			}
			definition, ok := lookup[id]
			if !ok {
				continue
			}
			d := definition.Def
			tap := q.Tap
			if definition.Tap != nil {
				tap = definition.Tap
			}
			if tap == nil {
				noTap := int32(-1)
				tap = &noTap
			}
			rows, err := c.service.recordings.Query(ctx, influx.Query{Session: q.SessionID, DecodeID: q.DecodeID, Signal: d.Message + "." + d.ID.Name, Tap: tap, From: q.From, To: q.To, Limit: min(q.MaxPoints, budget)})
			if err != nil {
				fail(err)
				return
			}
			sort.SliceStable(rows, func(i, j int) bool { return rows[i].TimeMS < rows[j].TimeMS })
			if len(rows) == q.MaxPoints {
				truncated = true
			}
			if len(rows) == 0 {
				continue
			}
			data = append(data, series{id, rows})
			budget -= len(rows)
			bucket := rows[0].BucketMS
			if bucket == 0 {
				bucket = 1000
			}
			level := historyLevel(bucket)
			info = append(info, map[string]any{"signal_id": id, "level": level, "bucket_ms": bucket, "points": len(rows), "t_min": rows[0].TimeMS, "t_max": rows[len(rows)-1].TimeMS, "chunks": 1, "approximate": rows[0].Approximate})
		}
		if !send(browserOutput{text: map[string]any{"type": "history_result", "query_id": q.QueryID, "level": 2, "bucket_ms": 1000, "from_ms": q.From, "to_ms": q.To, "series": info, "truncated": truncated}, completedQuery: len(data) == 0}) {
			return
		}
		for seq, s := range data {
			if !send(browserOutput{kind: 6, payload: historyPayload(q.QueryID, s.id, uint32(seq), s.rows), history: true, completedQuery: seq == len(data)-1}) {
				return
			}
		}
	}()
	return nil
}

// Presence bits preserve gaps in one-second plot history.
func historyPayload(query, signal, seq uint32, rows []influx.Row) []byte {
	n := len(rows)
	bucket := uint32(1000)
	if n > 0 && rows[0].BucketMS != 0 {
		bucket = rows[0].BucketMS
	}
	var base uint64
	slots := 0
	if n > 0 {
		base = rows[0].TimeMS
		slots = int((rows[n-1].TimeMS-base)/uint64(bucket)) + 1
	}
	presence := make([]byte, (slots+7)/8)
	for _, r := range rows {
		slot := (r.TimeMS - base) / uint64(bucket)
		presence[slot/8] |= 1 << uint(slot%8)
	}
	p := make([]byte, 36+len(presence)+n*3*4)
	binary.LittleEndian.PutUint32(p, query)
	binary.LittleEndian.PutUint32(p[4:], signal)
	p[8] = byte(historyLevel(bucket))
	p[9] = 7
	binary.LittleEndian.PutUint16(p[10:], 1)
	binary.LittleEndian.PutUint32(p[12:], seq)
	if n > 0 {
		binary.LittleEndian.PutUint64(p[16:], rows[0].TimeMS)
	}
	binary.LittleEndian.PutUint32(p[24:], bucket)
	binary.LittleEndian.PutUint32(p[28:], uint32(slots))
	binary.LittleEndian.PutUint32(p[32:], uint32(n))
	copy(p[36:], presence)
	offset := 36 + len(presence)
	for _, which := range []int{0, 1, 2} {
		for _, r := range rows {
			v := r.EngineeringMin
			if which == 1 {
				v = r.EngineeringMax
			}
			if which == 2 {
				v = r.EngineeringMean
			}
			binary.LittleEndian.PutUint32(p[offset:], math.Float32bits(float32(v)))
			offset += 4
		}
	}
	return p
}
func historyLevel(bucket uint32) int {
	switch bucket {
	case 1:
		return 0
	case 100:
		return 1
	case 1000:
		return 2
	case 10000:
		return 3
	case 60000:
		return 4
	case 600000:
		return 5
	}
	return 2
}

// Analysis jobs.
func (c *browserClient) startJob(q browserControl) error {
	var fields map[string]any
	if err := json.Unmarshal(q.Raw, &fields); err != nil {
		return err
	}
	id, _ := fields["job_id"].(string)
	if q.Type == "analysis_compute_cancel" {
		if cancel := c.jobs[id]; cancel != nil {
			cancel()
		}
		return nil
	}
	if id == "" {
		random := make([]byte, 16)
		_, _ = rand.Read(random)
		id = hex.EncodeToString(random)
	}
	if len(c.jobs) >= 2 {
		return fmt.Errorf("two jobs per client maximum")
	}
	if _, exists := c.jobs[id]; exists {
		return fmt.Errorf("job_id is already active")
	}
	if fields["session_id"] == nil || fields["session_id"] == "" {
		fields["session_id"] = c.session
	}
	if fields["decode_id"] == nil || fields["decode_id"] == "" {
		fields["decode_id"] = c.decode
	}
	ctx, cancel := context.WithTimeout(c.ctx, 120*time.Second)
	c.jobs[id] = cancel
	owner := fmt.Sprintf("%p", c)
	go func() {
		defer cancel()
		select {
		case c.service.requests <- struct{}{}:
		case <-ctx.Done():
			return
		}
		defer func() { <-c.service.requests }()
		result, err := c.service.analysisJob(ctx, owner, id, fields)
		if err != nil {
			result = map[string]any{"type": "analysis_compute_result", "job_id": id, "status": "failed", "code": "compute_error", "message": err.Error()}
		}
		select {
		case c.out <- browserOutput{text: result, completedJob: id}:
		case <-c.ctx.Done():
		}
	}()
	return nil
}
func (b *browserService) analysisJob(ctx context.Context, owner, id string, fields map[string]any) (any, error) {
	base := os.Getenv("PHOTON_ANALYSIS_URL")
	if base == "" {
		base = "http://127.0.0.1:18765"
	}
	u, err := url.Parse(base)
	if err != nil || u.Hostname() != "127.0.0.1" && u.Hostname() != "localhost" {
		return nil, fmt.Errorf("analysis sidecar must be on loopback")
	}
	scratch, err := os.MkdirTemp("", "photon-analysis-compute-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(scratch)
	bundle, err := b.analysisBundle(ctx, fields)
	if err != nil {
		return nil, err
	}
	data, err := json.Marshal(bundle)
	if err != nil {
		return nil, err
	}
	path := filepath.Join(scratch, "bundle.json")
	if err = os.WriteFile(path, data, 0600); err != nil {
		return nil, err
	}
	recipe, _ := fields["recipe_id"].(string)
	if recipe == "" {
		recipe = "summarize_window"
	}
	request := map[string]any{"jobId": id, "clientId": owner, "bundlePath": path, "recipe": recipe, "timeoutS": 120, "sessionId": fields["session_id"], "t0": fields["t0_ms"], "t1": fields["t1_ms"], "prompt": fields["prompt"]}
	client := &http.Client{Timeout: 10 * time.Second}
	call := func(callCtx context.Context, method, path string, body any) (map[string]any, error) {
		var data []byte
		if body != nil {
			data, _ = json.Marshal(body)
		}
		req, err := http.NewRequestWithContext(callCtx, method, strings.TrimRight(base, "/")+path, bytes.NewReader(data))
		if err != nil {
			return nil, err
		}
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("X-Client-Id", owner)
		resp, err := client.Do(req)
		if err != nil {
			return nil, err
		}
		defer resp.Body.Close()
		var result map[string]any
		if err = json.NewDecoder(io.LimitReader(resp.Body, 1024*1024)).Decode(&result); err != nil {
			return nil, err
		}
		if resp.StatusCode >= 300 {
			return nil, fmt.Errorf("sidecar HTTP %d", resp.StatusCode)
		}
		return result, nil
	}
	if _, err = call(ctx, "POST", "/jobs", request); err != nil {
		return nil, err
	}
	completed := false
	defer func() {
		if completed {
			return
		}
		cancelCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_, _ = call(cancelCtx, "POST", "/jobs/"+url.PathEscape(id)+"/cancel", map[string]any{"clientId": owner})
	}()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-ticker.C:
		}
		view, err := call(ctx, "GET", "/jobs/"+url.PathEscape(id), nil)
		if err != nil {
			return nil, err
		}
		status, _ := view["status"].(string)
		if status == "succeeded" || status == "failed" || status == "cancelled" {
			completed = true
			view["type"] = "analysis_compute_result"
			view["job_id"] = id
			normalizeAnalysisResult(view)
			return view, nil
		}
	}
}

// Jobs receive bounded summaries from the same immutable decoding as the UI.
func (b *browserService) analysisBundle(ctx context.Context, fields map[string]any) (map[string]any, error) {
	var q struct {
		Session   string   `json:"session_id"`
		Decode    string   `json:"decode_id"`
		Signals   []uint32 `json:"signal_ids"`
		From      uint64   `json:"t0_ms"`
		To        uint64   `json:"t1_ms"`
		MaxPoints int      `json:"max_points"`
	}
	raw, err := json.Marshal(fields)
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(raw, &q); err != nil {
		return nil, err
	}
	if len(q.Signals) == 0 || len(q.Signals) > 64 {
		return nil, fmt.Errorf("analysis requires 1..64 signals")
	}
	if q.To <= q.From {
		return nil, fmt.Errorf("analysis requires an increasing time range")
	}
	s := b.recordings.Store(q.Session)
	if s == nil {
		return nil, fmt.Errorf("unknown recording")
	}
	meta := s.Snapshot()
	if q.Decode == "" {
		q.Decode = meta.DefaultDecode
	}
	var defs *cache.SessionCache
	for _, d := range meta.Decodes {
		if d.ID == q.Decode {
			if dbc, ok := b.recordings.registry.Get(d.DBCHash); ok {
				defs = dbc.Defs
			}
		}
	}
	if defs == nil {
		return nil, fmt.Errorf("unknown decoding")
	}
	lookup := make(map[uint32]cache.Series)
	for _, series := range defs.Series() {
		lookup[series.ID] = series
	}
	limit := q.MaxPoints
	if limit <= 0 {
		limit = 2000
	}
	limit = min(limit, 2000, 16000/len(q.Signals))
	signals := make([]any, 0, len(q.Signals))
	series := make([]any, 0, len(q.Signals))
	for _, id := range q.Signals {
		def, ok := lookup[id]
		if !ok {
			return nil, fmt.Errorf("unknown signal %d", id)
		}
		tap := def.Tap
		if tap == nil {
			scalar := int32(-1)
			tap = &scalar
		}
		rows, err := b.recordings.Query(ctx, influx.Query{Session: q.Session, DecodeID: q.Decode, Signal: def.Def.Message + "." + def.Def.ID.Name, Tap: tap, From: q.From, To: q.To, Limit: limit})
		if err != nil {
			return nil, err
		}
		points := make([]any, 0, len(rows))
		for _, r := range rows {
			points = append(points, map[string]any{"t": r.TimeMS, "v": r.EngineeringMean, "min": r.EngineeringMin, "max": r.EngineeringMax, "count": r.Count, "bucketMs": r.BucketMS, "approximate": r.Approximate})
		}
		name := def.Def.Message + "." + def.Name
		signals = append(signals, map[string]any{"id": id, "name": name})
		series = append(series, map[string]any{"name": name, "unit": def.Def.Unit, "points": points, "truncated": len(rows) == limit})
	}
	return map[string]any{"sessionId": q.Session, "decodeId": q.Decode, "signalIds": q.Signals, "signals": signals, "series": series, "t0Ms": q.From, "t1Ms": q.To, "prompt": fields["prompt"], "recipeId": fields["recipe_id"], "threshold": fields["threshold"], "summaries": true}, nil
}

func normalizeAnalysisResult(view map[string]any) {
	for _, field := range []struct{ group, from, to string }{{"derived", "pointsRef", "points_ref"}, {"events", "tMs", "t_ms"}} {
		if rows, ok := view[field.group].([]any); ok {
			for _, item := range rows {
				if row, ok := item.(map[string]any); ok {
					if value, ok := row[field.from]; ok {
						row[field.to] = value
					}
				}
			}
		}
	}
}
