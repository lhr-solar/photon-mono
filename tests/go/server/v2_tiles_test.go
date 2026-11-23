package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"server/dbc"
)

// tileTestRun is the live run of the tile tests.
const tileTestRun = "run-test"

// newTileHub returns a LAN hub on the test catalog, with history from fake, and
// live as its live run.
func newTileHub(t *testing.T, fake *fakeQueries, live func() string) *v2Hub {
	t.Helper()
	return newTileHubOn(t, fake, live, loadTestCatalog(t))
}

// newTileHubOn is newTileHub on the catalog cat.
func newTileHubOn(t *testing.T, fake *fakeQueries, live func() string, cat *dbc.Catalog) *v2Hub {
	t.Helper()
	return newTileHubURL(t, newFakeQueryServer(t, fake), live, cat)
}

// newTileHubURL is newTileHubOn with history from the InfluxDB at url.
func newTileHubURL(t *testing.T, url string, live func() string, cat *dbc.Catalog) *v2Hub {
	t.Helper()
	q := newInfluxQuerier(url, "photon_test", "", 6)
	h := newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), live, newLiveRing(liveRingMinFrames), q)
	h.SetCatalog(cat)
	return h
}

// fixedLive returns a live-run function that always gives id.
func fixedLive(id string) func() string { return func() string { return id } }

// tileConn dials h and completes hello, reading the hello_ack and the catalog.
func tileConn(t *testing.T, h *v2Hub) *wsConn {
	t.Helper()
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)
	readMsg(t, conn) // the catalog, sent after hello_ack
	return conn
}

// requestTile sends a tile_request.
func requestTile(t *testing.T, conn *wsConn, qid uint32, run string, signal int64, level int, index int64) {
	t.Helper()
	sendV2(t, conn, map[string]any{"type": "tile_request", "qid": qid, "run": run, "signal": signal, "level": level, "index": index})
}

// readTile reads the pieces that answer qid, up to the one with the last flag. A
// tile_error for qid fails the test.
func readTile(t *testing.T, conn *wsConn, qid uint32) []v2Tile {
	t.Helper()
	pieces, msg := readTileReply(t, conn, qid)
	if msg != "" {
		t.Fatalf("tile %d failed: %s", qid, msg)
	}
	return pieces
}

// readTileReply reads the messages that answer qid, up to the piece with the last
// flag or a tile_error. It returns the pieces read, and the message of the tile_error
// when there is one.
func readTileReply(t *testing.T, conn *wsConn, qid uint32) ([]v2Tile, string) {
	t.Helper()
	var pieces []v2Tile
	for {
		typ, data := readAny(t, conn)
		if typ == wsText {
			var e struct {
				Type    string `json:"type"`
				QID     uint32 `json:"qid"`
				Message string `json:"message"`
			}
			if err := json.Unmarshal(data, &e); err != nil || e.Type != "tile_error" || e.QID != qid {
				t.Fatalf("got text %s while reading tile %d", data, qid)
			}
			return pieces, e.Message
		}
		piece, err := decodeTile(data)
		if err != nil {
			t.Fatalf("decode tile: %v", err)
		}
		if piece.QID != qid {
			t.Fatalf("tile for qid %d while reading qid %d", piece.QID, qid)
		}
		pieces = append(pieces, piece)
		if piece.Last {
			return pieces, ""
		}
	}
}

// newPausingQueryServer serves one query. It writes head and flushes it, then waits
// until release is closed or the query's context ends; if released it writes tail.
// The returned channel is closed when the handler returns.
func newPausingQueryServer(t *testing.T, head, tail string, release <-chan struct{}) (string, <-chan struct{}) {
	t.Helper()
	returned := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer close(returned)
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, head)
		w.(http.Flusher).Flush()
		select {
		case <-release:
		case <-r.Context().Done():
			return
		}
		_, _ = io.WriteString(w, tail)
	}))
	t.Cleanup(srv.Close)
	return srv.URL, returned
}

// joinTile checks that the pieces come in order, with the last flag only on the
// last, and joins them into one tile.
func joinTile(t *testing.T, pieces []v2Tile) v2Tile {
	t.Helper()
	out := pieces[0]
	out.TMS, out.Raw, out.Min, out.Max, out.Mean = nil, nil, nil, nil, nil
	next := uint32(0)
	for i, p := range pieces {
		if p.Offset != next {
			t.Fatalf("piece %d starts at sample %d, want %d", i, p.Offset, next)
		}
		if p.Last != (i == len(pieces)-1) {
			t.Fatalf("piece %d has last=%v", i, p.Last)
		}
		if p.Sealed != pieces[0].Sealed {
			t.Fatalf("piece %d has sealed=%v, the first has %v", i, p.Sealed, pieces[0].Sealed)
		}
		next += p.N
		out.TMS = append(out.TMS, p.TMS...)
		out.Raw = append(out.Raw, p.Raw...)
		out.Min = append(out.Min, p.Min...)
		out.Max = append(out.Max, p.Max...)
		out.Mean = append(out.Mean, p.Mean...)
	}
	out.Offset, out.N, out.Last = 0, next, true
	return out
}

// readTileError reads the next message, which must be a tile_error for qid, and
// returns its message.
func readTileError(t *testing.T, conn *wsConn, qid uint32) string {
	t.Helper()
	var e struct {
		Type    string `json:"type"`
		QID     uint32 `json:"qid"`
		Message string `json:"message"`
	}
	readInto(t, conn, &e)
	if e.Type != "tile_error" || e.QID != qid {
		t.Fatalf("reply = %+v, want tile_error for qid %d", e, qid)
	}
	return e.Message
}

