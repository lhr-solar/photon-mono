package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// The index keeps one entry per keyframe, not per NAL. 50 000 NALs with a keyframe
// every 48 fill about 35 segments; the in-memory keyframe count must be the GOP
// count, and every NAL must read back from disk with the hot tail empty.
func TestVideoIndexKeepsOnlyKeyframesInMemory(t *testing.T) {
	store := newVideoStore(t)
	const n = 50_000
	const gop = 48
	base := uint64(1_700_000_000_000)
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), base)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), base)

	seqs := make([]uint64, n)
	payloads := make([][]byte, n)
	for i := 0; i < n; i++ {
		nalType := byte(1)
		if i%gop == 0 {
			nalType = nalTypeIDR
		}
		payloads[i] = annexBNAL(nalType, byte(i), byte(i>>8), byte(i>>16), 0x5a)
		seqs[i] = mustAppend(t, store, payloads[i], base+uint64(i)*1000/24)
	}

	store.mu.Lock()
	keyframes := len(store.keyframes)
	segments := len(store.order)
	store.hot = make(map[uint64][]byte)
	store.hotOrder = nil
	store.hotBytes = 0
	store.mu.Unlock()

	if want := (n + gop - 1) / gop; keyframes != want {
		t.Fatalf("%d keyframes in memory for %d NALs, want %d (one per GOP)", keyframes, n, want)
	}
	if segments < 3 {
		t.Fatalf("%d segments, want several so that the keyframe files are exercised", segments)
	}
	for i, seq := range seqs {
		frame, ok, err := store.ReadSeq(seq)
		if err != nil || !ok {
			t.Fatalf("NAL %d (seq %d): ok=%v err=%v", i, seq, ok, err)
		}
		if !bytes.Equal(frame.Payload, payloads[i]) {
			t.Fatalf("NAL %d read back %x, want %x", i, frame.Payload, payloads[i])
		}
	}
}

// A sealed segment comes back from its keyframe file. Its .idx can be damaged: the
// store still starts, and its keyframes still read.
func TestVideoSealedSegmentLoadsWithoutItsIndex(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	idr1 := annexBNAL(nalTypeIDR, 0x01, 0x02, 0x03)
	idr2 := annexBNAL(nalTypeIDR, 0x04, 0x05, 0x06)
	mustAppend(t, store, sps, 1000)
	mustAppend(t, store, pps, 1000)
	mustAppend(t, store, idr1, 1000)
	for ts := uint64(1100); ts <= 59_000; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x55), ts)
	}
	// Segment 1 runs past 60 s: this keyframe rotates it, so segment 1 is sealed.
	idr2Seq := mustAppend(t, store, idr2, 61_000)
	for ts := uint64(61_100); ts <= 61_500; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x66), ts)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	segDir := filepath.Join(dir, "video", "segments")
	if _, err := os.Stat(videoKeyframePath(segDir, 1)); err != nil {
		t.Fatalf("sealing segment 1 must write its keyframe file: %v", err)
	}
	if err := os.WriteFile(videoIndexPath(segDir, 1), bytes.Repeat([]byte{0xff}, 4096), 0o644); err != nil {
		t.Fatal(err)
	}

	reopened, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("reopen with a damaged sealed index: %v", err)
	}
	defer reopened.Close()
	frame, _, ok := reopened.FindKeyframeAtOrBefore(1500)
	if !ok || frame.TimestampMs != 1000 || !bytes.Equal(frame.Payload, idr1) {
		t.Fatalf("keyframe of the sealed segment: ok=%v ts=%d, want the IDR at 1000", ok, frame.TimestampMs)
	}
	frame, seq, ok := reopened.LatestKeyframe()
	if !ok || seq != idr2Seq || !bytes.Equal(frame.Payload, idr2) {
		t.Fatalf("latest keyframe ok=%v seq=%d, want the IDR at seq %d", ok, seq, idr2Seq)
	}
	if got, ok, err := reopened.ReadSeq(idr2Seq); err != nil || !ok || !bytes.Equal(got.Payload, idr2) {
		t.Fatalf("ReadSeq of the newest segment: ok=%v err=%v", ok, err)
	}
	gotSPS, gotPPS := reopened.ConfigNALs()
	if !bytes.Equal(gotSPS, sps) || !bytes.Equal(gotPPS, pps) {
		t.Fatal("config after reopen does not match the stored SPS/PPS")
	}
}

// A sealed segment that has no keyframe file (old data) is scanned once on open and
// gets one. The scan writes the same file the seal wrote, and leaves the .idx alone.
func TestVideoSegmentWithoutKeyframeFileIsScannedOnce(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 1000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 1000)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01, 0x02), 1000)
	for ts := uint64(1100); ts <= 59_000; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x55), ts)
	}
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x04, 0x05), 61_000)
	mustAppend(t, store, annexBNAL(1, 0x66), 61_100)
	if err := store.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	segDir := filepath.Join(dir, "video", "segments")
	kfPath := videoKeyframePath(segDir, 1)
	idxPath := videoIndexPath(segDir, 1)
	wantKF, err := os.ReadFile(kfPath)
	if err != nil {
		t.Fatal(err)
	}
	wantIdx, err := os.ReadFile(idxPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(kfPath); err != nil {
		t.Fatal(err)
	}

	scanned, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open without a keyframe file: %v", err)
	}
	gotKF, err := os.ReadFile(kfPath)
	if err != nil {
		t.Fatalf("the scan must write the keyframe file: %v", err)
	}
	if !bytes.Equal(gotKF, wantKF) {
		t.Fatal("the keyframe file written by the scan differs from the one the seal wrote")
	}
	rowsScanned := scanned.PointerRows()
	if err := scanned.Close(); err != nil {
		t.Fatal(err)
	}
	gotIdx, err := os.ReadFile(idxPath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(gotIdx, wantIdx) {
		t.Fatal("the scan changed a sealed segment's .idx")
	}

	// The next open reads the file the scan wrote, and gets the same table.
	again, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer again.Close()
	rowsAgain := again.PointerRows()
	if len(rowsAgain) != len(rowsScanned) {
		t.Fatalf("%d rows after the scan, %d on the next open", len(rowsScanned), len(rowsAgain))
	}
	for i := range rowsAgain {
		if rowsAgain[i] != rowsScanned[i] {
			t.Fatalf("row %d differs between the scan and the next open", i)
		}
	}
}
