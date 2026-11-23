package main

import (
	"fmt"
	"math"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

	"server/dbc"
)

// setSignalRaw writes raw into the bits of data that s occupies: the inverse of
// dbc.Signal.Raw.
func setSignalRaw(data []byte, s *dbc.Signal, raw uint64) {
	n := int(s.Bits)
	bit := int(s.Start)
	for j := 0; j < n; j++ {
		var v uint64
		if s.LittleEndian {
			v = raw >> j & 1 // Intel: bit j of raw sits at Start+j
		} else {
			v = raw >> (n - 1 - j) & 1 // Motorola: the most significant bit comes first
		}
		if v == 1 {
			data[bit/8] |= 1 << (bit % 8)
		} else {
			data[bit/8] &^= 1 << (bit % 8)
		}
		switch {
		case s.LittleEndian:
			bit++
		case bit%8 == 0:
			bit += 15
		default:
			bit--
		}
	}
}

// findSignal returns the catalog index and the signal of the first signal that pred
// accepts.
func findSignal(t *testing.T, cat *dbc.Catalog, pred func(tileSignal) bool) (int64, tileSignal) {
	t.Helper()
	for i, ts := range catalogSignals(cat) {
		if pred(ts) {
			return int64(i), ts
		}
	}
	t.Fatal("no signal in the spec matches")
	return 0, tileSignal{}
}

