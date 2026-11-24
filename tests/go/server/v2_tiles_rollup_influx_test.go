package main

import (
	"context"
	"fmt"
	"os"
	"testing"
	"time"

	"server/dbc"
)

// TestTilesFromRollupIntoRealInfluxDB writes 25 seconds of a two-signal message, ten
// frames a second, through the real writer. Then it reads a 1 s tile and a 10 s tile
// over a hub, which read rollup_1s, and checks every bucket's min, max and mean
// against the raw frames. It runs only when PHOTON_TEST_INFLUX names an InfluxDB 3,
// for example http://127.0.0.1:8181. The database is deleted afterwards.
func TestTilesFromRollupIntoRealInfluxDB(t *testing.T) {
	url := os.Getenv("PHOTON_TEST_INFLUX")
	if url == "" {
		t.Skip("PHOTON_TEST_INFLUX is not set")
	}
	db := fmt.Sprintf("photon_test_%d", time.Now().UnixNano())
	t.Cleanup(func() { dropTestDatabase(t, url, db) })
	cat, err := dbc.Load(writeFrameTestDBC(t), nil)
	if err != nil {
		t.Fatal(err)
	}
	layouts := buildLayouts(cat)
	zetaIdx, zeta := findSignal(t, cat, func(ts tileSignal) bool { return ts.msg.Name == "Plain" && ts.sig.Name == "Zeta" })
	speedIdx, speed := findSignal(t, cat, func(ts tileSignal) bool { return ts.msg.Name == "Plain" && ts.sig.Name == "Speed" })

	// A 10 s tile starts this far before now, and the data starts 5 s into it: all of
	// it is in that tile, and in the 1 s tile that starts with it.
	const span10 = 10240_000 // ms in a level 4 tile
	tileStart := (time.Now().UnixMilli()/span10 - 2) * span10
	const seconds = 25
	type sample struct {
		zeta  uint8
		speed float64
		sec   int // second of the data, 0 to 24
	}
	var samples []sample

	w, err := newDBWriter(url, db, "", dbWriterOptions{Flush: DefaultInfluxFlush, Block: true})
	if err != nil {
		t.Fatal(err)
	}
	s := newFrameSession(time.Now(), w)
	sid := s.sid
	for k := range seconds {
		for j := range 10 {
			z := uint8((k*7 + j*3) % 200)
			sp := float32(k) + float32(j)*0.5
			tNS := (tileStart + 5000 + int64(k)*1000 + int64(j)*100) * nsPerMs
			s.appendFrame(layouts[100], plainFrame(z, 0, 0, sp), tNS)
			samples = append(samples, sample{zeta: z, speed: float64(sp), sec: k})
		}
	}
	s.Close(10 * time.Second)
	if n := w.Dropped(); n != 0 {
		t.Fatalf("the writer dropped %d rows", n)
	}

	h := newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), func() string { return "" },
		newLiveRing(liveRingMinFrames), newInfluxQuerier(url, db, "", 4), nil)
	h.SetCatalog(cat)
	conn := tileConn(t, h)
	var qid uint32
	fetch := func(signal int64, level int, index int64) v2Tile {
		qid++
		requestTile(t, conn, qid, sid, signal, level, index)
		return joinTile(t, readTile(t, conn, qid))
	}
	// The rows are queryable a moment after the writer closes, so wait for them.
	index1 := tileStart / tileSpanMS(3)
	deadline := time.Now().Add(20 * time.Second)
	for fetch(zetaIdx, 3, index1).N != seconds && time.Now().Before(deadline) {
		time.Sleep(250 * time.Millisecond)
	}
	// The tiles must be answered from rollup_1s: it holds one row per second of the run,
	// so the fallback to the frame rows is not what answered them.
	counts, err := newInfluxQuerier(url, db, "", 1).query(context.Background(),
		"SELECT count(*) AS n FROM rollup_1s WHERE session = $sid", map[string]string{"sid": sid})
	if err != nil || len(counts) != 1 {
		t.Fatalf("rollup count: %v (%d rows)", err, len(counts))
	}
	if n, err := rowInt(counts[0], "n"); err != nil || n != seconds {
		t.Fatalf("rollup_1s holds %d rows for the run (%v), want %d", n, err, seconds)
	}

	// One bucket a second: the min, max and mean of its ten frames.
	for _, sig := range []struct {
		idx int64
		ts  tileSignal
		raw func(sample) float64
	}{
		{zetaIdx, zeta, func(s sample) float64 { return float64(s.zeta) }},
		{speedIdx, speed, func(s sample) float64 { return s.speed }},
	} {
		tile := fetch(sig.idx, 3, index1)
		if tile.N != seconds || tile.Level != 3 {
			t.Fatalf("%s: 1 s tile holds %d buckets, want %d", sig.ts.sig.Name, tile.N, seconds)
		}
		for k := range seconds {
			lo, hi, sum, n := 0.0, 0.0, 0.0, 0.0
			for _, sm := range samples {
				if sm.sec != k {
					continue
				}
				v := sig.raw(sm)
				if n == 0 || v < lo {
					lo = v
				}
				if n == 0 || v > hi {
					hi = v
				}
				sum += v
				n++
			}
			wantLo, wantHi := physicalRange(sig.ts, lo, hi)
			if tile.Min[k] != wantLo || tile.Max[k] != wantHi || tile.Mean[k] != physical(sig.ts, sum/n) {
				t.Fatalf("%s second %d: min %v max %v mean %v, want %v %v %v", sig.ts.sig.Name, k,
					tile.Min[k], tile.Max[k], tile.Mean[k], wantLo, wantHi, physical(sig.ts, sum/n))
			}
		}
	}

	// The data starts 5 s into the tile, so its 10 s buckets hold seconds 0 to 4, 5 to
	// 14 and 15 to 24 of the data: bucket (5 + second) / 10.
	index10 := tileStart / tileSpanMS(4)
	tile := fetch(zetaIdx, 4, index10)
	if tile.N != 3 || tile.Level != 4 {
		t.Fatalf("10 s tile holds %d buckets, want 3", tile.N)
	}
	for b := range 3 {
		lo, hi, sum, n := 0.0, 0.0, 0.0, 0.0
		for _, sm := range samples {
			if (5+sm.sec)/10 != b {
				continue
			}
			v := float64(sm.zeta)
			if n == 0 || v < lo {
				lo = v
			}
			if n == 0 || v > hi {
				hi = v
			}
			sum += v
			n++
		}
		if tile.Min[b] != lo || tile.Max[b] != hi || tile.Mean[b] != sum/n {
			t.Fatalf("10 s bucket %d: min %v max %v mean %v, want %v %v %v", b, tile.Min[b], tile.Max[b], tile.Mean[b], lo, hi, sum/n)
		}
	}
}
