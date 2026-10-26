package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func annexBNAL(nalType byte, rest ...byte) []byte {
	payload := append([]byte{0x00, 0x00, 0x00, 0x01, nalType}, rest...)
	return payload
}

func TestVideoStoreWriteReadAndKeyframeIndex(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}

	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	idr1 := annexBNAL(nalTypeIDR, 0x01, 0x02)
	p1 := annexBNAL(1, 0x11)
	idr2 := annexBNAL(nalTypeIDR, 0x03, 0x04)
	p2 := annexBNAL(1, 0x22)

	writes := []struct {
		nal []byte
		ts  uint64
	}{
		{sps, 1000},
		{pps, 1000},
		{idr1, 1000},
		{p1, 1033},
		{idr2, 2000},
		{p2, 2033},
	}
	for _, w := range writes {
		if _, err := store.Append(w.nal, w.ts); err != nil {
			t.Fatalf("append ts=%d: %v", w.ts, err)
		}
	}

	kf, seq, ok := store.FindKeyframeAtOrBefore(1500)
	if !ok {
		t.Fatal("expected keyframe <= 1500")
	}
	if kf.TimestampMs != 1000 {
		t.Fatalf("keyframe ts %d, want 1000", kf.TimestampMs)
	}
	if !bytes.Equal(kf.Payload, idr1) {
		t.Fatalf("keyframe payload mismatch: %x", kf.Payload)
	}
	if seq == 0 {
		t.Fatal("expected non-zero keyframe seq")
	}

	kf, _, ok = store.FindKeyframeAtOrBefore(2000)
	if !ok || kf.TimestampMs != 2000 {
		t.Fatalf("expected keyframe at 2000, got ok=%v ts=%d", ok, kf.TimestampMs)
	}
	if !bytes.Equal(kf.Payload, idr2) {
		t.Fatalf("second keyframe payload mismatch")
	}

	if _, _, ok := store.FindKeyframeAtOrBefore(999); ok {
		t.Fatal("expected no keyframe <= 999")
	}

	gotSPS, gotPPS := store.ConfigNALs()
	if !bytes.Equal(gotSPS, sps) || !bytes.Equal(gotPPS, pps) {
		t.Fatalf("SPS/PPS mismatch")
	}

	if err := store.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	reopened, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer reopened.Close()

	kf, _, ok = reopened.FindKeyframeAtOrBefore(1500)
	if !ok || kf.TimestampMs != 1000 || !bytes.Equal(kf.Payload, idr1) {
		t.Fatalf("recovered keyframe mismatch ok=%v ts=%d", ok, kf.TimestampMs)
	}
	gotSPS, gotPPS = reopened.ConfigNALs()
	if !bytes.Equal(gotSPS, sps) || !bytes.Equal(gotPPS, pps) {
		t.Fatal("recovered SPS/PPS mismatch")
	}
	stats := reopened.Stats()
	if stats.SegmentCount == 0 {
		t.Fatal("expected recovered segment")
	}
	if stats.BytesStored == 0 {
		t.Fatal("expected recovered bytes")
	}
}

func TestVideoReplayStartPoint(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	_ = mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42), 900)
	_ = mustAppend(t, store, annexBNAL(nalTypePPS, 0x68), 900)
	kf1 := mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01), 1000)
	_ = mustAppend(t, store, annexBNAL(1, 0x11), 1100)
	kf2 := mustAppend(t, store, annexBNAL(nalTypeIDR, 0x02), 2000)
	_ = mustAppend(t, store, annexBNAL(1, 0x22), 2100)

	cases := []struct {
		t    uint64
		want uint64
	}{
		{1000, kf1},
		{1500, kf1},
		{1999, kf1},
		{2000, kf2},
		{5000, kf2},
	}
	for _, tc := range cases {
		_, seq, ok := store.FindKeyframeAtOrBefore(tc.t)
		if !ok {
			t.Fatalf("T=%d: expected keyframe", tc.t)
		}
		if seq != tc.want {
			t.Fatalf("T=%d: got seq %d, want %d", tc.t, seq, tc.want)
		}
	}

	frames, err := store.ReplayFrom(1500)
	if err != nil {
		t.Fatalf("replay: %v", err)
	}
	if len(frames) < 3 {
		t.Fatalf("expected config + frames from first IDR, got %d", len(frames))
	}
	if !frames[0].IsConfig() || !frames[1].IsConfig() {
		t.Fatal("replay must begin with SPS/PPS config frames")
	}
	var firstMedia VideoFrame
	for _, f := range frames {
		if !f.IsConfig() {
			firstMedia = f
			break
		}
	}
	if firstMedia.TimestampMs != 1000 || !firstMedia.IsKeyframe() {
		t.Fatalf("replay media start ts=%d key=%v, want 1000 keyframe", firstMedia.TimestampMs, firstMedia.IsKeyframe())
	}
}