// plainSignal is an integer signal that is on every frame of its message.
func plainSignal(t *testing.T) (int64, tileSignal) {
	t.Helper()
	for i, ts := range catalogSignals(loadTestCatalog(t)) {
		if ts.sig.Mux < 0 && ts.msg.Selector < 0 && ts.sig.Kind == dbc.Integer {
			return int64(i), ts
		}
	}
	t.Fatal("the spec has no plain integer signal")
	return 0, tileSignal{}
}

// muxSignal is a signal on one page of a multiplexed message.
func muxSignal(t *testing.T) (int64, tileSignal) {
	t.Helper()
	for i, ts := range catalogSignals(loadTestCatalog(t)) {
		if ts.sig.Mux >= 0 && ts.msg.Selector >= 0 {
			return int64(i), ts
		}
	}
	t.Fatal("the spec has no multiplexed signal")
	return 0, tileSignal{}
}

// pastIndex is a tile of level that ended an hour ago, so it is sealed.
func pastIndex(level int) int64 {
	return (time.Now().UnixMilli() - 3600_000) / tileSpanMS(level)
}

// currentIndex is the tile of level that holds now, so it is not sealed.
func currentIndex(level int) int64 {
	return time.Now().UnixMilli() / tileSpanMS(level)
}

// tileBounds returns the Unix ns bounds of tile index of level.
func tileBounds(level int, index int64) (int64, int64) {
	span := tileSpanMS(level) * nsPerMs
	return index * span, index*span + span
}

// wantTileSQL is the query a tile of ts should send, written out in full.
func wantTileSQL(ts tileSignal, level int, fromNS, toNS int64) string {
	name := ts.sig.Name
	filter := "session = $run AND msg = $msg"
	if ts.sig.Mux >= 0 {
		filter += " AND tap = $tap"
	}
	filter += fmt.Sprintf(" AND time >= to_timestamp_nanos(%d) AND time < to_timestamp_nanos(%d) AND \"%s\" IS NOT NULL",
		fromNS, toNS, name)
	if level == 0 {
		return fmt.Sprintf(`SELECT CAST(time AS BIGINT) AS t_ns, "%s" AS v FROM frame WHERE %s ORDER BY time LIMIT 200000`,
			name, filter)
	}
	return fmt.Sprintf(`SELECT CAST(date_bin(INTERVAL '%d milliseconds', time, TIMESTAMP '1970-01-01T00:00:00Z') AS BIGINT) AS t_ns, `+
		`min("%[2]s") AS lo, max("%[2]s") AS hi, avg(CAST("%[2]s" AS DOUBLE)) AS mean FROM frame WHERE %[3]s GROUP BY 1 ORDER BY 1`,
		tileLevelMS[level], name, filter)
}

// rawRows is the CSV reply to a level 0 tile query of n rows: one sample per ms from
// baseNS, valued 0 to n-1.
func rawRows(n int, baseNS int64) string {
	return "t_ns,v\n" + rawLines(baseNS, 0, n)
}

// rawLines is the CSV rows of the level 0 samples from to to-1, one per ms from
// baseNS, each valued by its index.
func rawLines(baseNS int64, from, to int) string {
	var b strings.Builder
	for k := from; k < to; k++ {
		fmt.Fprintf(&b, "%d,%d\n", baseNS+int64(k)*nsPerMs, k)
	}
	return b.String()
}

// rollupReply is the CSV reply to a rollup tile query, with rows as its data lines.
func rollupReply(rows string) string {
	return "t_ns,lo,hi,mean\n" + rows
}

// runsReply is the runs reply as the tests read it.
type runsReply struct {
	Type string `json:"type"`
	Runs []struct {
		ID      string `json:"id"`
		Label   string `json:"label"`
		Source  string `json:"source"`
		Device  string `json:"device"`
		DBCSHA  string `json:"dbc_sha"`
		StartMS int64  `json:"start_ms"`
		EndMS   int64  `json:"end_ms"`
		Live    bool   `json:"live"`
	} `json:"runs"`
}

func TestRunsListMarksTheLiveRun(t *testing.T) {
	f := &fakeQueries{reply: func(_ context.Context, q fakeQuery) (int, string) {
		return http.StatusOK,
			`{"id":"run-old","dbc_label":"A+B","dbc_sha":"aaa","start":1000,"end":2000}` + "\n" +
				`{"id":"run-test","dbc_label":"A+B","dbc_sha":"bbb","start":3000,"end":3500}` + "\n"
	}}
	h := newTileHub(t, f, fixedLive(tileTestRun))
	conn := tileConn(t, h)
	sendV2(t, conn, map[string]any{"type": "runs_list"})
	var reply runsReply
	readInto(t, conn, &reply)

	if got := f.queries()[0].SQL; got != `SELECT id, dbc_label, dbc_sha, start, "end" FROM session ORDER BY start DESC` {
		t.Fatalf("runs query = %q", got)
	}
	if reply.Type != "runs" || len(reply.Runs) != 2 {
		t.Fatalf("reply = %+v, want two runs", reply)
	}
	old, live := reply.Runs[0], reply.Runs[1]
	if old.ID != "run-old" || old.Label != "run-old" || old.Source != "car" || old.Device != "" ||
		old.DBCSHA != "aaa" || old.StartMS != 1000 || old.EndMS != 2000 || old.Live {
		t.Fatalf("old run = %+v", old)
	}
	if live.ID != tileTestRun || !live.Live || live.DBCSHA != "bbb" || live.StartMS != 3000 || live.EndMS != 3500 {
		t.Fatalf("live run = %+v", live)
	}
}

