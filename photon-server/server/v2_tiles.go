package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"

	"server/dbc"
)

// tileLevelMS is the bucket size of each tile level, in ms. Level 0 is the raw
// samples. hello_ack sends it, so the client has the same tile geometry.
var tileLevelMS = []int{0, 100, 1000, 10000, 60000, 600000}

const (
	// tileRawSpanMS is how much time a level 0 tile covers.
	tileRawSpanMS = 60000
	// tileBuckets is how many buckets a tile above level 0 covers.
	tileBuckets = 1024
	// tileSealLagMS is how long before now a tile must end to be sealed: InfluxDB's
	// write delay, with a margin.
	tileSealLagMS = 2000
	// tileRawRows caps the raw samples of one level 0 tile: 60 s at 3.3 kHz, far
	// above any one signal's rate. A tile that reaches it is an error, not data.
	tileRawRows = 200000
	// v2MaxTileRequests is how many tile requests one client may have in flight.
	v2MaxTileRequests = 32
)

// tileSpanMS is how much time one tile of level covers, in ms.
func tileSpanMS(level int) int64 {
	if level == 0 {
		return tileRawSpanMS
	}
	return int64(tileLevelMS[level]) * tileBuckets
}

// tileMaxIndex is the first tile index whose end, in ns, would not fit an int64.
func tileMaxIndex(level int) int64 {
	return math.MaxInt64 / (tileSpanMS(level) * nsPerMs)
}

// tileSignal is one catalog signal and the message it belongs to.
type tileSignal struct {
	msg *dbc.Message
	sig *dbc.Signal
}

// tileJob is a tile request that has passed its checks.
type tileJob struct {
	qid    uint32
	run    string
	ts     tileSignal
	sha    string // the catalog the signal index belongs to
	signal int64  // the signal's catalog index
	level  int
	index  int64
}

// tileRequest is a tile request in flight, with what it needs to be cancelled. A
// request is told apart by its pointer, so a finished request cannot remove a
// later one that reuses its qid.
type tileRequest struct {
	cancel context.CancelFunc
}

// v2TileError answers a tile request that failed.
type v2TileError struct {
	Type    string `json:"type"`
	QID     uint32 `json:"qid"`
	Message string `json:"message"`
}

// v2Run is one session in the runs list.
type v2Run struct {
	ID      string `json:"id"`
	Label   string `json:"label"`
	Source  string `json:"source"`
	Device  string `json:"device"`
	DBCSHA  string `json:"dbc_sha"`
	StartMS int64  `json:"start_ms"`
	EndMS   int64  `json:"end_ms"`
	Live    bool   `json:"live"`
}

// v2RunsMsg is the reply to runs_list.
type v2RunsMsg struct {
	Type string  `json:"type"`
	Runs []v2Run `json:"runs"`
}

// sessionQuery lists the sessions, newest first.
const sessionQuery = `SELECT id, dbc_label, dbc_sha, start, "end" FROM session ORDER BY start DESC`

// listRuns returns the reply to runs_list: every session InfluxDB has, newest
// first. It is an error reply when the query fails.
func (h *v2Hub) listRuns(ctx context.Context) []byte {
	runs := []v2Run{}
	if h.querier != nil {
		rows, err := h.querier.query(ctx, sessionQuery, nil)
		if err != nil && !errors.Is(err, errNoData) {
			return encodeV2(v2Error{Type: "error", Code: "runs", Message: err.Error()})
		}
		live := h.liveRun()
		for _, row := range rows {
			r, err := sessionRun(row, live)
			if err != nil {
				return encodeV2(v2Error{Type: "error", Code: "runs", Message: err.Error()})
			}
			runs = append(runs, r)
		}
	}
	return encodeV2(v2RunsMsg{Type: "runs", Runs: runs})
}

