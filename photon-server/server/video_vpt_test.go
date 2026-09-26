package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// The 40-byte row layout is fixed by tsdb-architecture.md §8.1. Asserting the
// byte offsets rather than only a round trip is what catches a field being
// silently reordered, which would make every existing table misread.
func TestGopRowByteLayout(t *testing.T) {
	row := GopRow{
		StartMs:    0x0102030405060708,
		EndMs:      0x1112131415161718,
		ByteOffset: 0x2122232425262728,
		ByteLength: 0x31323334,
		SegmentID:  0x41424344,
		NalCount:   0x51525354,
		StreamID:   0x61,
		Flags:      vptFlagConfigInline,
	}
	var buf [vptRowSize]byte
	encodeGopRow(buf[:], row)

	if got := binary.BigEndian.Uint64(buf[0:8]); got != row.StartMs {
		t.Fatalf("startMs at offset 0: got %#x", got)
	}
	if got := binary.BigEndian.Uint64(buf[8:16]); got != row.EndMs {
		t.Fatalf("endMs at offset 8: got %#x", got)
	}
	if got := binary.BigEndian.Uint64(buf[16:24]); got != row.ByteOffset {
		t.Fatalf("byteOffset at offset 16: got %#x", got)
	}
	if got := binary.BigEndian.Uint32(buf[24:28]); got != row.ByteLength {
		t.Fatalf("byteLength at offset 24: got %#x", got)
	}
	if got := binary.BigEndian.Uint32(buf[28:32]); got != row.SegmentID {
		t.Fatalf("segmentId at offset 28: got %#x", got)
	}
	if got := binary.BigEndian.Uint32(buf[32:36]); got != row.NalCount {
		t.Fatalf("nalCount at offset 32: got %#x", got)
	}
	if buf[36] != row.StreamID {
		t.Fatalf("streamId at offset 36: got %#x", buf[36])
	}
	if buf[37] != row.Flags {
		t.Fatalf("flags at offset 37: got %#x", buf[37])
	}
	if got := binary.BigEndian.Uint16(buf[38:40]); got != 0 {
		t.Fatalf("reserved at offset 38 must be zero, got %#x", got)
	}

	back := decodeGopRow(buf[:])
	if back != row {
		t.Fatalf("round trip mismatch:\n got %+v\nwant %+v", back, row)
	}
}

func TestVptFileRoundTrip(t *testing.T) {
	rows := []GopRow{
		{StartMs: 1000, EndMs: 2966, ByteOffset: 0, ByteLength: 4096, SegmentID: 1, NalCount: 60},
		{StartMs: 3000, EndMs: 4966, ByteOffset: 4096, ByteLength: 3800, SegmentID: 1, NalCount: 60},
	}
	buf := encodeVptHeader(vptHeader{
		rowCount:      uint32(len(rows)),
		minMs:         1000,
		maxMs:         4966,
		clockOffsetMs: -37,
		clockProv:     clockProvRTCP,
	})
	for _, r := range rows {
		var rowBuf [vptRowSize]byte
		encodeGopRow(rowBuf[:], r)
		buf = append(buf, rowBuf[:]...)
	}

	hdr, back, err := decodeVpt(buf)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if hdr.rowCount != 2 || hdr.minMs != 1000 || hdr.maxMs != 4966 {
		t.Fatalf("header mismatch: %+v", hdr)
	}
	if hdr.clockOffsetMs != -37 {
		t.Fatalf("clock offset %d, want -37 (a negative offset must survive the unsigned field)",
			hdr.clockOffsetMs)
	}
	if hdr.clockProv != clockProvRTCP {
		t.Fatalf("provenance %d, want %d", hdr.clockProv, clockProvRTCP)
	}
	if len(back) != 2 || back[0] != rows[0] || back[1] != rows[1] {
		t.Fatalf("rows mismatch: %+v", back)
	}

	if _, _, err := decodeVpt(buf[:10]); err == nil {
		t.Fatal("a truncated file must be rejected")
	}
	bad := append([]byte(nil), buf...)
	bad[0] ^= 0xff
	if _, _, err := decodeVpt(bad); err == nil {
		t.Fatal("a bad magic must be rejected")
	}
}