func TestRunsListWithNoSessionTableIsEmpty(t *testing.T) {
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusBadRequest, "Error during planning: table 'public.iox.session' not found"
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	sendV2(t, conn, map[string]any{"type": "runs_list"})
	if got := string(readMsg(t, conn)); got != `{"type":"runs","runs":[]}` {
		t.Fatalf("reply = %s, want an empty runs list", got)
	}
}

func TestRunsListQueryErrorIsErrorReply(t *testing.T) {
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusInternalServerError, "boom" }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	sendV2(t, conn, map[string]any{"type": "runs_list"})
	var e struct {
		Type    string `json:"type"`
		Code    string `json:"code"`
		Message string `json:"message"`
	}
	readInto(t, conn, &e)
	if e.Type != "error" || e.Code != "runs" || !strings.Contains(e.Message, "boom") {
		t.Fatalf("reply = %+v, want a runs error carrying boom", e)
	}
}

// With history off, runs_list is empty and a tile request is refused, and nothing
// is queried.
func TestHistoryOffAnswersWithoutQuerying(t *testing.T) {
	h := newTestHub(t)
	h.SetCatalog(loadTestCatalog(t))
	conn := tileConn(t, h)
	sendV2(t, conn, map[string]any{"type": "runs_list"})
	if got := string(readMsg(t, conn)); got != `{"type":"runs","runs":[]}` {
		t.Fatalf("runs reply = %s", got)
	}
	requestTile(t, conn, 3, "run-old", 0, 0, 0)
	if msg := readTileError(t, conn, 3); msg != "history is off" {
		t.Fatalf("tile_error %q, want history is off", msg)
	}
}

// A level 0 tile asks for the raw samples of a plain signal, on the window of its index.
func TestTileRawSamplesOfAPlainSignal(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(0)
	from, to := tileBounds(0, index)
	base := from + 5_000_000_000
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(3, base) }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 7, "run-old", signal, 0, index)
	tile := joinTile(t, readTile(t, conn, 7))

	q := f.queries()[0]
	if q.SQL != wantTileSQL(ts, 0, from, to) {
		t.Fatalf("SQL\n%s\nwant\n%s", q.SQL, wantTileSQL(ts, 0, from, to))
	}
	if !slices.Equal(mapKeys(q.Params), []string{"msg", "run"}) || q.Params["run"] != "run-old" || q.Params["msg"] != ts.msg.Name {
		t.Fatalf("params %v, want run and msg only", q.Params)
	}
	if tile.QID != 7 || tile.Signal != uint32(signal) || tile.Level != 0 || tile.Index != index || !tile.Sealed {
		t.Fatalf("tile %+v", tile)
	}
	if !slices.Equal(tile.TMS, []float64{float64(base / nsPerMs), float64(base/nsPerMs) + 1, float64(base/nsPerMs) + 2}) {
		t.Fatalf("t_ms %v", tile.TMS)
	}
	if !slices.Equal(tile.Raw, []float64{physical(ts, 0), physical(ts, 1), physical(ts, 2)}) {
		t.Fatalf("values %v, want the raw 0 1 2 scaled", tile.Raw)
	}
}

// A muxed signal's query also asks for its page (the tap).
func TestTileRollupOfAMuxedSignal(t *testing.T) {
	signal, ts := muxSignal(t)
	index := pastIndex(3)
	from, to := tileBounds(3, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, rollupReply(
			fmt.Sprintf("%d,1,9,5.5\n", from) + fmt.Sprintf("%d,2,8,5\n", from+1000*nsPerMs))
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 8, "run-old", signal, 3, index)
	tile := joinTile(t, readTile(t, conn, 8))

	q := f.queries()[0]
	if q.SQL != wantTileSQL(ts, 3, from, to) {
		t.Fatalf("SQL\n%s\nwant\n%s", q.SQL, wantTileSQL(ts, 3, from, to))
	}
	if q.Params["tap"] != strconv.Itoa(int(ts.sig.Mux)) {
		t.Fatalf("tap %q, want %d", q.Params["tap"], ts.sig.Mux)
	}
	min0, max0 := physicalRange(ts, 1, 9)
	min1, max1 := physicalRange(ts, 2, 8)
	if tile.Level != 3 || tile.Index != index || !tile.Sealed ||
		!slices.Equal(tile.Min, []float64{min0, min1}) || !slices.Equal(tile.Max, []float64{max0, max1}) ||
		!slices.Equal(tile.Mean, []float64{physical(ts, 5.5), physical(ts, 5)}) {
		t.Fatalf("tile %+v", tile)
	}
	// Level 3 buckets are 1000 ms apart.
	if !slices.Equal(tile.TMS, []float64{float64(from / nsPerMs), float64(from/nsPerMs) + 1000}) {
		t.Fatalf("t_ms %v", tile.TMS)
	}
}

// A tile of many samples arrives in pieces, each sample run starting where the
// last one ended, and the last flag only on the last.
func TestTileLargeLevel0ArrivesInPieces(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	const n = 10000
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, rawRows(n, from+1_000_000_000)
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 4, "run-old", signal, 0, index)
	pieces := readTile(t, conn, 4)
	if len(pieces) != 3 {
		t.Fatalf("%d pieces for %d samples, want 3", len(pieces), n)
	}
	for i, want := range []uint32{0, 4093, 8186} {
		if pieces[i].Offset != want {
			t.Fatalf("piece %d offset %d, want %d", i, pieces[i].Offset, want)
		}
	}
	tile := joinTile(t, pieces)
	if tile.N != n || len(tile.Raw) != n || tile.Raw[n-1] != physical(ts, n-1) || tile.TMS[n-1] != float64(from/nsPerMs+1000+n-1) {
		t.Fatalf("joined tile holds %d samples, want %d", tile.N, n)
	}
}