// sessionRun reads one row of the session table. For now the label is the ID, and
// the car is the only source.
func sessionRun(row map[string]any, live string) (v2Run, error) {
	var r v2Run
	var err error
	if r.ID, err = rowString(row, "id"); err != nil {
		return v2Run{}, err
	}
	if r.DBCSHA, err = rowString(row, "dbc_sha"); err != nil {
		return v2Run{}, err
	}
	if r.StartMS, err = rowInt(row, "start"); err != nil {
		return v2Run{}, err
	}
	if r.EndMS, err = rowInt(row, "end"); err != nil {
		return v2Run{}, err
	}
	r.Label, r.Source, r.Live = r.ID, "car", r.ID == live
	return r, nil
}

// tileTarget checks a tile request against the catalog, and returns the signal
// and the sha of the catalog it came from.
func (h *v2Hub) tileTarget(signal int64, level int, index int64) (tileSignal, string, error) {
	if level < 0 || level >= len(tileLevelMS) {
		return tileSignal{}, "", fmt.Errorf("level %d is not in 0 to %d", level, len(tileLevelMS)-1)
	}
	if index < 0 || index >= tileMaxIndex(level) {
		return tileSignal{}, "", fmt.Errorf("tile index %d is out of range", index)
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.cat == nil {
		return tileSignal{}, "", errors.New("no catalog")
	}
	if signal < 0 || signal >= int64(len(h.signals)) {
		return tileSignal{}, "", fmt.Errorf("unknown signal %d", signal)
	}
	return h.signals[signal], h.catalogSHA, nil
}

// fetchTile reads the samples of one tile from InfluxDB. A tile with nothing
// written in its window has no samples.
func (h *v2Hub) fetchTile(ctx context.Context, ts tileSignal, run string, level int, index int64) (*tileData, error) {
	spanNS := tileSpanMS(level) * nsPerMs
	fromNS := index * spanNS
	sql, params := tileSQL(ts, run, level, fromNS, fromNS+spanNS)
	t := &tileData{level: uint8(level), index: index}
	rows, err := h.querier.query(ctx, sql, params)
	if errors.Is(err, errNoData) {
		return t, nil
	}
	if err != nil {
		return nil, err
	}
	if level == 0 && len(rows) >= tileRawRows {
		// The LIMIT cut the tile short; an incomplete tile must not pass (or be
		// cached) as a whole one. The client asks for a coarser level instead.
		return nil, fmt.Errorf("tile has more than %d raw samples; use a coarser level", tileRawRows)
	}
	cols, dst := []string{"v"}, []*[]float64{&t.raw}
	if level > 0 {
		cols, dst = []string{"lo", "hi", "mean"}, []*[]float64{&t.lo, &t.hi, &t.mean}
	}
	for _, row := range rows {
		ns, err := rowInt(row, "t_ns")
		if err != nil {
			return nil, err
		}
		t.tMS = append(t.tMS, nsToMS(ns))
		for i, col := range cols {
			v, err := rowFloat(row, col)
			if err != nil {
				return nil, err
			}
			*dst[i] = append(*dst[i], v)
		}
	}
	return t, nil
}

// tileSQL returns the query for the samples of one tile of ts in run, and its
// parameters. Level 0 gives each raw sample. A higher level gives one row per
// bucket, with its min, max and mean. The time bounds are literals, since
// parameters cannot carry them.
func tileSQL(ts tileSignal, run string, level int, fromNS, toNS int64) (string, map[string]string) {
	col := `"` + strings.ReplaceAll(ts.sig.Name, `"`, `""`) + `"`
	params := map[string]string{"run": run, "msg": ts.msg.Name}
	where := "session = $run AND msg = $msg"
	if ts.sig.Mux >= 0 {
		where += " AND tap = $tap"
		params["tap"] = strconv.Itoa(int(ts.sig.Mux))
	}
	where += fmt.Sprintf(" AND time >= to_timestamp_nanos(%d) AND time < to_timestamp_nanos(%d) AND %s IS NOT NULL",
		fromNS, toNS, col)
	if level == 0 {
		return fmt.Sprintf("SELECT CAST(time AS BIGINT) AS t_ns, %s AS v FROM frame WHERE %s ORDER BY time LIMIT %d",
			col, where, tileRawRows), params
	}
	rollup := `SELECT CAST(date_bin(INTERVAL '%[1]d milliseconds', time, TIMESTAMP '1970-01-01T00:00:00Z') AS BIGINT) AS t_ns, ` +
		`min(%[2]s) AS lo, max(%[2]s) AS hi, avg(CAST(%[2]s AS DOUBLE)) AS mean FROM frame WHERE %[3]s GROUP BY 1 ORDER BY 1`
	return fmt.Sprintf(rollup, tileLevelMS[level], col, where), params
}

// nsToMS is a Unix time in ns, as ms. Whole ms are exact; the rest is a fraction.
func nsToMS(ns int64) float64 {
	return float64(ns/nsPerMs) + float64(ns%nsPerMs)/nsPerMs
}

// requestContext returns a context for one request, which ends when the client is
// done. The caller calls cancel when the request finishes.
func (c *v2Client) requestContext() (context.Context, context.CancelFunc) {
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		select {
		case <-c.done:
			cancel()
		case <-ctx.Done():
		}
	}()
	return ctx, cancel
}