// One row per GOP, built as NALs arrive. The GOP byte span must be exactly the
// bytes between one keyframe and the next, or a range read hands the decoder a
// truncated or overlapping stream.
func TestVptBuilderOneRowPerGop(t *testing.T) {
	var b vptBuilder
	b.reset("")

	// sizes: sps 8, pps 6, idr 40, p 20 each.
	off := uint64(0)
	add := func(ts uint64, size uint32, flags uint8, nalType uint8) {
		b.add(0, 1, ts, off, size, flags, nalType)
		off += uint64(size)
	}
	add(1000, 8, VideoFlagConfig, nalTypeSPS)
	add(1000, 6, VideoFlagConfig, nalTypePPS)
	add(1000, 40, VideoFlagKeyframe, nalTypeIDR)
	add(1033, 20, 0, 1)
	add(1066, 20, 0, 1)
	// Second GOP at a 2 s IDR interval (§8.4).
	add(3000, 40, VideoFlagKeyframe, nalTypeIDR)
	add(3033, 20, 0, 1)

	rows := b.snapshot()
	if len(rows) != 2 {
		t.Fatalf("expected 2 GOP rows, got %d: %+v", len(rows), rows)
	}

	g0 := rows[0]
	// SPS+PPS precede the IDR contiguously, so they belong to the GOP span and
	// the row advertises inline config.
	if g0.ByteOffset != 0 {
		t.Fatalf("gop0 byteOffset %d, want 0 (SPS is the first byte of the span)", g0.ByteOffset)
	}
	if !g0.HasInlineConfig() {
		t.Fatal("gop0 must set the inline-config flag: its span starts with SPS/PPS")
	}
	if want := uint32(8 + 6 + 40 + 20 + 20); g0.ByteLength != want {
		t.Fatalf("gop0 byteLength %d, want %d", g0.ByteLength, want)
	}
	if g0.NalCount != 5 {
		t.Fatalf("gop0 nalCount %d, want 5", g0.NalCount)
	}
	if g0.StartMs != 1000 || g0.EndMs != 1066 {
		t.Fatalf("gop0 span [%d,%d], want [1000,1066]", g0.StartMs, g0.EndMs)
	}
	if g0.IsContinuation() {
		t.Fatal("gop0 begins at a keyframe and must not be a continuation")
	}

	g1 := rows[1]
	if g1.ByteOffset != uint64(8+6+40+20+20) {
		t.Fatalf("gop1 byteOffset %d, want %d (must abut gop0)", g1.ByteOffset, 8+6+40+20+20)
	}
	if g1.ByteLength != 60 {
		t.Fatalf("gop1 byteLength %d, want 60", g1.ByteLength)
	}
	if g1.HasInlineConfig() {
		t.Fatal("gop1 has no preceding SPS/PPS and must not claim inline config")
	}

	// Spans must tile the stream with no gap and no overlap.
	if g0.ByteOffset+uint64(g0.ByteLength) != g1.ByteOffset {
		t.Fatalf("gop spans do not tile: gop0 ends at %d, gop1 starts at %d",
			g0.ByteOffset+uint64(g0.ByteLength), g1.ByteOffset)
	}
	if uint64(g1.ByteOffset)+uint64(g1.ByteLength) != off {
		t.Fatalf("gop1 does not reach the end of the stream: %d vs %d",
			uint64(g1.ByteOffset)+uint64(g1.ByteLength), off)
	}
}