// A query whose column or table has no rows yet is an empty tile, not an error.
func TestTileWithNoDataIsEmpty(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status int
		body   string
	}{
		{"no field", http.StatusInternalServerError, `Schema error: No field named "tap". Valid fields are frame.v.`},
		{"no table", http.StatusBadRequest, `Error during planning: table 'public.iox.frame' not found`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			signal, _ := plainSignal(t)
			index := pastIndex(0)
			f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return tc.status, tc.body }}
			conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
			requestTile(t, conn, 5, "run-old", signal, 0, index)
			pieces := readTile(t, conn, 5)
			if len(pieces) != 1 || pieces[0].N != 0 || !pieces[0].Last || !pieces[0].Sealed {
				t.Fatalf("pieces %+v, want one empty sealed piece", pieces)
			}
		})
	}
}

func TestTileQueryFailureIsTileError(t *testing.T) {
	signal, _ := plainSignal(t)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusInternalServerError, "boom" }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 9, "run-old", signal, 0, pastIndex(0))
	if msg := readTileError(t, conn, 9); !strings.Contains(msg, "boom") {
		t.Fatalf("tile_error %q, want it to carry boom", msg)
	}
}

// Requests that name no signal, no level, or no tile are refused with the qid.
func TestTileRequestsThatAreRefused(t *testing.T) {
	f := &fakeQueries{}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	for _, tc := range []struct {
		qid     uint32
		signal  int64
		level   int
		index   int64
		message string
	}{
		{1, 1 << 30, 0, 0, "unknown signal"},
		{2, 0, 7, 0, "level 7 is not in 0 to 6"},
		{3, 0, -1, 0, "level -1 is not in 0 to 6"},
		{4, 0, 0, -1, "tile index -1 is out of range"},
		{5, 0, 0, 1 << 62, "is out of range"},
	} {
		requestTile(t, conn, tc.qid, "run-old", tc.signal, tc.level, tc.index)
		if msg := readTileError(t, conn, tc.qid); !strings.Contains(msg, tc.message) {
			t.Fatalf("qid %d: tile_error %q, want %q", tc.qid, msg, tc.message)
		}
	}
	if n := len(f.queries()); n != 0 {
		t.Fatalf("%d queries for refused requests, want none", n)
	}
}

// A tile_request without a usable qid gets bad_tile_request.
func TestTileRequestMalformedIsBadRequest(t *testing.T) {
	conn := tileConn(t, newTileHub(t, &fakeQueries{}, fixedLive(tileTestRun)))
	sendV2(t, conn, json.RawMessage(`{"type":"tile_request","qid":"x"}`))
	var e struct {
		Type string `json:"type"`
		Code string `json:"code"`
	}
	readInto(t, conn, &e)
	if e.Type != "error" || e.Code != "bad_tile_request" {
		t.Fatalf("reply = %+v, want bad_tile_request", e)
	}
}

// Without a catalog there are no signals, so a tile request is refused.
func TestTileWithoutCatalogIsTileError(t *testing.T) {
	h := newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), fixedLive(tileTestRun), newLiveRing(liveRingMinFrames),
		newInfluxQuerier("http://127.0.0.1:1", "photon_test", "", 1))
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)
	requestTile(t, conn, 6, "run-old", 0, 0, 0)
	if msg := readTileError(t, conn, 6); msg != "no catalog" {
		t.Fatalf("tile_error %q, want no catalog", msg)
	}
}

// tile_cancel stops a request whose query is still running: no tile follows.
func TestTileCancelStopsTheRequest(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	started := make(chan struct{}, 1)
	f := &fakeQueries{reply: func(ctx context.Context, _ fakeQuery) (int, string) {
		started <- struct{}{}
		<-ctx.Done()
		return http.StatusOK, rawRows(1, 0)
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 5, "run-old", signal, 0, index)
	select {
	case <-started:
	case <-time.After(5 * time.Second):
		t.Fatal("the query never started")
	}
	sendV2(t, conn, map[string]any{"type": "tile_cancel", "qid": 5})
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 1})
	var pong struct {
		Type string `json:"type"`
	}
	readInto(t, conn, &pong)
	if pong.Type != "pong" {
		t.Fatalf("after tile_cancel got %s, want the pong and no tile", pong.Type)
	}
	expectNothingElse(t, conn)
}