func TestVideoStoreIndexRebuildAfterCrash(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	idr := annexBNAL(nalTypeIDR, 0x99)
	if _, err := store.Append(annexBNAL(nalTypeSPS, 0x42), 10); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Append(annexBNAL(nalTypePPS, 0x68), 10); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Append(idr, 20); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	segDir := filepath.Join(dir, "video", "segments")
	entries, err := os.ReadDir(segDir)
	if err != nil {
		t.Fatal(err)
	}
	removed := 0
	for _, e := range entries {
		if filepath.Ext(e.Name()) == ".idx" {
			if err := os.Remove(filepath.Join(segDir, e.Name())); err != nil {
				t.Fatal(err)
			}
			removed++
		}
	}
	if removed == 0 {
		t.Fatal("expected an index file to remove")
	}

	reopened, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("reopen after idx loss: %v", err)
	}
	defer reopened.Close()

	frames, err := reopened.ReplayFrom(0)
	if err != nil {
		t.Fatalf("replay after rebuild: %v", err)
	}
	foundIDR := false
	for _, f := range frames {
		if f.IsKeyframe() && bytes.Equal(f.Payload, idr) {
			foundIDR = true
		}
	}
	if !foundIDR {
		t.Fatal("rebuilt index did not recover the IDR NAL")
	}
}

func TestReadOrWaitDoesNotRewindSameKeyframe(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 1000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 1000)
	idrSeq := mustAppend(t, store, annexBNAL(nalTypeIDR, 0x65, 0x01), 1000)
	for i := 1; i <= videoLiveMaxLag+20; i++ {
		mustAppend(t, store, annexBNAL(1, byte(i)), 1000+uint64(i)*33)
	}

	done := make(chan struct {
		seq uint64
		key bool
	}, 1)
	go func() {
		frame, seq, err := store.ReadOrWait(idrSeq + 1)
		if err != nil {
			t.Errorf("ReadOrWait: %v", err)
			done <- struct {
				seq uint64
				key bool
			}{}
			return
		}
		done <- struct {
			seq uint64
			key bool
		}{seq, frame.IsKeyframe()}
	}()

	select {
	case got := <-done:
		t.Fatalf("ReadOrWait returned immediately seq=%d key=%v; it must wait at the live edge instead of rewinding to the IDR", got.seq, got.key)
	case <-time.After(80 * time.Millisecond):
	}

	liveSeq := mustAppend(t, store, annexBNAL(1, 0xee), 9000)
	select {
	case got := <-done:
		if got.key {
			t.Fatalf("ReadOrWait rewound to a keyframe seq=%d", got.seq)
		}
		if got.seq != liveSeq {
			t.Fatalf("ReadOrWait seq=%d, want live tail %d", got.seq, liveSeq)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("ReadOrWait did not unblock after a new live NAL")
	}
}

func TestReadOrWaitJumpsForwardToNewerKeyframe(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 1000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 1000)
	idr1 := mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01), 1000)
	for i := 1; i <= videoLiveMaxLag+20; i++ {
		mustAppend(t, store, annexBNAL(1, byte(i)), 1000+uint64(i)*33)
	}
	idr2 := mustAppend(t, store, annexBNAL(nalTypeIDR, 0x02), 20000)

	frame, seq, err := store.ReadOrWait(idr1 + 1)
	if err != nil {
		t.Fatalf("ReadOrWait: %v", err)
	}
	if !frame.IsKeyframe() || seq != idr2 {
		t.Fatalf("got seq=%d key=%v, want newer IDR %d", seq, frame.IsKeyframe(), idr2)
	}
}

func mustAppend(t *testing.T, store *VideoStore, nal []byte, ts uint64) uint64 {
	t.Helper()
	seq, err := store.Append(nal, ts)
	if err != nil {
		t.Fatalf("append: %v", err)
	}
	return seq
}