// Real encoders repeat SPS/PPS ahead of every IDR. Those config NALs get pulled
// into the following keyframe's row, so the row that was open when they arrived
// must not have stretched its EndMs over them: otherwise consecutive rows
// overlap and a timestamp on the boundary is inside two GOPs at once, which
// makes "the GOP containing this timestamp" ambiguous.
func TestVptBuilderRowsDoNotOverlapInTime(t *testing.T) {
	var b vptBuilder
	b.reset("")

	off := uint64(0)
	add := func(ts uint64, size uint32, flags uint8, nalType uint8) {
		b.add(0, 1, ts, off, size, flags, nalType)
		off += uint64(size)
	}
	// Three GOPs at a 1.5 s IDR interval, 30 fps, with config before each IDR.
	for g := 0; g < 3; g++ {
		base := uint64(100_000 + g*1500)
		add(base, 8, VideoFlagConfig, nalTypeSPS)
		add(base, 6, VideoFlagConfig, nalTypePPS)
		add(base, 40, VideoFlagKeyframe, nalTypeIDR)
		for k := 1; k < 45; k++ {
			add(base+uint64(k)*1500/45, 20, 0, 1)
		}
	}

	rows := b.snapshot()
	if len(rows) != 3 {
		t.Fatalf("expected 3 GOP rows, got %d", len(rows))
	}
	for i := 1; i < len(rows); i++ {
		prev, cur := rows[i-1], rows[i]
		if prev.EndMs >= cur.StartMs {
			t.Fatalf("row %d ends at %d but row %d starts at %d: the spans overlap, so a "+
				"timestamp at %d is inside both GOPs", i-1, prev.EndMs, i, cur.StartMs, cur.StartMs)
		}
		// Bytes still have to tile with no gap; only the timestamps changed.
		if prev.ByteOffset+uint64(prev.ByteLength) != cur.ByteOffset {
			t.Fatalf("row %d ends at byte %d but row %d starts at byte %d",
				i-1, prev.ByteOffset+uint64(prev.ByteLength), i, cur.ByteOffset)
		}
		if !cur.HasInlineConfig() {
			t.Fatalf("row %d is preceded by SPS/PPS and must advertise inline config", i)
		}
	}
	// The last non-config NAL of GOP 0 is at 100_000 + 44*1500/45 = 101466.
	if rows[0].EndMs != 101_466 {
		t.Fatalf("row 0 EndMs %d, want 101466 (its last picture, not the next GOP's SPS)",
			rows[0].EndMs)
	}
}

// A forced rotation can move the tail of a GOP into the next segment. Those
// bytes must stay addressable, but they are not a legal decode entry point.
func TestVptBuilderMarksSegmentSplitAsContinuation(t *testing.T) {
	var b vptBuilder
	b.reset("")
	b.add(0, 1, 1000, 0, 40, VideoFlagKeyframe, nalTypeIDR)
	b.add(0, 1, 1033, 40, 20, 0, 1)
	// Rotation: same GOP, new segment, offsets restart.
	b.add(0, 2, 1066, 0, 20, 0, 1)

	rows := b.snapshot()
	if len(rows) != 2 {
		t.Fatalf("expected a keyframe row plus a continuation row, got %d: %+v", len(rows), rows)
	}
	if rows[0].IsContinuation() {
		t.Fatal("row 0 starts at the IDR and must not be flagged as a continuation")
	}
	if !rows[1].IsContinuation() {
		t.Fatal("row 1 does not start at a keyframe and must be flagged as a continuation")
	}
	if rows[1].SegmentID != 2 || rows[1].ByteOffset != 0 || rows[1].ByteLength != 20 {
		t.Fatalf("continuation row wrong: %+v", rows[1])
	}
	if idx := locateEntry(rows, 0, 1066); idx != 0 {
		t.Fatalf("locate at 1066 returned row %d; a continuation row is not a decodable entry, "+
			"so the keyframe row 0 is the only valid answer", idx)
	}
}