// At most v2MaxTileRequests requests are in flight for one client; the next is refused.
func TestTooManyTileRequestsAreRefused(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	f := &fakeQueries{reply: func(ctx context.Context, _ fakeQuery) (int, string) {
		<-ctx.Done()
		return http.StatusOK, ""
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	for qid := uint32(1); qid <= v2MaxTileRequests+1; qid++ {
		requestTile(t, conn, qid, "run-old", signal, 0, index)
	}
	if msg := readTileError(t, conn, v2MaxTileRequests+1); msg != "too many tile requests" {
		t.Fatalf("tile_error %q, want too many tile requests", msg)
	}
}

// A sealed tile of the live run is read once; the next request for it comes from
// the cache, with its own qid.
func TestLiveRunTileIsCachedOnce(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(3, from) }}
	h := newTileHub(t, f, fixedLive(tileTestRun))
	conn := tileConn(t, h)

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	first := joinTile(t, readTile(t, conn, 1))
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	second := joinTile(t, readTile(t, conn, 2))

	if n := len(f.queries()); n != 1 {
		t.Fatalf("%d queries, want 1: the second request should come from the cache", n)
	}
	if !first.Sealed || second.QID != 2 || !slices.Equal(first.Raw, second.Raw) || !slices.Equal(first.TMS, second.TMS) {
		t.Fatalf("first %+v, second %+v", first, second)
	}
	if m := v2Metadata(h); m.TileCacheHits != 1 || m.TileCacheBytes <= 0 || m.TilesServed != 2 {
		t.Fatalf("metadata hits %d bytes %d served %d", m.TileCacheHits, m.TileCacheBytes, m.TilesServed)
	}
}

// A tile that still holds now is not sealed, so it is queried every time.
func TestRecentLiveTileIsNotCached(t *testing.T) {
	signal, _ := plainSignal(t)
	index := currentIndex(0)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(1, 0) }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	if first := joinTile(t, readTile(t, conn, 1)); first.Sealed {
		t.Fatal("a tile that holds now is marked sealed")
	}
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	readTile(t, conn, 2)
	if n := len(f.queries()); n != 2 {
		t.Fatalf("%d queries, want 2: an unsealed tile is not cached", n)
	}
}

// Tiles of a run that is not live are sealed, and always read from InfluxDB.
func TestOtherRunTilesAreNeverCached(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(1, 0) }}
	h := newTileHub(t, f, fixedLive(tileTestRun))
	conn := tileConn(t, h)

	for qid := uint32(1); qid <= 2; qid++ {
		requestTile(t, conn, qid, "run-old", signal, 0, index)
		if tile := joinTile(t, readTile(t, conn, qid)); !tile.Sealed {
			t.Fatal("a past run's tile is not sealed")
		}
	}
	if n := len(f.queries()); n != 2 {
		t.Fatalf("%d queries, want 2", n)
	}
	if m := v2Metadata(h); m.TileCacheBytes != 0 {
		t.Fatalf("cache holds %d bytes of another run's tiles", m.TileCacheBytes)
	}
}

// When the live run changes, the cache empties: tiles of the old live run are not
// served once it is no longer live, nor after the new one has had its own tiles.
func TestLiveRunChangeEmptiesTheCache(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(1, 0) }}
	var mu sync.Mutex
	liveID := tileTestRun
	setLive := func(id string) { mu.Lock(); liveID = id; mu.Unlock() }
	live := func() string { mu.Lock(); defer mu.Unlock(); return liveID }
	conn := tileConn(t, newTileHub(t, f, live))
	queries := func() int { return len(f.queries()) }

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	readTile(t, conn, 1)
	if queries() != 1 {
		t.Fatalf("%d queries after the first tile, want 1", queries())
	}

	setLive("run-next")
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	readTile(t, conn, 2)
	if queries() != 2 {
		t.Fatalf("%d queries after the old run stopped being live, want 2", queries())
	}

	requestTile(t, conn, 3, "run-next", signal, 0, index)
	readTile(t, conn, 3)
	if queries() != 3 {
		t.Fatalf("%d queries for the new live run's first tile, want 3", queries())
	}

	setLive(tileTestRun)
	requestTile(t, conn, 4, tileTestRun, signal, 0, index)
	readTile(t, conn, 4)
	if queries() != 4 {
		t.Fatalf("%d queries after the cache changed runs, want 4: the old entry must be gone", queries())
	}
}

// runs_list and tile requests are answered in their own goroutines, so a slow
// query does not hold up the reader: a ping gets its pong first.
func TestSlowQueryDoesNotBlockTheReader(t *testing.T) {
	signal, _ := plainSignal(t)
	release := make(chan struct{})
	unblock := once(func() { close(release) })
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		<-release
		return http.StatusOK, rawRows(1, 0)
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	t.Cleanup(unblock)

	requestTile(t, conn, 1, "run-old", signal, 0, pastIndex(0))
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 2})
	var pong struct {
		Type string `json:"type"`
	}
	readInto(t, conn, &pong)
	if pong.Type != "pong" {
		t.Fatalf("got %s while the query was running, want the pong", pong.Type)
	}
	unblock()
	readTile(t, conn, 1)
}

// Once a request's last message has been read, its qid can be used again at once.
func TestTileQIDIsFreeWhenTheAnswerEnds(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(2, 0) }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	for range 3 {
		requestTile(t, conn, 42, "run-old", signal, 0, index)
		readTile(t, conn, 42)
	}
}

