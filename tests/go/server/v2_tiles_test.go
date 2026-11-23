package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
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
	q := newInfluxQuerier(newFakeQueryServer(t, fake), "photon_test", "", 6)
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

// readTile reads the pieces that answer qid, up to the one with the last flag.
func readTile(t *testing.T, conn *wsConn, qid uint32) []v2Tile {
	t.Helper()
	var pieces []v2Tile
	for {
		typ, data := readAny(t, conn)
		if typ == wsText {
			t.Fatalf("got text %s while reading tile %d", data, qid)
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
			return pieces
		}
	}
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

// rawRows is n level 0 rows of the query reply: one sample per ms from baseNS, valued 0 to n-1.
func rawRows(n int, baseNS int64) string {
	var b strings.Builder
	for k := range n {
		fmt.Fprintf(&b, `{"t_ns":%d,"v":%d}`+"\n", baseNS+int64(k)*nsPerMs, k)
	}
	return b.String()
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
	index := pastIndex(2)
	from, to := tileBounds(2, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK,
			fmt.Sprintf(`{"t_ns":%d,"lo":1,"hi":9,"mean":5.5}`+"\n", from) +
				fmt.Sprintf(`{"t_ns":%d,"lo":2,"hi":8,"mean":5}`+"\n", from+1000*nsPerMs)
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	requestTile(t, conn, 8, "run-old", signal, 2, index)
	tile := joinTile(t, readTile(t, conn, 8))

	q := f.queries()[0]
	if q.SQL != wantTileSQL(ts, 2, from, to) {
		t.Fatalf("SQL\n%s\nwant\n%s", q.SQL, wantTileSQL(ts, 2, from, to))
	}
	if q.Params["tap"] != strconv.Itoa(int(ts.sig.Mux)) {
		t.Fatalf("tap %q, want %d", q.Params["tap"], ts.sig.Mux)
	}
	min0, max0 := physicalRange(ts, 1, 9)
	min1, max1 := physicalRange(ts, 2, 8)
	if tile.Level != 2 || tile.Index != index || !tile.Sealed ||
		!slices.Equal(tile.Min, []float64{min0, min1}) || !slices.Equal(tile.Max, []float64{max0, max1}) ||
		!slices.Equal(tile.Mean, []float64{physical(ts, 5.5), physical(ts, 5)}) {
		t.Fatalf("tile %+v", tile)
	}
	// Level 2 buckets are 1000 ms apart.
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
		{2, 0, 6, 0, "level 6 is not in 0 to 5"},
		{3, 0, -1, 0, "level -1 is not in 0 to 5"},
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

// encodeTile splits a long tile into pieces of at most 64 KiB, and an empty tile is
// one piece with no samples.
func TestEncodeTilePiecesFitTheLimit(t *testing.T) {
	for _, level := range []int{0, 2} {
		const n = 10000
		td := &tileData{level: uint8(level), index: 7, tMS: make([]float64, n)}
		if level == 0 {
			td.raw = make([]float64, n)
		} else {
			td.lo, td.hi, td.mean = make([]float64, n), make([]float64, n), make([]float64, n)
		}
		msgs := encodeTile(9, 3, true, td)
		total := uint32(0)
		for i, m := range msgs {
			if len(m) > v2TilePieceMax {
				t.Fatalf("level %d piece %d is %d bytes, over %d", level, i, len(m), v2TilePieceMax)
			}
			piece, err := decodeTile(m)
			if err != nil {
				t.Fatal(err)
			}
			if piece.Offset != total || piece.Last != (i == len(msgs)-1) || !piece.Sealed || piece.Signal != 3 || piece.QID != 9 {
				t.Fatalf("level %d piece %d = %+v", level, i, piece)
			}
			total += piece.N
		}
		if total != n {
			t.Fatalf("level %d pieces hold %d samples, want %d", level, total, n)
		}
	}
	empty := encodeTile(1, 1, false, &tileData{})
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
	for i := 0; i < tileRawRows; i++ {
		fmt.Fprintf(&body, "{\"t_ns\":%d,\"v\":1}\n", 1_700_000_000_000_000_000+int64(i))
	}
	rows := body.String()
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rows }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 11, "run-old", signal, 0, pastIndex(0))
	if msg := readTileError(t, conn, 11); !strings.Contains(msg, "more than") {
		t.Fatalf("tile_error %q, want the raw limit named", msg)
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

	index2 := pastIndex(2)
	from2, _ := tileBounds(2, index2)
	f2 := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, fmt.Sprintf(`{"t_ns":%d,"lo":2,"hi":8,"mean":5}`+"\n", from2)
	}}
	conn2 := tileConn(t, newTileHubOn(t, f2, fixedLive(tileTestRun), cat))
	requestTile(t, conn2, 2, "run-old", signal, 2, index2)
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