func TestLocateEntryMapsTimestampToContainingGop(t *testing.T) {
	// GOPs at a 2 s IDR interval, each spanning [start, start+1966].
	var rows []GopRow
	for i := 0; i < 8; i++ {
		start := uint64(10_000 + i*2000)
		rows = append(rows, GopRow{
			StartMs:    start,
			EndMs:      start + 1966,
			ByteOffset: uint64(i) * 4096,
			ByteLength: 4096,
			SegmentID:  1,
			NalCount:   60,
		})
	}

	cases := []struct {
		at   uint64
		want int
	}{
		{10_000, 0}, // exactly on the first keyframe
		{10_001, 0}, // just inside
		{11_999, 0}, // last ms before the next keyframe
		{12_000, 1}, // exactly on the second keyframe
		{13_500, 1}, // mid GOP
		{23_999, 6}, // last ms of the penultimate GOP
		{24_000, 7}, // final keyframe
		{99_999, 7}, // past the end clamps to the last GOP
		{1, 0},      // before the start: earliest keyframe is the only entry
	}
	for _, tc := range cases {
		got := locateEntry(rows, 0, tc.at)
		if got != tc.want {
			t.Fatalf("locate(%d) = row %d (startMs %d), want row %d (startMs %d)",
				tc.at, got, rows[got].StartMs, tc.want, rows[tc.want].StartMs)
		}
		// The defining property: the entry is the *last* keyframe at or before
		// the timestamp. Checking both halves is what makes an off-by-one in
		// the search fail here rather than silently seek a GOP too early.
		if tc.at >= rows[0].StartMs {
			if rows[got].StartMs > tc.at {
				t.Fatalf("locate(%d) returned a GOP starting at %d, which is after it",
					tc.at, rows[got].StartMs)
			}
			if got+1 < len(rows) && rows[got+1].StartMs <= tc.at {
				t.Fatalf("locate(%d) returned row %d (startMs %d) but row %d (startMs %d) is a later "+
					"keyframe still at or before it", tc.at, got, rows[got].StartMs, got+1,
					rows[got+1].StartMs)
			}
		}
	}

	if idx := locateEntry(rows, 3, 12_000); idx != -1 {
		t.Fatalf("locate on an absent streamId returned %d, want -1", idx)
	}
	if idx := locateEntry(nil, 0, 12_000); idx != -1 {
		t.Fatalf("locate on an empty table returned %d, want -1", idx)
	}
}

func TestCoverageCollapsesToSegments(t *testing.T) {
	rows := []GopRow{
		{StartMs: 1000, EndMs: 2900, SegmentID: 1},
		{StartMs: 3000, EndMs: 4900, SegmentID: 1},
		{StartMs: 5000, EndMs: 6900, SegmentID: 2},
	}
	cov := coverageOf(rows, 0, 0, 10_000)
	if len(cov) != 2 {
		t.Fatalf("expected one coverage row per segment, got %d: %+v", len(cov), cov)
	}
	if cov[0].SegmentID != 1 || cov[0].StartMs != 1000 || cov[0].EndMs != 4900 {
		t.Fatalf("segment 1 coverage %+v, want [1000,4900]", cov[0])
	}
	if cov[1].SegmentID != 2 || cov[1].StartMs != 5000 || cov[1].EndMs != 6900 {
		t.Fatalf("segment 2 coverage %+v", cov[1])
	}
	if got := coverageOf(rows, 0, 20_000, 30_000); len(got) != 0 {
		t.Fatalf("a window past the data must be uncovered, got %+v", got)
	}
}