// startRuns answers a runs_list in its own goroutine, so InfluxDB does not hold up
// the reader.
func (c *v2Client) startRuns() {
	ctx, cancel := c.requestContext()
	go func() {
		defer cancel()
		reply := c.hub.listRuns(ctx)
		if ctx.Err() == nil {
			c.enqueue(reply)
		}
	}()
}

// startTile handles a tile_request. The request is checked here. A valid one is
// answered in its own goroutine, so the reader does not wait for InfluxDB.
func (c *v2Client) startTile(data []byte) {
	var req struct {
		QID    uint32 `json:"qid"`
		Run    string `json:"run"`
		Signal int64  `json:"signal"`
		Level  int    `json:"level"`
		Index  int64  `json:"index"`
	}
	if err := json.Unmarshal(data, &req); err != nil {
		c.enqueueError("bad_tile_request", "tile_request needs qid, run, signal, level and index")
		return
	}
	h := c.hub
	if h.querier == nil {
		c.tileError(req.QID, "history is off")
		return
	}
	ts, sha, err := h.tileTarget(req.Signal, req.Level, req.Index)
	if err != nil {
		c.tileError(req.QID, err.Error())
		return
	}
	ctx, cancel := c.requestContext()
	me := &tileRequest{cancel: cancel}
	if why := c.addTile(req.QID, me); why != "" {
		cancel()
		c.tileError(req.QID, why)
		return
	}
	job := tileJob{qid: req.QID, run: req.Run, ts: ts, sha: sha, signal: req.Signal, level: req.Level, index: req.Index}
	go c.serveTile(ctx, me, job)
}

// addTile records a request as in flight. It returns why the request is refused,
// or "" when it is accepted.
func (c *v2Client) addTile(qid uint32, me *tileRequest) string {
	c.tileMu.Lock()
	defer c.tileMu.Unlock()
	if _, busy := c.tiles[qid]; busy {
		return fmt.Sprintf("qid %d is already in use", qid)
	}
	if len(c.tiles) >= v2MaxTileRequests {
		return "too many tile requests"
	}
	c.tiles[qid] = me
	return ""
}

// finishTile forgets a request once it is answered or cancelled.
func (c *v2Client) finishTile(qid uint32, me *tileRequest) {
	c.tileMu.Lock()
	defer c.tileMu.Unlock()
	if c.tiles[qid] == me {
		delete(c.tiles, qid)
	}
}

