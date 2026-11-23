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
var tileLevelMS = []int{0, 10, 100, 1000, 10000, 60000, 600000}

const (
	// tileRawSpanMS is how much time a level 0 tile covers.
	tileRawSpanMS = 10000
	// tileBuckets is how many buckets a tile above level 0 covers.
	tileBuckets = 1024
	// tileSealLagMS is how long before now a tile must end to be sealed: InfluxDB's
	// write delay, with a margin.
	tileSealLagMS = 2000
	// tileRawRows caps the raw samples of one level 0 tile: 10 s at 20 kHz, far above
	// any one signal's rate. A tile that reaches it is an error, not data.
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

// v2Run is one session in the runs list. Rev rises each time the run's history
// is rewritten by a backfill, so a client can tell its cached tiles are stale.
type v2Run struct {
	ID      string `json:"id"`
	Label   string `json:"label"`
	Source  string `json:"source"`
	Device  string `json:"device"`
	DBCSHA  string `json:"dbc_sha"`
	StartMS int64  `json:"start_ms"`
	EndMS   int64  `json:"end_ms"`
	Rev     int64  `json:"rev"`
	Live    bool   `json:"live"`
}

// v2RunsMsg is the reply to runs_list.
type v2RunsMsg struct {
	Type string  `json:"type"`
	Runs []v2Run `json:"runs"`
}

// sessionQuery lists the sessions, newest first, with their revs.
const sessionQuery = `SELECT id, dbc_label, dbc_sha, start, "end", rev FROM session ORDER BY start DESC`

// sessionQueryNoRev is sessionQuery for a database whose session rows were all
// written before rev existed. Every run then has rev 0.
const sessionQueryNoRev = `SELECT id, dbc_label, dbc_sha, start, "end" FROM session ORDER BY start DESC`

// listRuns returns the reply to runs_list: every session InfluxDB has, newest
// first. It is an error reply when the query fails.
func (h *v2Hub) listRuns(ctx context.Context) []byte {
	runs := []v2Run{}
	if h.querier != nil {
		rows, err := h.querier.query(ctx, sessionQuery, nil)
		if errors.Is(err, errNoData) {
			// A rev column that does not exist yet reads as no data, so ask again
			// without it. If the session table is missing too, this is no data as well.
			rows, err = h.querier.query(ctx, sessionQueryNoRev, nil)
		}
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
	// A session written before rev existed has no rev, or a null one when other
	// sessions have it. Such a run has rev 0.
	if row["rev"] != nil {
		if r.Rev, err = rowInt(row, "rev"); err != nil {
			return v2Run{}, err
		}
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

// tileSend hands one encoded piece of a tile on. last is set for the tile's last
// piece. It returns false when the piece is not sent, which ends the tile.
type tileSend func(piece []byte, last bool) bool

// errTileStopped means a piece was not sent: the request was cancelled, or the
// client is gone.
var errTileStopped = errors.New("tile request stopped")

// streamTile reads the samples of the tile of job from InfluxDB, and hands its
// pieces to send as they fill. A tile with nothing written in its window is one
// empty piece. It returns errTileStopped when send refuses a piece.
func (h *v2Hub) streamTile(ctx context.Context, job tileJob, sealed bool, send tileSend) error {
	spanNS := tileSpanMS(job.level) * nsPerMs
	fromNS := job.index * spanNS
	sql, params := tileSQL(job.ts, job.run, job.level, fromNS, fromNS+spanNS)
	b := newTileBuilder(job, sealed, send)
	err := h.querier.stream(ctx, sql, params, tileColumns(job.level), b.addRow)
	if err != nil && !errors.Is(err, errNoData) {
		return err
	}
	return b.end()
}

// tileBuilder turns the rows of one tile into its Tile pieces. It holds the samples
// of one piece at a time, and sends the piece as soon as it is full.
type tileBuilder struct {
	qid, signal uint32
	sig         *dbc.Signal // the signal, whose values are scaled to its unit
	level       int
	sealed      bool
	perPiece    int        // samples in a full piece
	cur         tileData   // the samples of the piece being filled; its slices are reused
	off         int        // index in the tile of the first sample of cur
	rows        int        // rows read from InfluxDB, NULL rows included
	scratch     [3]float64 // the values of one row
	send        tileSend
}

// newTileBuilder returns a builder for the tile of job. Its pieces are sealed when
// sealed is set.
func newTileBuilder(job tileJob, sealed bool, send tileSend) *tileBuilder {
	n := tilePieceSamples(job.level)
	b := &tileBuilder{
		qid:      job.qid,
		signal:   uint32(job.signal),
		sig:      job.ts.sig,
		level:    job.level,
		sealed:   sealed,
		perPiece: n,
		cur:      tileData{level: uint8(job.level), index: job.index, tMS: make([]float64, 0, n)},
		send:     send,
	}
	if job.level == 0 {
		b.cur.raw = make([]float64, 0, n)
	} else {
		b.cur.lo = make([]float64, 0, n)
		b.cur.hi = make([]float64, 0, n)
		b.cur.mean = make([]float64, 0, n)
	}
	return b
}

// addRow reads one row of the reply: its time, then its values, in the order of
// tileColumns. A row with a NULL value, or a value that is not finite, is no sample.
func (b *tileBuilder) addRow(row []string) error {
	b.rows++
	if b.level == 0 && b.rows >= tileRawRows {
		// The LIMIT cut the tile short, so an incomplete tile must not pass (or be
		// cached) as a whole one. The client asks for a coarser level instead.
		return fmt.Errorf("tile has more than %d raw samples; use a coarser level", tileRawRows)
	}
	ns, err := strconv.ParseInt(row[0], 10, 64)
	if err != nil {
		return fmt.Errorf("InfluxDB row has a bad time %q", row[0])
	}
	vals := b.scratch[:len(row)-1]
	for i, s := range row[1:] {
		if s == "" {
			return nil // NULL: no sample at this time
		}
		if vals[i], err = strconv.ParseFloat(s, 64); err != nil {
			return fmt.Errorf("InfluxDB row has a bad value %q", s)
		}
	}
	toPhysical(b.sig, b.level, vals)
	for _, v := range vals {
		if !finite(v) {
			return nil
		}
	}
	return b.add(nsToMS(ns), vals)
}

// add appends a sample to the piece being filled. A piece that becomes full is sent
// at once.
func (b *tileBuilder) add(tMS float64, vals []float64) error {
	b.cur.tMS = append(b.cur.tMS, tMS)
	if b.level == 0 {
		b.cur.raw = append(b.cur.raw, vals[0])
	} else {
		b.cur.lo = append(b.cur.lo, vals[0])
		b.cur.hi = append(b.cur.hi, vals[1])
		b.cur.mean = append(b.cur.mean, vals[2])
	}
	if len(b.cur.tMS) == b.perPiece {
		return b.flush(false)
	}
	return nil
}

// end sends the samples still held as the tile's last piece. The piece may be empty.
func (b *tileBuilder) end() error {
	return b.flush(true)
}

// flush sends the samples held as one piece, with the last flag when last is set,
// and then empties the buffers for the next piece.
func (b *tileBuilder) flush(last bool) error {
	flags := byte(0)
	if b.sealed {
		flags |= v2TileSealed
	}
	if last {
		flags |= v2TileLast
	}
	n := len(b.cur.tMS)
	if !b.send(encodeTilePiece(b.qid, b.signal, flags, uint32(b.off), &b.cur), last) {
		return errTileStopped
	}
	b.off += n
	b.cur.tMS, b.cur.raw = b.cur.tMS[:0], b.cur.raw[:0]
	b.cur.lo, b.cur.hi, b.cur.mean = b.cur.lo[:0], b.cur.hi[:0], b.cur.mean[:0]
	return nil
}

// toPhysical scales the values of one row from raw to the signal's unit, as
// dbc.Signal.Value does: v*factor + offset. vals is the raw value of a level 0 row,
// or the raw min, max and mean of a bucket. With a negative factor the scaled min
// and max trade places, so min is still the lower one.
func toPhysical(sig *dbc.Signal, level int, vals []float64) {
	for i := range vals {
		vals[i] = vals[i]*sig.Factor + sig.Offset
	}
	if level > 0 && sig.Factor < 0 {
		vals[0], vals[1] = vals[1], vals[0]
	}
}

// tileColumns is the names of the columns of a tile query's reply, in order.
func tileColumns(level int) []string {
	if level == 0 {
		return []string{"t_ns", "v"}
	}
	return []string{"t_ns", "lo", "hi", "mean"}
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

// tileError queues a tile_error for qid on the control queue. It is for a request
// that has no pieces in flight.
func (c *v2Client) tileError(qid uint32, msg string) {
	c.enqueue(tileErrorMessage(qid, msg))
}

// tileErrorMessage returns the tile_error for qid.
func tileErrorMessage(qid uint32, msg string) []byte {
	return encodeV2(v2TileError{Type: "tile_error", QID: qid, Message: msg})
}

// serveTile answers one checked tile request, and sends nothing once ctx ends. The
// tile is read from InfluxDB and sent piece by piece, as its rows arrive. A sealed
// tile of the live run is kept in the hub's cache once it has been read whole; any
// other tile is read from InfluxDB every time.
func (c *v2Client) serveTile(ctx context.Context, me *tileRequest, job tileJob) {
	defer me.cancel()
	h := c.hub
	// A request leaves the in-flight table just before its last piece is queued, so
	// a client that reads that piece may reuse the qid at once.
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

	// kept holds the pieces as they are sent, for the cache. A tile that would not
	// fit in the cache is not kept, so a request never holds more than the cache could.
	var kept [][]byte
	keptBytes := 0
	stalled := false // the client took no piece for tileSendStall
	send := func(piece []byte, last bool) bool {
		if cacheable {
			if keptBytes+len(piece) > tileCacheMax {
				cacheable, kept = false, nil
			} else {
				keptBytes += len(piece)
				// The cache keeps its own copies: the writer stamps seq into the piece it sends.
				kept = append(kept, append([]byte(nil), piece...))
			}
		}
		if last {
			// The tile has been read whole, so it is cached before its last piece is
			// queued: a client that reads that piece can ask for the tile at once.
			if cacheable {
				h.tiles.put(live, key, endMS, kept, nowMS)
			}
			h.tilesServed.Add(1)
			finish()
		}
		ok, timedOut := c.enqueueTileWithin(ctx, outMsg{typ: wsBinary, data: piece}, tileSendStall)
		stalled = timedOut
		return ok
	}
	err := h.streamTile(ctx, job, sealed, send)
	if stalled {
		// The query held an InfluxDB slot while the client read nothing; it was
		// stopped so other requests can run. The error skips the full live queue.
		finish()
		h.tileStalls.Add(1)
		c.tileError(job.qid, "tile stopped: the client did not read it")
		return
	}
	if err == nil || errors.Is(err, errTileStopped) || ctx.Err() != nil {
		return
	}
	// The read failed, and some pieces may already be queued. The error goes through
	// the live queue, behind them, so the client has read those pieces before it
	// reads the error and drops them.
	finish()
	_ = c.enqueueTile(ctx, outMsg{typ: wsText, data: tileErrorMessage(job.qid, err.Error())})
}

// sendCached queues cached pieces with the request's qid in them. Each piece is
// copied first, so the cached one is never changed. beforeLast runs just before the
// last piece is queued.
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

// tileSendStall is how long a streaming tile waits for room in the client's live
// queue. The query holds one of the few InfluxDB slots meanwhile, so a client that
// stops reading must not keep it for the whole query timeout.
var tileSendStall = 5 * time.Second

// enqueueTileWithin is enqueueTile that gives up after wait; timedOut reports that.
func (c *v2Client) enqueueTileWithin(ctx context.Context, m outMsg, wait time.Duration) (ok, timedOut bool) {
	if ctx.Err() != nil {
		return false, false
	}
	t := time.NewTimer(wait)
	defer t.Stop()
	select {
	case c.live <- m:
		return true, false
	case <-ctx.Done():
		return false, false
	case <-c.done:
		return false, false
	case <-t.C:
		return false, true
	}
}

// enqueueTile queues one message on the live queue, waiting for room. It returns
// false, without queueing, once ctx ends or the client is done. A tile piece goes
// this way so that a cancelled request sends nothing more once it has ended.
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