// A tile built sample by sample is cut into pieces of at most 64 KiB: each piece
// full but the last, with the offsets running on and the last flag only on the last.
// An empty tile is one piece with no samples.
func TestTilePiecesFitTheLimit(t *testing.T) {
	for _, level := range []int{0, 2} {
		const n = 10000
		var pieces [][]byte
		send := func(piece []byte, last bool) bool { pieces = append(pieces, piece); return true }
		b := newTileBuilder(tileJob{qid: 9, signal: 3, level: level}, true, send)
		vals := make([]float64, 1)
		if level > 0 {
			vals = make([]float64, 3)
		}
		for i := range n {
			if err := b.add(float64(i), vals); err != nil {
				t.Fatal(err)
			}
		}
		if err := b.end(); err != nil {
			t.Fatal(err)
		}
		total := uint32(0)
		for i, m := range pieces {
			if len(m) > v2TilePieceMax {
				t.Fatalf("level %d piece %d is %d bytes, over %d", level, i, len(m), v2TilePieceMax)
			}
			piece, err := decodeTile(m)
			if err != nil {
				t.Fatal(err)
			}
			if piece.Offset != total || piece.Last != (i == len(pieces)-1) || !piece.Sealed || piece.Signal != 3 || piece.QID != 9 {
				t.Fatalf("level %d piece %d = %+v", level, i, piece)
			}
			total += piece.N
		}
		if total != n {
			t.Fatalf("level %d pieces hold %d samples, want %d", level, total, n)
		}
	}
	var empty [][]byte
	eb := newTileBuilder(tileJob{qid: 1, signal: 1}, false, func(piece []byte, last bool) bool {
		empty = append(empty, piece)
		return true
	})
	if err := eb.end(); err != nil {
		t.Fatal(err)
	}
	if len(empty) != 1 {
		t.Fatalf("empty tile is %d pieces, want 1", len(empty))
	}
	if piece, err := decodeTile(empty[0]); err != nil || piece.N != 0 || !piece.Last || piece.Sealed {
		t.Fatalf("empty piece %+v, %v", piece, err)
	}
}

// mapKeys returns the keys of m, sorted.
func mapKeys(m map[string]string) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	slices.Sort(keys)
	return keys
}

// A raw tile that fills the LIMIT is cut short, so it is a tile_error, not data.
func TestTileAtRawLimitIsTileError(t *testing.T) {
	signal, _ := plainSignal(t)
	var body strings.Builder
	body.WriteString("t_ns,v\n")
	for i := 0; i < tileRawRows; i++ {
		fmt.Fprintf(&body, "%d,1\n", 1_700_000_000_000_000_000+int64(i))
	}
	rows := body.String()
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rows }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 11, "run-old", signal, 0, pastIndex(0))
	pieces, msg := readTileReply(t, conn, 11)
	if !strings.Contains(msg, "more than") {
		t.Fatalf("tile_error %q, want the raw limit named", msg)
	}
	// The error comes at the row that reaches the limit, so the samples before it were
	// sent as whole pieces, and then the error.
	if want := (tileRawRows - 1) / tilePieceSamples(0); len(pieces) != want {
		t.Fatalf("%d pieces before the error, want %d", len(pieces), want)
	}
	for i, p := range pieces {
		if p.N != uint32(tilePieceSamples(0)) || p.Last {
			t.Fatalf("piece %d holds %d samples, want a whole piece", i, p.N)
		}
	}
}

// physical is a raw value of ts in the signal's unit, as the server sends it.
func physical(ts tileSignal, raw float64) float64 {
	return raw*ts.sig.Factor + ts.sig.Offset
}

// physicalRange is the min and max, in the signal's unit, of a bucket whose raw min
// and max are lo and hi. A negative factor swaps them.
func physicalRange(ts tileSignal, lo, hi float64) (float64, float64) {
	a, b := physical(ts, lo), physical(ts, hi)
	if ts.sig.Factor < 0 {
		return b, a
	}
	return a, b
}

// A signal with a negative factor is scaled, and at level 2 its min and max trade
// places, so min is the lower one. Neg is raw * -0.5 + 10: raw 0, 1 and 2 are 10,
// 9.5 and 9, and a bucket of raw 2 to 8 with mean 5 is min 6, max 9, mean 7.5.
func TestTileScalesNegativeFactor(t *testing.T) {
	cat := liveCatalog(t)
	signal, _ := signalAt(t, cat, "Std101", "Neg")

	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(3, from) }}
	conn := tileConn(t, newTileHubOn(t, f, fixedLive(tileTestRun), cat))
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	if raw := joinTile(t, readTile(t, conn, 1)); !slices.Equal(raw.Raw, []float64{10, 9.5, 9}) {
		t.Fatalf("level 0 values %v, want 10 9.5 9", raw.Raw)
	}

	index2 := pastIndex(3)
	from2, _ := tileBounds(3, index2)
	f2 := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, rollupReply(fmt.Sprintf("%d,2,8,5\n", from2))
	}}
	conn2 := tileConn(t, newTileHubOn(t, f2, fixedLive(tileTestRun), cat))
	requestTile(t, conn2, 2, "run-old", signal, 3, index2)
	bucket := joinTile(t, readTile(t, conn2, 2))
	if !slices.Equal(bucket.Min, []float64{6}) || !slices.Equal(bucket.Max, []float64{9}) || !slices.Equal(bucket.Mean, []float64{7.5}) {
		t.Fatalf("level 2 min %v, max %v, mean %v; want 6, 9, 7.5", bucket.Min, bucket.Max, bucket.Mean)
	}
}

// A sealed tile of the live run is cached with its values already scaled, so the
// second request, answered from the cache, has the same physical values.
func TestLiveRunTileCacheHoldsScaledValues(t *testing.T) {
	cat := liveCatalog(t)
	signal, _ := signalAt(t, cat, "Std101", "Neg")
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(3, from) }}
	conn := tileConn(t, newTileHubOn(t, f, fixedLive(tileTestRun), cat))
	want := []float64{10, 9.5, 9}

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	first := joinTile(t, readTile(t, conn, 1))
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	second := joinTile(t, readTile(t, conn, 2))
	if !slices.Equal(first.Raw, want) || !slices.Equal(second.Raw, want) {
		t.Fatalf("values %v then %v, want %v both times", first.Raw, second.Raw, want)
	}
	if n := len(f.queries()); n != 1 {
		t.Fatalf("%d queries, want 1: the second request should come from the cache", n)
	}
}