// cancelTile handles a tile_cancel: the request with that qid is cancelled, and
// sends nothing more. An unknown qid is ignored.
func (c *v2Client) cancelTile(data []byte) {
	var req struct {
		QID uint32 `json:"qid"`
	}
	if err := json.Unmarshal(data, &req); err != nil {
		c.enqueueError("bad_tile_cancel", "tile_cancel needs a qid")
		return
	}
	c.tileMu.Lock()
	me := c.tiles[req.QID]
	delete(c.tiles, req.QID)
	c.tileMu.Unlock()
	if me != nil {
		me.cancel()
	}
}

// tileError queues a tile_error for qid.
func (c *v2Client) tileError(qid uint32, msg string) {
	c.enqueue(encodeV2(v2TileError{Type: "tile_error", QID: qid, Message: msg}))
}

// serveTile answers one checked tile request, and sends nothing once ctx ends. A
// sealed tile of the live run is kept in the hub's cache; any other tile is read
// from InfluxDB every time.
func (c *v2Client) serveTile(ctx context.Context, me *tileRequest, job tileJob) {
	defer me.cancel()
	h := c.hub
	// A request leaves the in-flight table just before its last message is queued,
	// so a client that reads that message may reuse the qid at once.
	finish := func() { c.finishTile(job.qid, me) }
	defer finish()
	live := h.liveRun()
	nowMS := time.Now().UnixMilli()
	endMS := (job.index + 1) * tileSpanMS(job.level)
	sealed := job.run != live || endMS < nowMS-tileSealLagMS
	cacheable := sealed && job.run != "" && job.run == live
	key := tileKey{run: job.run, sha: job.sha, signal: uint32(job.signal), level: uint8(job.level), index: job.index}

	if cacheable {
		if msgs, ok := h.tiles.get(live, key); ok {
			h.tilesServed.Add(1)
			c.sendCached(ctx, job.qid, msgs, finish)
			return
		}
	}
	t, err := h.fetchTile(ctx, job.ts, job.run, job.level, job.index)
	if err != nil {
		if ctx.Err() == nil {
			finish()
			c.tileError(job.qid, err.Error())
		}
		return
	}
	msgs := encodeTile(job.qid, uint32(job.signal), sealed, t)
	if cacheable {
		// The cache keeps its own copies: the writer stamps seq into the messages it sends.
		h.tiles.put(live, key, endMS, cloneMessages(msgs), nowMS)
	}
	h.tilesServed.Add(1)
	c.sendPieces(ctx, msgs, finish)
}

// sendPieces queues the pieces of a tile, each through the live queue, so live
// frames can go between them. beforeLast runs just before the last piece is queued.
func (c *v2Client) sendPieces(ctx context.Context, msgs [][]byte, beforeLast func()) {
	for i, m := range msgs {
		if i == len(msgs)-1 {
			beforeLast()
		}
		if !c.enqueueTile(ctx, outMsg{typ: wsBinary, data: m}) {
			return
		}
	}
}

// sendCached queues cached pieces with the request's qid in them. Each piece is
// copied first, so the cached one is never changed. beforeLast runs just before
// the last piece is queued.
func (c *v2Client) sendCached(ctx context.Context, qid uint32, msgs [][]byte, beforeLast func()) {
	for i, m := range msgs {
		if i == len(msgs)-1 {
			beforeLast()
		}
		out := append([]byte(nil), m...)
		binary.LittleEndian.PutUint32(out[v2HeaderSize:], qid)
		if !c.enqueueTile(ctx, outMsg{typ: wsBinary, data: out}) {
			return
		}
	}
}

// enqueueTile queues one Tile piece on the live queue, waiting for room. It returns
// false, without queueing, once ctx ends or the client is done.
func (c *v2Client) enqueueTile(ctx context.Context, m outMsg) bool {
	if ctx.Err() != nil {
		return false
	}
	select {
	case c.live <- m:
		return true
	case <-ctx.Done():
		return false
	case <-c.done:
		return false
	}
}

// cloneMessages returns a copy of each message.
func cloneMessages(msgs [][]byte) [][]byte {
	out := make([][]byte, len(msgs))
	for i, m := range msgs {
		out[i] = append([]byte(nil), m...)
	}
	return out
}
