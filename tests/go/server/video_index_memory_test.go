package main

import (
	"bytes"
	"os"
	"runtime"
	"testing"
)

// TestVideoIndexMemoryPerHour stores an hour of 24 fps video with a keyframe every
// 2 s and logs how much heap the index adds per hour. It takes minutes of fsyncs,
// so it runs only when PHOTON_VIDEO_MEMORY_TEST is set. The heap is read at 15 and
// at 60 minutes: the hot cache is full by then, so the difference is the index.
func TestVideoIndexMemoryPerHour(t *testing.T) {
	if os.Getenv("PHOTON_VIDEO_MEMORY_TEST") == "" {
		t.Skip("set PHOTON_VIDEO_MEMORY_TEST=1 to measure the index per hour")
	}
	store, err := OpenVideoStore(t.TempDir())
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	const fps = 24
	const hour = 60 * 60 * fps
	const start = uint64(1_700_000_000_000)
	heap := func() uint64 {
		runtime.GC()
		runtime.GC()
		var m runtime.MemStats
		runtime.ReadMemStats(&m)
		return m.HeapAlloc
	}

	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	mustAppend(t, store, sps, start)
	mustAppend(t, store, pps, start)
	pFrame := annexBNAL(1, bytes.Repeat([]byte{0x55}, 1019)...)
	idr := annexBNAL(nalTypeIDR, bytes.Repeat([]byte{0x66}, 1019)...)

	var at15 uint64
	for i := 0; i < hour; i++ {
		ts := start + uint64(i)*1000/fps
		nal := pFrame
		if i%(2*fps) == 0 {
			nal = idr
		}
		if _, err := store.Append(nal, ts); err != nil {
			t.Fatalf("append %d: %v", i, err)
		}
		if i == 15*60*fps {
			at15 = heap()
		}
	}
	at60 := heap()
	perHour := float64(at60-at15) / 0.75
	stats := store.Stats()
	t.Logf("video index memory: heap at 15 min %d B, at 60 min %d B; index growth %.0f B per hour of video",
		at15, at60, perHour)
	t.Logf("video index memory: %d NALs, %d GOP rows, %d segments, %d B on disk",
		hour+2, len(store.PointerRows()), stats.SegmentCount, stats.BytesStored)
}
