package main

import (
	"context"
	"fmt"
	"net/http"
	"regexp"
	"strings"
	"testing"
	"time"
)

// spanReply is the reply to a run's span query: the run's first and last session times, in ms.
func spanReply(startMS, endMS int64) string {
	return fmt.Sprintf(`{"start":%d,"end":%d}`+"\n", startMS, endMS)
}

// isRollupQuery reports whether a query reads rollup_1s.
func isRollupQuery(q fakeQuery) bool {
	return strings.Contains(q.SQL, "FROM rollup_1s")
}

// A tile of a second or more is read from rollup_1s, with the sum over the count for
// its mean; a tile of 100 ms is read from the frame rows.
func TestTileOfASecondReadsRollup(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(3)
	from, to := tileBounds(3, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, rollupReply(fmt.Sprintf("%d,4,6,5\n", from))
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 3, index)
	tile := joinTile(t, readTile(t, conn, 1))

	qs := f.queries()
	if len(qs) != 1 || qs[0].SQL != wantTileSQL(ts, 3, from, to) || !isRollupQuery(qs[0]) {
		t.Fatalf("queries %+v, want the one rollup query\n%s", qs, wantTileSQL(ts, 3, from, to))
	}
	if !strings.Contains(qs[0].SQL, `sum("`+ts.sig.Name+`_sum") / sum("`+ts.sig.Name+`_n")`) {
		t.Fatalf("mean is not sum over count: %s", qs[0].SQL)
	}
	if !slicesEqual(tile.Min, []float64{physical(ts, 4)}) || !slicesEqual(tile.Max, []float64{physical(ts, 6)}) ||
		!slicesEqual(tile.Mean, []float64{physical(ts, 5)}) || tile.Level != 3 {
		t.Fatalf("tile %+v", tile)
	}
}

func TestTile100msReadsFrames(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(2)
	from, to := tileBounds(2, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, rollupReply(fmt.Sprintf("%d,1,2,1.5\n", from))
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 2, index)
	readTile(t, conn, 1)
	qs := f.queries()
	if len(qs) != 1 || qs[0].SQL != wantTileSQL(ts, 2, from, to) || strings.Contains(qs[0].SQL, "rollup_1s") {
		t.Fatalf("queries %+v, want the one frame query", qs)
	}
}

// A rollup query with no rows, or with no such column yet, is answered from the frame
// rows: runs written before rollup_1s have no rollup rows.
func TestTileFallsBackToFramesWhenRollupHasNothing(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(3)
	from, _ := tileBounds(3, index)
	for _, tc := range []struct {
		name   string
		status int
		body   string
	}{
		{"no rows", http.StatusOK, ""},
		{"no column yet", http.StatusInternalServerError, `Schema error: No field named "` + ts.sig.Name + `_min".`},
		{"no table yet", http.StatusBadRequest, "Error during planning: table 'public.iox.rollup_1s' not found"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &fakeQueries{reply: func(_ context.Context, q fakeQuery) (int, string) {
				if isRollupQuery(q) {
					return tc.status, tc.body
				}
				return http.StatusOK, rollupReply(fmt.Sprintf("%d,4,6,5\n", from))
			}}
			conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
			requestTile(t, conn, 1, "run-old", signal, 3, index)
			tile := joinTile(t, readTile(t, conn, 1))
			qs := f.queries()
			if len(qs) != 2 || !isRollupQuery(qs[0]) || strings.Contains(qs[1].SQL, "rollup_1s") {
				t.Fatalf("queries %+v, want the rollup query and then the frame query", qs)
			}
			if !slicesEqual(tile.Mean, []float64{physical(ts, 5)}) {
				t.Fatalf("tile %+v, want the frame rows' bucket", tile)
			}
		})
	}
}

