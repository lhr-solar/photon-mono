package main

import (
	"encoding/binary"
	"testing"
	"time"
)

// liveBatch is one CANP v3 batch with one frame per ID, all stamped tsMS. Each
// frame has eight data bytes, counting up from its position in the batch.
func liveBatch(tsMS uint64, ids []uint32) []byte {
	b := binary.BigEndian.AppendUint32(nil, canpMagic)
	b = binary.BigEndian.AppendUint16(b, canpVersion)
	b = binary.BigEndian.AppendUint16(b, uint16(len(ids)))
	b = binary.BigEndian.AppendUint32(b, 0)
	b = binary.BigEndian.AppendUint64(b, tsMS)
	for i, id := range ids {
		rec := make([]byte, canpFrameSize)
		binary.BigEndian.PutUint32(rec, id)
		rec[4] = 8
		for j := range 8 {
			rec[5+j] = byte(i + j)
		}
		b = append(b, rec...)
	}
	return b
}

// repeatID is n copies of id.
func repeatID(id uint32, n int) []uint32 {
	ids := make([]uint32, n)
	for i := range ids {
		ids[i] = id
	}
	return ids
}

// nowMS is the current time in the CANP clock's units.
func nowMS() uint64 { return uint64(time.Now().UnixMilli()) }

// anyID keeps every frame.
func anyID(uint32) bool { return true }

func TestLiveRingKeepsFramesInOrder(t *testing.T) {
	r := newLiveRing(1024)
	r.appendBuffer(liveBatch(nowMS(), []uint32{0x100, 0x101, canpExtended | 0x18FEF100}))
	r.appendBuffer(liveBatch(nowMS(), []uint32{0x200}))

	frames, next, oldest, _ := r.read(0, make([]liveFrame, 0, 16), anyID)
	if len(frames) != 4 || next != 4 || oldest != 0 {
		t.Fatalf("read gave %d frames, next %d, oldest %d; want 4, 4, 0", len(frames), next, oldest)
	}
	wantIDs := []uint32{0x100, 0x101, canpExtended | 0x18FEF100, 0x200}
	for i, f := range frames {
		if f.id != wantIDs[i] {
			t.Fatalf("frame %d has ID %#x, want %#x", i, f.id, wantIDs[i])
		}
	}
	if frames[0].n != 8 || frames[0].data != [8]byte{0, 1, 2, 3, 4, 5, 6, 7} {
		t.Fatalf("frame 0 = %+v, want 8 bytes counting up", frames[0])
	}
}

// A read stops when dst is full, and next picks up after the last frame looked
// at, whether it was kept or not.
func TestLiveRingFilterAndResume(t *testing.T) {
	r := newLiveRing(1024)
	ids := make([]uint32, 10)
	for i := range ids {
		ids[i] = 0x100 + uint32(i)
	}
	r.appendBuffer(liveBatch(nowMS(), ids))
	even := func(id uint32) bool { return id%2 == 0 }
	dst := make([]liveFrame, 0, 3)

	frames, next, _, _ := r.read(0, dst, even)
	if len(frames) != 3 || next != 5 || frames[2].id != 0x104 {
		t.Fatalf("first read: %d frames, next %d; want 3 frames ending at 0x104, next 5", len(frames), next)
	}
	frames, next, _, _ = r.read(next, dst, even)
	if len(frames) != 2 || next != 10 || frames[0].id != 0x106 || frames[1].id != 0x108 {
		t.Fatalf("second read: %d frames, next %d; want 0x106 and 0x108, next 10", len(frames), next)
	}
}

// Frames whose car time is before the clock was set, or far ahead, are not kept.
func TestLiveRingDropsBadClockFrames(t *testing.T) {
	r := newLiveRing(1024)
	r.appendBuffer(testBatch(3, 1000))                                       // 1970: car clock not set
	r.appendBuffer(testBatch(2, nowMS()+uint64(time.Hour/time.Millisecond))) // an hour ahead
	r.appendBuffer(liveBatch(nowMS(), []uint32{0x104}))                      // good

	if _, head := r.stats(); head != 1 {
		t.Fatalf("head = %d, want 1 (only the good frame is kept)", head)
	}
	frames, _, _, _ := r.read(0, make([]liveFrame, 0, 4), anyID)
	if len(frames) != 1 || frames[0].id != 0x104 {
		t.Fatalf("kept %+v, want only ID 0x104", frames)
	}
}

// Past its capacity the ring overwrites its oldest frames. A reader that is
// behind them is told so, and a reader at the oldest frame gets a full ring.
func TestLiveRingOverwritesOldest(t *testing.T) {
	r := newLiveRing(1024)
	ids := make([]uint32, 3000)
	for i := range ids {
		ids[i] = uint32(i)
	}
	r.appendBuffer(liveBatch(nowMS(), ids))

	frames, next, oldest, _ := r.read(0, make([]liveFrame, 0, 2000), anyID)
	if len(frames) != 0 || next != 0 || oldest != 1976 {
		t.Fatalf("read from 0: %d frames, next %d, oldest %d; want none, 0, 1976", len(frames), next, oldest)
	}
	frames, next, _, _ = r.read(oldest, make([]liveFrame, 0, 2000), anyID)
	if len(frames) != 1024 || next != 3000 || frames[0].id != 1976 {
		t.Fatalf("read from oldest: %d frames, next %d, first ID %d; want 1024, 3000, 1976", len(frames), next, frames[0].id)
	}
}

// The notify channel closes when frames are added, so a reader that waited on
// it wakes up.
func TestLiveRingWaitClosesOnAppend(t *testing.T) {
	r := newLiveRing(1024)
	frames, next, _, wait := r.read(0, make([]liveFrame, 0, 4), anyID)
	if len(frames) != 0 || next != 0 {
		t.Fatalf("empty ring gave %d frames, next %d", len(frames), next)
	}
	select {
	case <-wait:
		t.Fatal("wait closed before any frame was added")
	default:
	}
	r.appendBuffer(liveBatch(nowMS(), []uint32{0x100}))
	select {
	case <-wait:
	default:
		t.Fatal("wait did not close on the next append")
	}
}

func TestLiveRingTimeAt(t *testing.T) {
	r := newLiveRing(1024)
	ts := nowMS()
	r.appendBuffer(testBatch(3, ts))
	if got := r.timeAt(0); got != int64(ts)*1_000_000 {
		t.Fatalf("timeAt(0) = %d, want %d", got, int64(ts)*1_000_000)
	}
	if got := r.timeAt(3); got != 0 {
		t.Fatalf("timeAt(3) = %d, want 0 (not written yet)", got)
	}
	r.appendBuffer(testBatch(1100, ts)) // overwrites sequence 0
	if got := r.timeAt(0); got != 0 {
		t.Fatalf("timeAt(0) after overwrite = %d, want 0", got)
	}
}

func TestLiveRingFramesFromConfig(t *testing.T) {
	cfg := &Config{Ingest: poolSizing{Rate: DefaultIngestRate, Latency: DefaultIngestLatency, Stall: DefaultIngestStall}, LiveSeconds: 5 * time.Second}
	if got := liveRingFrames(cfg); got != 40_000*5 {
		t.Fatalf("liveRingFrames = %d, want 200000 (40k frames/s for 5 s)", got)
	}
	cfg.LiveSeconds = time.Millisecond
	if got := liveRingFrames(cfg); got != liveRingMinFrames {
		t.Fatalf("liveRingFrames = %d, want the minimum %d", got, liveRingMinFrames)
	}
}
