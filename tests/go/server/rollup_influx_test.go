package main

import (
	"context"
	"fmt"
	"os"
	"testing"
	"time"
)

// TestRollupRowsIntoRealInfluxDB writes ten frames a second for four seconds of a
// two-signal message through the real writer, then reads rollup_1s back. Each second's
// row must hold the min, max, sum and count of the frames of that second, for an
// integer and a float signal. It runs only when PHOTON_TEST_INFLUX names an InfluxDB
// 3, for example http://127.0.0.1:8181. The database is deleted afterwards.
func TestRollupRowsIntoRealInfluxDB(t *testing.T) {
	url := os.Getenv("PHOTON_TEST_INFLUX")
	if url == "" {
		t.Skip("PHOTON_TEST_INFLUX is not set")
	}
	db := fmt.Sprintf("photon_test_%d", time.Now().UnixNano())
	t.Cleanup(func() { dropTestDatabase(t, url, db) })
	layouts := rollupTestLayouts(t)
	plain := layouts[100]

	w, err := newDBWriter(url, db, "", dbWriterOptions{Flush: DefaultInfluxFlush, Block: true})
	if err != nil {
		t.Fatal(err)
	}
	s := newFrameSession(time.Now(), w)
	sid := s.sid
	// Whole seconds, a minute back, so the rows are sealed long before they are read.
	t0 := (time.Now().UnixMilli()/1000 - 60) * 1000
	for k := range 4 {
		for j := range 10 {
			tNS := (t0 + int64(k)*1000 + int64(j)*100) * nsPerMs
			s.appendFrame(plain, plainFrame(uint8(10*k+j), 0, 0, float32(k)+float32(j)*0.5), tNS)
		}
	}
	s.Close(10 * time.Second)
	if n := w.Dropped(); n != 0 {
		t.Fatalf("the writer dropped %d rows", n)
	}
	time.Sleep(1500 * time.Millisecond) // InfluxDB makes writes queryable once its WAL flushes

	q := newInfluxQuerier(url, db, "", 1)
	rows, err := q.query(context.Background(),
		`SELECT CAST(time AS BIGINT) AS t_ns, "Zeta_min", "Zeta_max", "Zeta_sum", "Zeta_n", `+
			`"Speed_min", "Speed_max", "Speed_sum", "Speed_n" FROM rollup_1s WHERE session = $sid AND msg = 'Plain' ORDER BY time`,
		map[string]string{"sid": sid})
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 4 {
		t.Fatalf("%d rollup rows, want one per second (4)", len(rows))
	}
	for k, r := range rows {
		ns, err := rowInt(r, "t_ns")
		if err != nil || ns != (t0+int64(k)*1000)*nsPerMs {
			t.Fatalf("row %d time %d (%v), want second %d", k, ns, err, t0+int64(k)*1000)
		}
		// Zeta is 10k to 10k+9 with sum 100k+45; Speed is k to k+4.5 with sum 10k+22.5.
		kf := float64(k)
		want := map[string]float64{
			"Zeta_min": float64(10 * k), "Zeta_max": float64(10*k + 9), "Zeta_sum": float64(100*k + 45), "Zeta_n": 10,
			"Speed_min": kf, "Speed_max": kf + 4.5, "Speed_sum": 10*kf + 22.5, "Speed_n": 10,
		}
		for field, v := range want {
			got, err := rowFloat(r, field)
			if err != nil || got != v {
				t.Fatalf("second %d: %s = %v (%v), want %v", k, field, r[field], err, v)
			}
		}
	}
}
