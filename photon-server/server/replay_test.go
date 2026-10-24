package main

import (
	"encoding/csv"
	"math"
	"os"
	"server/cache"
	"strconv"
	"testing"
)

// The Brainerd clip is a VSD1 file of CANP batches; the decoder's resync
// steps over the wrapper. Latitude per second should match the CSV export.
func TestReplayBrainerdClip(t *testing.T) {
	const dir = "../../examples/brainerd-30s/"
	raw, err := os.ReadFile(dir + "2026-07-23-03-30s.log")
	if err != nil {
		t.Skip(err)
	}
	c, err := cache.LoadDBC(dir + "HighNoon.dbc")
	if err != nil {
		t.Fatal(err)
	}
	var d ingressDecoder
	frames := d.Decode(raw)
	if len(frames) == 0 {
		t.Fatal("no frames decoded")
	}
	for _, fr := range frames {
		c.Fold(fr)
	}

	f, err := os.Open(dir + "2026-07-23-03-30s.csv")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	rows, err := csv.NewReader(f).ReadAll()
	if err != nil {
		t.Fatal(err)
	}
	col := map[string]int{}
	for i, name := range rows[0] {
		col[name] = i
	}

	key := cache.SampleKey{Signal: cache.SignalID{Name: "GPS_Latitude.Latitude"}, Tap: -1}
	matched := 0
	for _, row := range rows[1:] {
		tsMs, _ := strconv.ParseInt(row[col["ts_ms"]], 10, 64)
		want, _ := strconv.ParseFloat(row[col["lat"]], 64)
		key.Second = tsMs / 1000
		smp, ok := c.Samples[key]
		if !ok {
			continue
		}
		got := float64(smp.Sum) / float64(smp.Count) * 0.000001
		if math.Abs(got-want) > 1e-3 {
			t.Errorf("second %d: latitude %.6f, CSV %.4f", key.Second, got, want)
		}
		matched++
	}
	if matched < len(rows[1:])/2 {
		t.Fatalf("only %d of %d CSV seconds had a latitude bucket (%d frames decoded)", matched, len(rows)-1, len(frames))
	}
	t.Logf("%d frames, %d/%d seconds matched", len(frames), matched, len(rows)-1)
}