// The pointer table is live state: it must be on disk while recording, not
// written by a later pass.
func TestVptWrittenLiveAndRebuildable(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}

	appendGop := func(startMs uint64, withConfig bool) {
		if withConfig {
			mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), startMs)
			mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), startMs)
		}
		mustAppend(t, store, annexBNAL(nalTypeIDR, bytes.Repeat([]byte{0xaa}, 32)...), startMs)
		for k := 1; k < 60; k++ {
			mustAppend(t, store, annexBNAL(1, bytes.Repeat([]byte{0xbb}, 12)...), startMs+uint64(k)*33)
		}
	}
	appendGop(100_000, true)
	appendGop(102_000, false)
	appendGop(104_000, false)

	vptPath := filepath.Join(dir, "video", "video.vpt")
	data, err := os.ReadFile(vptPath)
	if err != nil {
		t.Fatalf("video.vpt must exist while recording, not after a post-pass: %v", err)
	}
	_, persisted, err := decodeVpt(data)
	if err != nil {
		t.Fatalf("decode live video.vpt: %v", err)
	}
	// The third GOP is still open (no following keyframe), so two closed rows
	// are the most that can be durable.
	if len(persisted) != 2 {
		t.Fatalf("expected 2 closed rows persisted mid-recording, got %d", len(persisted))
	}
	if persisted[0].StartMs != 100_000 || persisted[1].StartMs != 102_000 {
		t.Fatalf("persisted rows out of order: %+v", persisted)
	}

	live := store.PointerRows()
	if len(live) != 3 {
		t.Fatalf("in-memory table must include the open GOP: got %d rows", len(live))
	}

	if err := store.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	// After close the open row is durable too.
	data, err = os.ReadFile(vptPath)
	if err != nil {
		t.Fatal(err)
	}
	_, afterClose, err := decodeVpt(data)
	if err != nil {
		t.Fatal(err)
	}
	if len(afterClose) != 3 {
		t.Fatalf("the final GOP must be persisted at close, got %d rows", len(afterClose))
	}

	// Deleting the table must lose nothing: it is derived from the .idx records.
	if err := os.Remove(vptPath); err != nil {
		t.Fatal(err)
	}
	reopened, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer reopened.Close()
	rebuilt := reopened.PointerRows()
	if len(rebuilt) != len(afterClose) {
		t.Fatalf("rebuilt %d rows, want %d", len(rebuilt), len(afterClose))
	}
	for i := range rebuilt {
		if rebuilt[i] != afterClose[i] {
			t.Fatalf("rebuilt row %d differs:\n got %+v\nwant %+v", i, rebuilt[i], afterClose[i])
		}
	}
}