// A tile is streamed: its first piece reaches the client while InfluxDB is still
// writing the reply, not after the whole tile has been read.
func TestTileStreamsBeforeTheQueryEnds(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	const first, total = 5000, 9000
	release := make(chan struct{})
	unblock := once(func() { close(release) })
	url, _ := newPausingQueryServer(t, "t_ns,v\n"+rawLines(from, 0, first), rawLines(from, first, total), release)
	t.Cleanup(unblock)
	conn := tileConn(t, newTileHubURL(t, url, fixedLive(tileTestRun), loadTestCatalog(t)))

	requestTile(t, conn, 7, "run-old", signal, 0, index)
	typ, data := readAny(t, conn) // read before the release, so the reply is not finished
	if typ != wsBinary {
		t.Fatalf("got text %s, want the first piece", data)
	}
	head, err := decodeTile(data)
	if err != nil {
		t.Fatal(err)
	}
	if head.QID != 7 || head.Offset != 0 || head.N != uint32(tilePieceSamples(0)) || head.Last {
		t.Fatalf("first piece %+v, want a whole piece with more to come", head)
	}

	unblock()
	tile := joinTile(t, append([]v2Tile{head}, readTile(t, conn, 7)...))
	if tile.N != total || !tile.Sealed {
		t.Fatalf("tile holds %d samples, sealed %v; want %d, sealed", tile.N, tile.Sealed, total)
	}
	for k := range total {
		if tile.TMS[k] != float64(from/nsPerMs+int64(k)) || tile.Raw[k] != physical(ts, float64(k)) {
			t.Fatalf("sample %d = (%v, %v)", k, tile.TMS[k], tile.Raw[k])
		}
	}
}

// A bad row after some pieces were sent ends the tile with a tile_error behind them.
// That tile is not cached, so the next request reads InfluxDB again.
func TestTileBadRowMidReplyIsTileError(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	var calls atomic.Int32
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		if calls.Add(1) > 1 {
			return http.StatusOK, rawRows(3, from)
		}
		return http.StatusOK, "t_ns,v\n" + rawLines(from, 0, 5000) + fmt.Sprintf("%d,oops\n", from+5000*nsPerMs)
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	pieces, msg := readTileReply(t, conn, 1)
	if len(pieces) != 1 || !strings.Contains(msg, "bad value") {
		t.Fatalf("%d pieces then error %q, want one piece then a bad value error", len(pieces), msg)
	}
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	if again := joinTile(t, readTile(t, conn, 2)); again.N != 3 || len(f.queries()) != 2 {
		t.Fatalf("second request holds %d samples after %d queries; want 3 after 2", again.N, len(f.queries()))
	}
}

// A connection that breaks mid-reply ends the tile with a tile_error behind the
// pieces already sent, and the tile is not cached.
func TestTileConnectionBreakMidReplyIsTileError(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	var calls atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		if calls.Add(1) > 1 {
			_, _ = io.WriteString(w, rawRows(3, from))
			return
		}
		_, _ = io.WriteString(w, "t_ns,v\n"+rawLines(from, 0, 5000))
		w.(http.Flusher).Flush()
		panic(http.ErrAbortHandler) // the connection breaks before the reply ends
	}))
	t.Cleanup(srv.Close)
	conn := tileConn(t, newTileHubURL(t, srv.URL, fixedLive(tileTestRun), loadTestCatalog(t)))

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	pieces, msg := readTileReply(t, conn, 1)
	if len(pieces) != 1 || msg == "" {
		t.Fatalf("%d pieces then error %q, want one piece then an error", len(pieces), msg)
	}
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	if again := joinTile(t, readTile(t, conn, 2)); again.N != 3 || calls.Load() != 2 {
		t.Fatalf("second request holds %d samples after %d queries; want 3 after 2", again.N, calls.Load())
	}
}

// tile_cancel while the reply is still arriving stops the read: the query gives up,
// and nothing more is sent for the request.
func TestTileCancelMidReplyStopsTheRead(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	release := make(chan struct{})
	unblock := once(func() { close(release) })
	url, stopped := newPausingQueryServer(t, "t_ns,v\n"+rawLines(from, 0, 5000), rawLines(from, 5000, 9000), release)
	t.Cleanup(unblock)
	conn := tileConn(t, newTileHubURL(t, url, fixedLive(tileTestRun), loadTestCatalog(t)))

	requestTile(t, conn, 5, "run-old", signal, 0, index)
	typ, data := readAny(t, conn)
	if typ != wsBinary {
		t.Fatalf("got text %s, want the first piece", data)
	}
	if piece, err := decodeTile(data); err != nil || piece.QID != 5 || piece.Last {
		t.Fatalf("first piece %+v, %v", piece, err)
	}
	sendV2(t, conn, map[string]any{"type": "tile_cancel", "qid": 5})
	select {
	case <-stopped:
	case <-time.After(5 * time.Second):
		t.Fatal("the query still reads after tile_cancel")
	}
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 3})
	var pong struct {
		Type string `json:"type"`
	}
	readInto(t, conn, &pong)
	if pong.Type != "pong" {
		t.Fatalf("got %s after tile_cancel, want the pong and no more pieces", pong.Type)
	}
	expectNothingElse(t, conn)
}

