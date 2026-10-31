package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"os"
	"testing"
	"time"
)

func serveHistoryTestDouble() {
	in := bufio.NewReader(os.Stdin)
	for {
		line, err := in.ReadBytes('\n')
		if err != nil {
			return
		}
		var req struct {
			QueryID uint32   `json:"query_id"`
			IDs     []uint32 `json:"signal_ids"`
		}
		if json.Unmarshal(line, &req) != nil || len(req.IDs) == 0 {
			_, _ = os.Stdout.WriteString("{\"ok\":false,\"error\":\"signal_ids required\"}\n")
			continue
		}
		chunk := []byte{9, 8, 7, 6}
		header := map[string]any{
			"ok": true, "level": 0, "bucket_ms": 0, "truncated": false, "chunks": 1,
			"series": []map[string]any{{
				"signal_id": req.IDs[0], "level": 0, "bucket_ms": 0, "points": 1,
				"t_min": 10, "t_max": 10, "chunks": 1,
				"prior": map[string]any{"t": 9, "v": 1.5},
			}},
		}
		raw, _ := json.Marshal(header)
		_, _ = os.Stdout.Write(append(raw, '\n'))
		var n [4]byte
		binary.LittleEndian.PutUint32(n[:], uint32(len(chunk)))
		_, _ = os.Stdout.Write(n[:])
		_, _ = os.Stdout.Write(chunk)
	}
}

func TestTsdbHistoryProtocol(t *testing.T) {
	historyToolOverride = os.Args[0]
	t.Cleanup(func() {
		historyToolOverride = ""
		closeHistoryProcs()
	})
	proc, err := historyProcFor(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	header, chunks, err := proc.query(ctx, 42, 10, 70, 100, []uint32{7})
	if err != nil {
		t.Fatal(err)
	}
	if !header.OK || header.Chunks != 1 || len(header.Series) != 1 || header.Series[0].SignalID != 7 {
		t.Fatalf("%+v", header)
	}
	if header.Series[0].Prior == nil || header.Series[0].Prior.T != 9 {
		t.Fatalf("prior %+v", header.Series[0].Prior)
	}
	if len(chunks) != 1 || string(chunks[0]) != "\x09\x08\x07\x06" {
		t.Fatalf("%v", chunks)
	}
	// The same process answers a second window without being started again.
	header, chunks, err = proc.query(ctx, 43, 70, 130, 100, []uint32{8})
	if err != nil {
		t.Fatal(err)
	}
	if header.Series[0].SignalID != 8 || len(chunks) != 1 {
		t.Fatalf("%+v %d", header, len(chunks))
	}
}

func TestFramesForRowsSeeks(t *testing.T) {
	s := &VideoStore{hot: map[uint64][]byte{}, segments: map[uint64]*videoSegment{}}
	// Noise in earlier segments, then the GOP the locate asked for.
	for i := 0; i < 50000; i++ {
		s.entries = append(s.entries, videoIndexEntry{seq: uint64(i + 1), segmentID: 1, offset: int64(i * 10), size: 4})
	}
	match := []byte("nal")
	s.entries = append(s.entries, videoIndexEntry{seq: 90001, segmentID: 4, offset: 1000, size: 3, rtpTimestamp: 3000, timestampMs: 50, flags: 1})
	s.entries = append(s.entries, videoIndexEntry{seq: 90002, segmentID: 4, offset: 1003, size: 3, rtpTimestamp: 3000, timestampMs: 51})
	s.entries = append(s.entries, videoIndexEntry{seq: 90003, segmentID: 4, offset: 2000, size: 3, timestampMs: 90})
	s.hot[90001] = match
	s.hot[90002] = match
	s.hot[90003] = match
	for i := 0; i < 10000; i++ {
		s.entries = append(s.entries, videoIndexEntry{seq: uint64(91000 + i), segmentID: 9, offset: int64(i * 8), size: 4})
	}
	got, err := s.framesForRows([]GopRow{{SegmentID: 4, ByteOffset: 1000, ByteLength: 10}})
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 || got[0].seq != 90001 || got[1].seq != 90002 || got[0].rtp != 3000 || string(got[0].frame.Payload) != "nal" {
		t.Fatalf("%+v", got)
	}
}

func TestVideoGroupOverlap(t *testing.T) {
	rows := []GopRow{
		{StreamID: 0, StartMs: 1000, EndMs: 1500},
		{StreamID: 0, StartMs: 5000, EndMs: 5500},
		{StreamID: 1, StartMs: 2000, EndMs: 2600},
	}
	groups := videoStreamGroups(rows)
	if len(groups) != 2 {
		t.Fatalf("groups %d", len(groups))
	}
	if !groupsOverlap(groups, 0, 5200, 5400) {
		t.Fatal("missed stream 0")
	}
	if !groupsOverlap(groups, -5000, 0, 100) {
		t.Fatal("offset should pull the 5000 ms GOP onto the window")
	}
	if groupsOverlap(groups, 0, 3000, 4000) {
		t.Fatal("gap reported as video")
	}
	if groupsOverlap(groups, 0, 10, 20) {
		t.Fatal("before the table")
	}
}