// The point of the pointer table: a timestamp resolves to a byte span that a
// decoder can actually start on.
func TestStoreLocateResolvesToDecodableByteRange(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	idrPayloads := map[uint64][]byte{}
	for g := 0; g < 4; g++ {
		startMs := uint64(200_000 + g*2000)
		if g == 0 {
			mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), startMs)
			mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), startMs)
		}
		idr := annexBNAL(nalTypeIDR, byte(g), 0x5a, 0x5a)
		idrPayloads[startMs] = idr
		mustAppend(t, store, idr, startMs)
		for k := 1; k < 10; k++ {
			mustAppend(t, store, annexBNAL(1, byte(g), byte(k)), startMs+uint64(k)*33)
		}
	}

	// A cursor in the middle of GOP 2 must land on GOP 2's keyframe.
	res := store.Locate(0, 204_500, 2)
	if !res.Found {
		t.Fatal("locate must find a GOP for a timestamp inside the recording")
	}
	if res.Entry.StartMs != 204_000 {
		t.Fatalf("entry startMs %d, want 204000 (the keyframe at or before 204500)", res.Entry.StartMs)
	}
	if len(res.Prefetch) != 1 {
		t.Fatalf("asked for 2 prefetch GOPs with only 1 remaining, got %d", len(res.Prefetch))
	}
	if res.Prefetch[0].StartMs != 206_000 {
		t.Fatalf("prefetch startMs %d, want 206000", res.Prefetch[0].StartMs)
	}
	if len(res.SPS) == 0 || len(res.PPS) == 0 {
		t.Fatal("locate must carry the SPS/PPS needed to initialise the decoder")
	}

	// The byte range must begin with the entry keyframe, so feeding it to a
	// decoder starts at an IDR.
	span, err := store.ReadRange(res.Entry.SegmentID, res.Entry.ByteOffset, res.Entry.ByteLength)
	if err != nil {
		t.Fatalf("range read: %v", err)
	}
	if len(span) != int(res.Entry.ByteLength) {
		t.Fatalf("range read returned %d bytes, want %d", len(span), res.Entry.ByteLength)
	}
	wantIDR := idrPayloads[204_000]
	if !bytes.HasPrefix(span, wantIDR) {
		t.Fatalf("GOP byte span does not start with its IDR:\n got %x\nwant prefix %x",
			span[:min(len(span), 16)], wantIDR)
	}
	// And it must not run into the next keyframe.
	if bytes.Contains(span, idrPayloads[206_000]) {
		t.Fatal("GOP byte span overruns into the next keyframe")
	}
	// Splitting the stream back into NALs must give exactly nalCount of them.
	if got := len(splitAnnexB(span)); got != int(res.Entry.NalCount) {
		t.Fatalf("span holds %d NALs, but the row claims %d", got, res.Entry.NalCount)
	}

	// The first GOP's span carries SPS/PPS inline.
	first := store.Locate(0, 200_000, 0)
	if !first.Entry.HasInlineConfig() {
		t.Fatal("the first GOP follows SPS/PPS and must advertise inline config")
	}
	firstSpan, err := store.ReadRange(first.Entry.SegmentID, first.Entry.ByteOffset, first.Entry.ByteLength)
	if err != nil {
		t.Fatal(err)
	}
	if nalTypeOfAnnexB(firstSpan) != nalTypeSPS {
		t.Fatalf("inline-config span starts with NAL type %d, want SPS (%d)",
			nalTypeOfAnnexB(firstSpan), nalTypeSPS)
	}

	if _, err := store.ReadRange(999, 0, 16); err == nil {
		t.Fatal("a range read on a missing segment must fail")
	}
	if _, err := store.ReadRange(res.Entry.SegmentID, 1<<40, 16); err == nil {
		t.Fatal("a range read past the end of a segment must fail")
	}
	if _, err := store.ReadRange(res.Entry.SegmentID, 0, videoRangeMaxBytes+1); err == nil {
		t.Fatal("an oversized range read must be refused rather than allocated")
	}
}

func TestLocateOnEmptyStore(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()
	res := store.Locate(0, 1234, 4)
	if res.Found {
		t.Fatal("locate on a store with no video must not report a GOP")
	}
	if res.TotalRows != 0 {
		t.Fatalf("totalRows %d, want 0", res.TotalRows)
	}
}

func TestLastKeyframeSkipsFutureGOP(t *testing.T) {
	now := videoNowMS()
	rows := []GopRow{
		{StartMs: now - 4000, EndMs: now - 3000, SegmentID: 1, Flags: vptFlagConfigInline},
		{StartMs: now - 500, EndMs: now - 100, SegmentID: 2, Flags: vptFlagConfigInline},
		{StartMs: now + 5_000_000, EndMs: now + 5_000_400, SegmentID: 3, Flags: vptFlagConfigInline},
	}
	got := lastKeyframe(rows, 0)
	if got != 1 {
		t.Fatalf("lastKeyframe=%d, want the current GOP not the hours-ahead RTCP row", got)
	}
	skewed := []GopRow{
		{StartMs: now - 4000, EndMs: now - 3000, SegmentID: 1, Flags: vptFlagConfigInline},
		{StartMs: now + 45_000, EndMs: now + 45_500, SegmentID: 2, Flags: vptFlagConfigInline},
	}
	got = lastKeyframe(skewed, 0)
	if got != 1 {
		t.Fatalf("lastKeyframe=%d, want the ~45s-ahead live GOP, not the old still", got)
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