// A row whose value is NULL, or not finite, is no sample, at raw level and in a
// rollup; the other rows are kept, in order.
func TestTileSkipsNULLAndNonFiniteRows(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	raw := fmt.Sprintf("t_ns,v\n%d,1\n%d,\n%d,NaN\n%d,3\n", from, from+nsPerMs, from+2*nsPerMs, from+3*nsPerMs)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, raw }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	tile := joinTile(t, readTile(t, conn, 1))
	ms := from / nsPerMs
	if !slices.Equal(tile.TMS, []float64{float64(ms), float64(ms + 3)}) ||
		!slices.Equal(tile.Raw, []float64{physical(ts, 1), physical(ts, 3)}) {
		t.Fatalf("samples %v at %v; want the rows with values 1 and 3", tile.Raw, tile.TMS)
	}

	index3 := pastIndex(3)
	from3, _ := tileBounds(3, index3)
	rollup := rollupReply(fmt.Sprintf("%d,1,2,1.5\n%d,,,\n%d,3,4,3.5\n",
		from3, from3+1000*nsPerMs, from3+2000*nsPerMs))
	f3 := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rollup }}
	conn3 := tileConn(t, newTileHub(t, f3, fixedLive(tileTestRun)))
	requestTile(t, conn3, 2, "run-old", signal, 3, index3)
	buckets := joinTile(t, readTile(t, conn3, 2))
	min0, max0 := physicalRange(ts, 1, 2)
	min1, max1 := physicalRange(ts, 3, 4)
	mb := from3 / nsPerMs
	if !slices.Equal(buckets.TMS, []float64{float64(mb), float64(mb + 2000)}) ||
		!slices.Equal(buckets.Min, []float64{min0, min1}) || !slices.Equal(buckets.Max, []float64{max0, max1}) ||
		!slices.Equal(buckets.Mean, []float64{physical(ts, 1.5), physical(ts, 3.5)}) {
		t.Fatalf("buckets %+v, want the two whole buckets", buckets)
	}
}

// A level 0 tile spans 10 s, and a level 1 bucket is 10 ms, so a level 1 tile spans
// 1024 buckets of 10 ms, which is 10.24 s. Each query covers that window.
func TestTileSpansAndTheTenMillisecondLevel(t *testing.T) {
	signal, _ := plainSignal(t)
	index := int64(123_456)
	f := &fakeQueries{}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	readTile(t, conn, 1)
	requestTile(t, conn, 2, "run-old", signal, 1, index)
	readTile(t, conn, 2)

	qs := f.queries()
	raw := index * 10_000 * nsPerMs
	if want := fmt.Sprintf("time >= to_timestamp_nanos(%d) AND time < to_timestamp_nanos(%d)", raw, raw+10_000*nsPerMs); !strings.Contains(qs[0].SQL, want) {
		t.Fatalf("level 0 SQL %s, want the window %q", qs[0].SQL, want)
	}
	ten := index * 10_240 * nsPerMs
	want := fmt.Sprintf("time >= to_timestamp_nanos(%d) AND time < to_timestamp_nanos(%d)", ten, ten+10_240*nsPerMs)
	if !strings.Contains(qs[1].SQL, "INTERVAL '10 milliseconds'") || !strings.Contains(qs[1].SQL, want) {
		t.Fatalf("level 1 SQL %s, want 10 ms buckets over %q", qs[1].SQL, want)
	}
}

// A sealed tile of the live run that takes several pieces is cached once, and the
// second request replays the same pieces with its own qid.
func TestLiveRunMultiPieceTileIsCachedOnce(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	const n = 10000
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(n, from) }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	first := readTile(t, conn, 1)
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	second := readTile(t, conn, 2)

	if len(first) != 3 || len(second) != len(first) {
		t.Fatalf("%d and %d pieces, want 3 each", len(first), len(second))
	}
	for i := range first {
		a, b := first[i], second[i]
		if b.QID != 2 || b.Offset != a.Offset || b.N != a.N || b.Last != a.Last || b.Sealed != a.Sealed ||
			!slices.Equal(b.TMS, a.TMS) || !slices.Equal(b.Raw, a.Raw) {
			t.Fatalf("piece %d differs from the first request's: %+v, want %+v", i, b, a)
		}
	}
	if n := len(f.queries()); n != 1 {
		t.Fatalf("%d queries, want 1: the second request should come from the cache", n)
	}
}


// A streaming tile gives up when the client's live queue takes nothing for the
// stall limit, so a client that stops reading does not keep an InfluxDB slot.
func TestTileSendGivesUpOnAStalledClient(t *testing.T) {
	c := &v2Client{live: make(chan outMsg), done: make(chan struct{})} // nobody reads live
	start := time.Now()
	ok, timedOut := c.enqueueTileWithin(context.Background(), outMsg{typ: wsBinary}, 50*time.Millisecond)
	if ok || !timedOut {
		t.Fatalf("ok=%v timedOut=%v, want a timeout", ok, timedOut)
	}
	if d := time.Since(start); d < 50*time.Millisecond || d > 2*time.Second {
		t.Fatalf("gave up after %s", d)
	}
	// A reader that takes the message in time: queued, no timeout.
	go func() { <-c.live }()
	if ok, timedOut := c.enqueueTileWithin(context.Background(), outMsg{typ: wsBinary}, time.Second); !ok || timedOut {
		t.Fatalf("ok=%v timedOut=%v, want queued", ok, timedOut)
	}
	// A cancelled request is not a stall.
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if ok, timedOut := c.enqueueTileWithin(ctx, outMsg{typ: wsBinary}, time.Second); ok || timedOut {
		t.Fatalf("cancelled: ok=%v timedOut=%v", ok, timedOut)
	}
}