// Any other failure of the rollup query is a tile error: the frame rows are not read
// in its place.
func TestTileRollupFailureIsTileErrorWithoutFallback(t *testing.T) {
	signal, _ := plainSignal(t)
	f := &fakeQueries{reply: func(_ context.Context, q fakeQuery) (int, string) {
		return http.StatusInternalServerError, "boom"
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 3, pastIndex(3))
	if msg := readTileError(t, conn, 1); !strings.Contains(msg, "boom") {
		t.Fatalf("tile_error %q, want it to carry boom", msg)
	}
	if n := len(f.queries()); n != 1 {
		t.Fatalf("%d queries after a rollup failure, want 1", n)
	}
}

// The window of a tile is clamped to the run's span: from the second of its first
// frame to one second past its last.
func TestTileWindowIsClampedToTheRun(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	msFrom := from / nsPerMs
	f := &fakeQueries{span: spanReply(msFrom+2300, msFrom+3400)}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	readTile(t, conn, 1)

	lo, hi := from+2000*nsPerMs, from+4400*nsPerMs
	qs := f.queries()
	if len(qs) != 1 || qs[0].SQL != wantTileSQL(ts, 0, lo, hi) {
		t.Fatalf("queries %+v, want the window %d to %d\n%s", qs, lo, hi, wantTileSQL(ts, 0, lo, hi))
	}
}

// A tile that lies outside its run is empty, and no query is sent for it.
func TestTileOutsideTheRunIsEmptyWithoutQuery(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	msFrom := from / nsPerMs
	f := &fakeQueries{span: spanReply(msFrom+20000, msFrom+30000)}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	pieces := readTile(t, conn, 1)
	if len(pieces) != 1 || pieces[0].N != 0 || !pieces[0].Last || !pieces[0].Sealed {
		t.Fatalf("pieces %+v, want one empty sealed piece", pieces)
	}
	if n := len(f.queries()); n != 0 {
		t.Fatalf("%d tile queries for a tile outside its run, want none", n)
	}
}

// The live run's window ends now: a tile that holds now is cut at one second past it.
func TestLiveRunWindowEndsNow(t *testing.T) {
	signal, _ := plainSignal(t)
	index := currentIndex(0)
	from, to := tileBounds(0, index)
	msFrom := from / nsPerMs
	f := &fakeQueries{span: spanReply(msFrom-1000, 0)}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	before := time.Now().UnixNano()
	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	readTile(t, conn, 1)
	after := time.Now().UnixNano()

	qs := f.queries()
	if len(qs) != 1 {
		t.Fatalf("%d queries, want 1", len(qs))
	}
	m := regexp.MustCompile(`time < to_timestamp_nanos\((\d+)\)`).FindStringSubmatch(qs[0].SQL)
	if m == nil {
		t.Fatalf("no upper bound in %s", qs[0].SQL)
	}
	var hi int64
	fmt.Sscan(m[1], &hi)
	low, high := min(to, before+rollupNS), min(to, after+rollupNS)
	if hi < low || hi > high {
		t.Fatalf("window ends at %d, want between %d and %d (now, plus a second, and the tile's end %d)", hi, low, high, to)
	}
}

// A run's span is read once, and then kept while the run is not live.
func TestRunSpanIsReadOnce(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	msFrom := from / nsPerMs
	f := &fakeQueries{span: spanReply(msFrom, msFrom+5000), reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, rawRows(1, from)
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	for qid := uint32(1); qid <= 2; qid++ {
		requestTile(t, conn, qid, "run-old", signal, 0, index)
		readTile(t, conn, qid)
	}
	if n := f.spanReads.Load(); n != 1 {
		t.Fatalf("%d span reads for two tiles of one run, want 1", n)
	}
}

// A run with no session row is not clamped: its tiles are queried over their own window.
func TestRunWithoutSessionRowIsNotClamped(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(0)
	from, to := tileBounds(0, index)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, rawRows(1, from)
	}}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	readTile(t, conn, 1)
	if qs := f.queries(); len(qs) != 1 || qs[0].SQL != wantTileSQL(ts, 0, from, to) {
		t.Fatalf("queries %+v, want the tile's own window", qs)
	}
}

// slicesEqual compares float slices exactly.
func slicesEqual(a, b []float64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// The live run's window is not cut at its start: a tile before the start is still
// queried, so an empty live tile is never cached for want of its frames.
func TestLiveRunWindowIsNotClampedAtItsStart(t *testing.T) {
	signal, ts := plainSignal(t)
	index := pastIndex(0)
	from, to := tileBounds(0, index)
	msFrom := from / nsPerMs
	f := &fakeQueries{span: spanReply(msFrom+20000, msFrom+30000)}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))
	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	readTile(t, conn, 1)
	if qs := f.queries(); len(qs) != 1 || qs[0].SQL != wantTileSQL(ts, 0, from, to) {
		t.Fatalf("queries %+v, want the tile's own window", qs)
	}
}
