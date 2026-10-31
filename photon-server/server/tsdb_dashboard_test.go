package main

import (
	"context"
	"encoding/binary"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func fakeHistoryChunk(queryID, signalID uint32) []byte {
	b := make([]byte, 12)
	binary.LittleEndian.PutUint32(b[0:4], queryID)
	binary.LittleEndian.PutUint32(b[4:8], signalID)
	return b
}

func TestRewriteChunkStampsQueryAndFinal(t *testing.T) {
	got := rewriteChunk(fakeHistoryChunk(1, 9), 42, true)
	if binary.LittleEndian.Uint32(got[0:4]) != 42 || binary.LittleEndian.Uint32(got[4:8]) != 9 {
		t.Fatalf("ids %v", got[:8])
	}
	if binary.LittleEndian.Uint16(got[10:12])&historyChunkFinal == 0 {
		t.Fatal("final bit clear")
	}
	cleared := rewriteChunk(got, 42, false)
	if binary.LittleEndian.Uint16(cleared[10:12])&historyChunkFinal != 0 {
		t.Fatal("final bit stuck")
	}
	if binary.LittleEndian.Uint32(fakeHistoryChunk(1, 9)[0:4]) != 1 {
		t.Fatal("rewrite mutated the original")
	}
}

func TestFanoutCarriesUnaskedSignalsOnTheFirstQuery(t *testing.T) {
	header := historyHeader{OK: true, Series: []historySeriesMeta{
		{SignalID: 1, Points: 4, Level: 0},
		{SignalID: 2, Points: 4, Level: 0},
		{SignalID: 7, Points: 4, Level: 0},
	}}
	chunks := [][]byte{fakeHistoryChunk(99, 7), fakeHistoryChunk(99, 1), fakeHistoryChunk(99, 2)}
	out := fanoutHistory(header, chunks, []historyWaiter{
		{id: 10, asked: []uint32{1}},
		{id: 11, asked: []uint32{2}},
	}, true)
	if len(out) != 2 || len(out[0].chunks) != 2 || len(out[1].chunks) != 1 {
		t.Fatalf("sizes %d %d", len(out[0].chunks), len(out[1].chunks))
	}
	if binary.LittleEndian.Uint32(out[0].chunks[0][4:8]) != 7 || binary.LittleEndian.Uint32(out[0].chunks[1][4:8]) != 1 {
		t.Fatalf("anchor order")
	}
	if binary.LittleEndian.Uint32(out[0].chunks[0][0:4]) != 10 || binary.LittleEndian.Uint16(out[0].chunks[0][10:12])&historyChunkFinal != 0 {
		t.Fatal("extra chunk was finalized early")
	}
	if binary.LittleEndian.Uint16(out[0].chunks[1][10:12])&historyChunkFinal == 0 {
		t.Fatal("anchor did not close the query")
	}
	if binary.LittleEndian.Uint32(out[1].chunks[0][0:4]) != 11 || binary.LittleEndian.Uint32(out[1].chunks[0][4:8]) != 2 {
		t.Fatal("second query")
	}
}

func TestFetchRawSplitsARolledUpCell(t *testing.T) {
	var calls int
	run := func(_ context.Context, _ uint32, from, to uint64, max int, ids []uint32) (historyHeader, [][]byte, error) {
		calls++
		level := 1
		if max >= historyRawMaxPoints && to-from <= historyRawSliceMs {
			level = 0
		}
		h := historyHeader{OK: true}
		var chunks [][]byte
		for _, id := range ids {
			h.Series = append(h.Series, historySeriesMeta{SignalID: id, Level: level, Points: 3})
			if level == 0 {
				chunks = append(chunks, fakeHistoryChunk(1, id))
			}
		}
		h.Chunks = len(chunks)
		return h, chunks, nil
	}
	h, chunks, err := fetchRawLevels(context.Background(), run, 1, 0, historyRawSliceMs*3, []uint32{8})
	if err != nil {
		t.Fatal(err)
	}
	if calls != 4 {
		t.Fatalf("calls %d", calls)
	}
	if len(chunks) != 3 {
		t.Fatalf("chunks %d", len(chunks))
	}
	if level, ok := seriesLevel(h, 8); !ok || level != 0 {
		t.Fatalf("level %d %v", level, ok)
	}
}

func TestDashboardIDsFollowTheMap(t *testing.T) {
	dir := t.TempDir()
	body := `[
		{"id":5,"message":"BPS_Status","name":"Main_Battery_Voltage"},
		{"id":8,"message":"BPS_Voltage_Aggregate_Arr","name":"BPS_Tap_idx"},
		{"id":9,"message":"Not_A_Gauge","name":"Ignored"}
	]`
	if err := os.WriteFile(filepath.Join(dir, "signals.json"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	all, raw := dashboardSignals(dir)
	if len(all) != 2 || all[0] != 5 || all[1] != 8 || len(raw) != 1 || raw[0] != 8 {
		t.Fatalf("all %v raw %v", all, raw)
	}
	if len(loadDashboardNames()) < 140 {
		t.Fatalf("names %d", len(loadDashboardNames()))
	}
	if historyQuerySlots < 16 {
		t.Fatal(historyQuerySlots)
	}
}

func TestCoalesceServesThePinBurstFromOneRead(t *testing.T) {
	resetHistoryBatches()
	t.Cleanup(resetHistoryBatches)
	old := historyBatchWait
	historyBatchWait = 80 * time.Millisecond
	t.Cleanup(func() { historyBatchWait = old })

	dir := t.TempDir()
	body := `[
		{"id":5,"message":"BPS_Status","name":"Main_Battery_Voltage"},
		{"id":8,"message":"BPS_Voltage_Aggregate_Arr","name":"BPS_Voltage_Tap_Data"}
	]`
	if err := os.WriteFile(filepath.Join(dir, "signals.json"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	dashboardIDCache.Delete(dir)

	var mu sync.Mutex
	var calls [][]uint32
	run := func(_ context.Context, queryID uint32, _, _ uint64, max int, ids []uint32) (historyHeader, [][]byte, error) {
		mu.Lock()
		calls = append(calls, append([]uint32(nil), ids...))
		mu.Unlock()
		h := historyHeader{OK: true}
		var chunks [][]byte
		for _, id := range ids {
			level := 0
			if id == 8 && max < historyRawMaxPoints {
				level = 1
			}
			h.Series = append(h.Series, historySeriesMeta{SignalID: id, Level: level, Points: 2})
			chunks = append(chunks, fakeHistoryChunk(queryID, id))
		}
		h.Chunks = len(chunks)
		return h, chunks, nil
	}

	ctx := context.Background()
	start := make(chan struct{})
	var wg sync.WaitGroup
	type reply struct {
		chunks [][]byte
		err    error
	}
	replies := make([]reply, 2)
	for i, id := range []uint32{3, 4} {
		wg.Add(1)
		go func(i int, id uint32) {
			defer wg.Done()
			<-start
			q := browserControl{QueryID: id, From: 1000, To: 5000, MaxPoints: 2000, SignalIDs: []uint32{id}}
			_, chunks, err := coalesceHistory(ctx, dir, q, run)
			replies[i] = reply{chunks, err}
		}(i, id)
	}
	close(start)
	wg.Wait()
	for i, r := range replies {
		if r.err != nil {
			t.Fatalf("reply %d: %v", i, r.err)
		}
	}
	mu.Lock()
	n := len(calls)
	mu.Unlock()
	// One shared read, then one raw re-read of the cell mux.
	if n != 2 {
		t.Fatalf("store reads %d", n)
	}
	ids := map[uint32]bool{}
	for _, r := range replies {
		for _, chunk := range r.chunks {
			ids[binary.LittleEndian.Uint32(chunk[4:8])] = true
			qid := binary.LittleEndian.Uint32(chunk[0:4])
			if qid != 3 && qid != 4 {
				t.Fatalf("query id %d", qid)
			}
		}
	}
	if !ids[3] || !ids[4] || !ids[5] || !ids[8] {
		t.Fatalf("signals %v", ids)
	}

	q := browserControl{QueryID: 6, From: 1000, To: 5000, MaxPoints: 2000, SignalIDs: []uint32{5}}
	_, chunks, err := coalesceHistory(ctx, dir, q, run)
	if err != nil {
		t.Fatal(err)
	}
	mu.Lock()
	n = len(calls)
	mu.Unlock()
	if n != 2 {
		t.Fatalf("cache missed, reads %d", n)
	}
	if len(chunks) != 1 || binary.LittleEndian.Uint32(chunks[0][4:8]) != 5 || binary.LittleEndian.Uint32(chunks[0][0:4]) != 6 {
		t.Fatalf("cached chunk %+v", chunks)
	}
}