// TestHistoryFromRealInfluxDB writes frames through the real frame writer into a
// fresh database, then reads them back over a hub: the runs list, and level 0 and
// level 1 tiles of a float, a plain integer and a muxed integer signal. It runs
// only when PHOTON_TEST_INFLUX names an InfluxDB 3, for example
// http://127.0.0.1:8181. The database is deleted afterwards: InfluxDB 3 Core
// allows only 5 databases, so leaving one per run soon makes runs fail.
func TestHistoryFromRealInfluxDB(t *testing.T) {
	url := os.Getenv("PHOTON_TEST_INFLUX")
	if url == "" {
		t.Skip("PHOTON_TEST_INFLUX is not set")
	}
	cat := loadTestCatalog(t)
	db := fmt.Sprintf("photon_test_%d", time.Now().UnixNano())
	t.Cleanup(func() {
		req, err := http.NewRequest(http.MethodDelete, strings.TrimRight(url, "/")+"/api/v3/configure/database?db="+db, nil)
		if err != nil {
			t.Errorf("delete %s: %v", db, err)
			return
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Errorf("delete %s: %v", db, err)
			return
		}
		resp.Body.Close()
		// 404: nothing was written, so there was no database to delete.
		if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusNotFound {
			t.Errorf("delete %s: HTTP %d", db, resp.StatusCode)
		}
	})

	// Each message gets 50 frames, 10 ms apart, two hours back. t0 starts a level 0
	// tile and a level 1 tile together: it is a multiple of both spans.
	const lcmMS = 7_680_000
	t0 := (time.Now().UnixMilli()/lcmMS - 2) * lcmMS

	fi, fl := findSignal(t, cat, func(ts tileSignal) bool {
		return ts.msg.Name == "MC_VelocityMeasurement" && ts.sig.Name == "MC_MotorVelocity"
	})
	ii, in := findSignal(t, cat, func(ts tileSignal) bool {
		return ts.sig.Kind == dbc.Integer && ts.sig.Mux < 0 && ts.msg.Selector < 0 && len(ts.msg.Groups) == 0 &&
			ts.sig.Bits >= 8 && ts.msg.Length <= 8 && ts.msg.Name != "MC_VelocityMeasurement"
	})
	mi, mx := findSignal(t, cat, func(ts tileSignal) bool {
		return ts.sig.Kind == dbc.Integer && ts.sig.Mux >= 0 && ts.msg.Selector >= 0 && len(ts.msg.Groups) == 0 &&
			ts.sig.Bits >= 8 && ts.msg.Length <= 8 && ts.msg.ID != in.msg.ID
	})

	w, err := newDBWriter(url, db, "", dbWriterOptions{Flush: DefaultInfluxFlush})
	if err != nil {
		t.Fatal(err)
	}
	s := newFrameSession(time.Now(), w)
	sid := s.sid
	s.appendSignalDefs(cat, t0)
	layouts := buildLayouts(cat)
	sel := &mx.msg.Signals[mx.msg.Selector]
	wantIn := make([]float64, 0, 50)
	wantMux := make([]float64, 0, 50)
	for k := range 50 {
		tNS := (t0 + int64(k)*10) * nsPerMs

		d := make([]byte, 8)
		setSignalRaw(d, fl.sig, uint64(math.Float32bits(float32(k)*0.25)))
		s.appendFrame(layouts[fl.msg.ID], d, tNS)

		d = make([]byte, 8)
		setSignalRaw(d, in.sig, uint64(k))
		v, _ := in.sig.RawInt(d)
		wantIn = append(wantIn, physical(in, float64(v)))
		s.appendFrame(layouts[in.msg.ID], d, tNS)

		d = make([]byte, 8)
		setSignalRaw(d, sel, uint64(mx.sig.Mux))
		setSignalRaw(d, mx.sig, uint64(k))
		v, _ = mx.sig.RawInt(d)
		wantMux = append(wantMux, physical(mx, float64(v)))
		s.appendFrame(layouts[mx.msg.ID], d, tNS)
	}
	s.Close(5 * time.Second)
	if n := w.dropped.Load(); n != 0 {
		t.Fatalf("the writer dropped %d rows", n)
	}

	h := newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), func() string { return "" },
		newLiveRing(liveRingMinFrames), newInfluxQuerier(url, db, "", 4))
	h.SetCatalog(cat)
	conn := tileConn(t, h)

	var qid uint32
	fetch := func(signal int64, level int, index int64) v2Tile {
		qid++
		requestTile(t, conn, qid, sid, signal, level, index)
		return joinTile(t, readTile(t, conn, qid))
	}
	// The rows are queryable a moment after the writer closes, so wait for them.
	deadline := time.Now().Add(20 * time.Second)
	for fetch(fi, 0, t0/tileSpanMS(0)).N != 50 && time.Now().Before(deadline) {
		time.Sleep(250 * time.Millisecond)
	}

	// The run, from the session row.
	var runs runsReply
	sendV2(t, conn, map[string]any{"type": "runs_list"})
	readInto(t, conn, &runs)
	found := false
	for _, r := range runs.Runs {
		if r.ID != sid {
			continue
		}
		found = true
		if r.StartMS != t0 || r.EndMS != t0+490 || r.Source != "car" || r.Live {
			t.Fatalf("run %+v, want start %d, end %d, source car, not live", r, t0, t0+490)
		}
	}
	if !found {
		t.Fatalf("runs %+v do not include session %s", runs.Runs, sid)
	}

	// Level 0: every raw sample, at its own time.
	checkRaw := func(name string, tile v2Tile, signal int64, want []float64) {
		t.Helper()
		if tile.N != 50 || !tile.Sealed || tile.Signal != uint32(signal) {
			t.Fatalf("%s: tile holds %d samples, sealed %v, signal %d; want 50 sealed", name, tile.N, tile.Sealed, tile.Signal)
		}
		for k := range 50 {
			if tile.TMS[k] != float64(t0+int64(k)*10) || tile.Raw[k] != want[k] {
				t.Fatalf("%s sample %d = (%v, %v), want (%d, %v)", name, k, tile.TMS[k], tile.Raw[k], t0+int64(k)*10, want[k])
			}
		}
	}
	wantFloat := make([]float64, 50)
	for k := range wantFloat {
		wantFloat[k] = physical(fl, float64(k)*0.25)
	}
	checkRaw("float", fetch(fi, 0, t0/tileSpanMS(0)), fi, wantFloat)
	checkRaw("integer", fetch(ii, 0, t0/tileSpanMS(0)), ii, wantIn)
	checkRaw("muxed integer", fetch(mi, 0, t0/tileSpanMS(0)), mi, wantMux)

	// Level 1: 10 ms buckets with one frame each, so the min, max and mean are the frame's value.
	ten := fetch(fi, 1, t0/tileSpanMS(1))
	if ten.N != 50 || ten.Level != 1 {
		t.Fatalf("level 1 tile holds %d buckets, want 50", ten.N)
	}
	for b := range 50 {
		want := physical(fl, float64(b)*0.25)
		if ten.TMS[b] != float64(t0+int64(b)*10) || ten.Min[b] != want || ten.Max[b] != want || ten.Mean[b] != want {
			t.Fatalf("10 ms bucket %d = (%v, %v, %v, %v), want %v", b, ten.TMS[b], ten.Min[b], ten.Max[b], ten.Mean[b], want)
		}
	}

	// Level 2: 100 ms buckets, each with the min, max and mean of its 10 frames.
	tile := fetch(fi, 2, t0/tileSpanMS(2))
	if tile.N != 5 || tile.Level != 2 {
		t.Fatalf("level 2 tile holds %d buckets, want 5", tile.N)
	}
	for b := range 5 {
		lo := float64(b) * 2.5
		minV, maxV := physicalRange(fl, lo, lo+2.25)
		mean := physical(fl, lo+1.125)
		if tile.TMS[b] != float64(t0+int64(b)*100) || tile.Min[b] != minV || tile.Max[b] != maxV || tile.Mean[b] != mean {
			t.Fatalf("bucket %d = (%v, %v, %v, %v), want (%d, %v, %v, %v)", b,
				tile.TMS[b], tile.Min[b], tile.Max[b], tile.Mean[b], t0+int64(b)*100, minV, maxV, mean)
		}
	}
}
