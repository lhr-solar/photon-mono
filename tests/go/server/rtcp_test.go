package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// buildSenderReport assembles a minimal RTCP SR (RFC 3550 §6.4.1).
func buildSenderReport(ntpMsw, ntpLsw, rtpTs uint32) []byte {
	pkt := make([]byte, 28)
	pkt[0] = rtpVersion << 6
	pkt[1] = rtcpPayloadTypeSR
	binary.BigEndian.PutUint16(pkt[2:4], uint16(len(pkt)/4-1))
	binary.BigEndian.PutUint32(pkt[4:8], 0xDEADBEEF) // SSRC
	binary.BigEndian.PutUint32(pkt[8:12], ntpMsw)
	binary.BigEndian.PutUint32(pkt[12:16], ntpLsw)
	binary.BigEndian.PutUint32(pkt[16:20], rtpTs)
	return pkt
}

// unixMsToNTP is the inverse of ntpToUnixMs, used to build test inputs.
func unixMsToNTP(ms uint64) (uint32, uint32) {
	secs := ms / 1000
	frac := ms % 1000
	return uint32(secs + ntpUnixEpochDelta), uint32((frac << 32) / 1000)
}

func TestNTPToUnixMs(t *testing.T) {
	// 1784843212345 ms is inside the corpus's July 2026 range.
	const want = uint64(1784843212345)
	msw, lsw := unixMsToNTP(want)
	got := ntpToUnixMs(msw, lsw)
	// The 32-bit fraction quantises to about 0.23 ns, so a whole millisecond
	// must survive exactly.
	if got != want {
		t.Fatalf("ntpToUnixMs = %d, want %d", got, want)
	}
	if ntpToUnixMs(0, 0) != 0 {
		t.Fatal("a pre-unix-epoch NTP timestamp must be rejected as 0, not wrap")
	}
}

func TestParseRTCPSenderReports(t *testing.T) {
	const wallMs = uint64(1784843212000)
	msw, lsw := unixMsToNTP(wallMs)
	pkt := buildSenderReport(msw, lsw, 900000)

	reports := parseRTCPSenderReports(pkt)
	if len(reports) != 1 {
		t.Fatalf("expected 1 sender report, got %d", len(reports))
	}
	if reports[0].NtpMs != wallMs {
		t.Fatalf("ntpMs %d, want %d", reports[0].NtpMs, wallMs)
	}
	if reports[0].RtpTs != 900000 {
		t.Fatalf("rtpTs %d, want 900000", reports[0].RtpTs)
	}

	// A compound packet: SR followed by a second SR.
	msw2, lsw2 := unixMsToNTP(wallMs + 5000)
	compound := append(append([]byte(nil), pkt...), buildSenderReport(msw2, lsw2, 900000+5*90000)...)
	if got := parseRTCPSenderReports(compound); len(got) != 2 {
		t.Fatalf("compound packet yielded %d reports, want 2", len(got))
	}

	// A receiver report (PT 201) carries no timing pair.
	rr := append([]byte(nil), pkt...)
	rr[1] = 201
	if got := parseRTCPSenderReports(rr); len(got) != 0 {
		t.Fatalf("a receiver report must yield no sender timing, got %+v", got)
	}

	// Garbage must not be mistaken for a report.
	if got := parseRTCPSenderReports([]byte{0x00, 0x01, 0x02}); len(got) != 0 {
		t.Fatalf("short garbage yielded %+v", got)
	}
	bad := append([]byte(nil), pkt...)
	bad[0] = 0x00 // version 0
	if got := parseRTCPSenderReports(bad); len(got) != 0 {
		t.Fatal("a bad RTP version must be rejected")
	}
}

// The whole point of §8.5: video timestamps must come from the sender's clock,
// not the receiver's, so they are comparable to telemetry timestamps.
func TestRTPClockMapsSenderTimeNotReceiveTime(t *testing.T) {
	clock := NewRTPClock()

	// Before any sender report there is nothing to map with, and the fallback
	// must be labelled rather than passed off as aligned.
	ms, prov := clock.WallMs(900000, 5_000_000)
	if ms != 5_000_000 || prov != clockProvReceive {
		t.Fatalf("pre-SR fallback gave ms=%d prov=%d, want receive time 5000000", ms, prov)
	}
	if clock.Provenance() != clockProvReceive {
		t.Fatal("provenance must be 'receive' before any sender report")
	}

	// SR: RTP 900000 was wall clock 1784843212000.
	const srWall = uint64(1784843212000)
	const srRtp = uint32(900000)
	clock.ApplySR(srWall, srRtp, srWall+40)

	// 90 kHz: one second later is +90000 ticks.
	ms, prov = clock.WallMs(srRtp+90000, srWall+1200)
	if prov != clockProvRTCP {
		t.Fatalf("provenance %d, want rtcp (%d)", prov, clockProvRTCP)
	}
	if ms != srWall+1000 {
		t.Fatalf("mapped ms %d, want %d (sender time), not %d (receive time)",
			ms, srWall+1000, srWall+1200)
	}
	// A 200 ms receive delay must not appear in the mapped timestamp.
	if ms == srWall+1200 {
		t.Fatal("mapped timestamp equals receive time; the RTP mapping is not being applied")
	}

	// Exactly at the sender report.
	if ms, _ := clock.WallMs(srRtp, srWall+40); ms != srWall {
		t.Fatalf("at the SR instant mapped ms %d, want %d", ms, srWall)
	}

	// A stale report must stop being trusted rather than extrapolate forever.
	// The report was seen at srWall+40, so staleness is measured from there.
	staleRecv := srWall + 40 + rtcpStaleMs + 1
	ms, prov = clock.WallMs(srRtp+90000, staleRecv)
	if prov != clockProvReceive {
		t.Fatalf("a sender report older than %d ms must fall back to receive time, got prov %d",
			rtcpStaleMs, prov)
	}
	if ms != staleRecv {
		t.Fatalf("stale fallback gave %d, want the receive time %d", ms, staleRecv)
	}
	// One millisecond inside the window is still trusted, so the boundary is a
	// real boundary rather than an always-stale or never-stale check.
	if _, p := clock.WallMs(srRtp+90000, srWall+40+rtcpStaleMs); p != clockProvRTCP {
		t.Fatalf("a report exactly at the staleness limit must still map, got prov %d", p)
	}
}

// A NAL can legitimately carry an RTP timestamp earlier than the last sender
// report: sender reports are periodic and frames are not aligned to them.
// Unsigned subtraction turns a small negative delta into ~13 h at 90 kHz.
func TestRTPClockMapsTimestampsBeforeTheSenderReport(t *testing.T) {
	clock := NewRTPClock()
	const srWall = uint64(1784843212000)
	const srRtp = uint32(900000)
	clock.ApplySR(srWall, srRtp, srWall)

	ms, prov := clock.WallMs(srRtp-90000, srWall-960)
	if prov != clockProvRTCP {
		t.Fatalf("a pre-SR RTP timestamp must still map, got prov %d", prov)
	}
	if ms != srWall-1000 {
		t.Fatalf("backwards mapping gave %d, want %d; a wrapped unsigned delta would give about %d",
			ms, srWall-1000, srWall+(1<<32)/90)
	}
}

// RTP timestamps wrap every ~13 h at 90 kHz; the mapping must survive it. The
// delta has to be taken in 32-bit width, because a 64-bit subtraction of the
// widened values reads the wrap as a 13 h jump backwards.
func TestRTPClockSurvivesTimestampWraparound(t *testing.T) {
	clock := NewRTPClock()
	const srWall = uint64(1784843212000)
	// 256 ticks below the wrap. A var, not a const: the deliberate overflow
	// below is a compile error on a constant.
	srRtp := uint32(0xFFFFFF00)
	clock.ApplySR(srWall, srRtp, srWall)

	wrapped := srRtp + 90000
	if wrapped >= srRtp {
		t.Fatalf("test input does not actually wrap: %d -> %d", srRtp, wrapped)
	}
	got, prov := clock.WallMs(wrapped, srWall+1000)
	if prov != clockProvRTCP {
		t.Fatalf("prov %d, want rtcp", prov)
	}
	if got != srWall+1000 {
		t.Fatalf("across the 32-bit wrap mapped ms %d, want %d; a 64-bit delta would give about %d",
			got, srWall+1000, srWall-(1<<32)/90)
	}
}

func TestRTPClockManualOffsetPersists(t *testing.T) {
	dir := t.TempDir()
	clock := NewRTPClock()
	clock.Load(dir)
	if clock.ManualOffset() != 0 {
		t.Fatalf("a fresh clock must have no manual offset, got %d", clock.ManualOffset())
	}
	if err := clock.SetManualOffset(-137); err != nil {
		t.Fatalf("set: %v", err)
	}
	if clock.ManualOffset() != -137 {
		t.Fatalf("manual offset %d, want -137", clock.ManualOffset())
	}
	// Provenance must report 'manual' when a nudge is the only alignment we
	// have, so a caller can tell it apart from a real RTCP mapping.
	if clock.Provenance() != clockProvManual {
		t.Fatalf("provenance %d, want manual (%d)", clock.Provenance(), clockProvManual)
	}

	path := filepath.Join(dir, "video", "video_clock.json")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("manual offset must persist to a manifest: %v", err)
	}
	var f videoClockFile
	if err := json.Unmarshal(data, &f); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if f.OffsetMs != -137 {
		t.Fatalf("persisted offset %d, want -137", f.OffsetMs)
	}
	if f.Provenance != "manual" {
		t.Fatalf("persisted provenance %q, want \"manual\"", f.Provenance)
	}

	reloaded := NewRTPClock()
	reloaded.Load(dir)
	if reloaded.ManualOffset() != -137 {
		t.Fatalf("reloaded offset %d, want -137", reloaded.ManualOffset())
	}

	// An RTCP mapping outranks a manual nudge.
	reloaded.ApplySR(1784843212000, 900000, videoNowMS())
	if reloaded.Provenance() != clockProvRTCP {
		t.Fatalf("with a live sender report provenance must be rtcp, got %d", reloaded.Provenance())
	}
}

func TestRTPClockManualOffsetAppliesToWallMs(t *testing.T) {
	clock := NewRTPClock()
	if err := clock.SetManualOffset(-50); err != nil {
		t.Fatal(err)
	}
	ms, prov := clock.WallMs(1, 5_000_000)
	if ms != 4_999_950 {
		t.Fatalf("receive+manual mapped %d, want 4999950", ms)
	}
	if prov != clockProvReceive {
		t.Fatalf("provenance %d, want receive", prov)
	}
	clock.ApplySR(1_000_000, 90000, 1_000_040)
	ms, prov = clock.WallMs(180000, 1_000_040)
	if ms != 1_000_950 {
		t.Fatalf("rtcp+manual mapped %d, want 1000950 (1s after SR minus 50 ms nudge)", ms)
	}
	if prov != clockProvRTCP {
		t.Fatalf("provenance %d, want rtcp — the nudge sits on top of the mapping", prov)
	}
}

// The .idx record has to carry the RTP timestamp and the raw receive time
// alongside the derived wall clock, or a recording's skew is unknowable after
// the fact (§8.5 item 1).
func TestNALRecordV2CarriesRTPAndReceiveTime(t *testing.T) {
	rec := videoNALRecord{
		TimestampMs:  1784843212345,
		Offset:       0x1122334455667788,
		Size:         4096,
		Flags:        VideoFlagKeyframe,
		NALType:      nalTypeIDR,
		ClockProv:    clockProvRTCP,
		RTPTimestamp: 0xAABBCCDD,
		ReceiveMs:    1784843212389,
	}
	var buf [videoNALRecordSizeV2]byte
	encodeNALRecord(buf[:], rec)
	back := decodeNALRecord(buf[:], videoNALRecordSizeV2)
	if back != rec {
		t.Fatalf("v2 round trip mismatch:\n got %+v\nwant %+v", back, rec)
	}

	// v1 records are 24 bytes and only ever held receive time; reading one must
	// say so rather than leaving RTP fields as plausible-looking zeros.
	v1 := buf[:videoNALRecordSizeV1]
	old := decodeNALRecord(v1, videoNALRecordSizeV1)
	if old.TimestampMs != rec.TimestampMs || old.Offset != rec.Offset || old.Size != rec.Size {
		t.Fatalf("v1 prefix must still decode: %+v", old)
	}
	if old.ClockProv != clockProvReceive {
		t.Fatalf("a v1 record must report receive provenance, got %d", old.ClockProv)
	}
	if old.ReceiveMs != rec.TimestampMs {
		t.Fatalf("a v1 record's receive time is its only timestamp, got %d", old.ReceiveMs)
	}
	if old.RTPTimestamp != 0 {
		t.Fatalf("a v1 record cannot carry an RTP timestamp, got %d", old.RTPTimestamp)
	}
}

// Sender reports arrive seconds after the stream starts, so the head of every
// recording is receive-stamped. The RTP timestamp stored alongside it is what
// makes that fixable instead of permanent.
func TestClockBackfillRestampsReceiveStampedHead(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	clock := NewRTPClock()
	store.SetClock(clock)

	const srWall = uint64(1784843212000)
	const srRtp = uint32(900000)
	// Receive time runs 250 ms behind sender time in this scenario.
	const delay = uint64(250)

	// Three NALs arrive before any sender report.
	for i := 0; i < 3; i++ {
		rtp := srRtp + uint32(i)*3000 // 33.3 ms steps at 90 kHz
		recv := srWall + uint64(i)*33 + delay
		flags := uint8(0)
		nalType := uint8(1)
		if i == 0 {
			flags = VideoFlagKeyframe
			nalType = nalTypeIDR
		}
		if _, err := store.AppendStamped(annexBNAL(nalType, byte(i)), recv, rtp, recv, clockProvReceive); err != nil {
			t.Fatalf("append %d: %v", i, err)
		}
		_ = flags
	}

	rowsBefore := store.PointerRows()
	if len(rowsBefore) != 1 {
		t.Fatalf("expected 1 GOP row, got %d", len(rowsBefore))
	}
	if rowsBefore[0].StartMs != srWall+delay {
		t.Fatalf("pre-backfill row startMs %d, want the receive-stamped %d",
			rowsBefore[0].StartMs, srWall+delay)
	}

	// Now the sender report lands and the fourth NAL is RTCP-mapped, which
	// triggers the back-fill.
	clock.ApplySR(srWall, srRtp, srWall+delay)
	rtp4 := srRtp + 3*3000
	wall4, prov4 := clock.WallMs(rtp4, srWall+99+delay)
	if prov4 != clockProvRTCP {
		t.Fatalf("the fourth NAL should map via RTCP, got prov %d", prov4)
	}
	if _, err := store.AppendStamped(annexBNAL(1, 0x04), wall4, rtp4, srWall+99+delay, prov4); err != nil {
		t.Fatalf("append 4: %v", err)
	}

	rows := store.PointerRows()
	if len(rows) != 1 {
		t.Fatalf("expected 1 GOP row after backfill, got %d", len(rows))
	}
	if rows[0].StartMs != srWall {
		t.Fatalf("post-backfill row startMs %d, want the sender-clock %d; the receive-stamped head "+
			"was not re-derived from its RTP timestamps", rows[0].StartMs, srWall)
	}
	if rows[0].EndMs != srWall+100 {
		t.Fatalf("post-backfill row endMs %d, want %d", rows[0].EndMs, srWall+100)
	}

	// The correction must be durable, not just in memory.
	if err := store.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	reopened, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer reopened.Close()
	after := reopened.PointerRows()
	if len(after) != 1 || after[0].StartMs != srWall {
		t.Fatalf("the backfill did not survive a restart: %+v", after)
	}

	res := reopened.Locate(0, srWall+50, 0)
	if !res.Found {
		t.Fatal("locate must find the backfilled GOP")
	}
	if res.ClockProv != clockProvRTCP {
		t.Fatalf("locate reports provenance %d for a backfilled GOP, want rtcp (%d)",
			res.ClockProv, clockProvRTCP)
	}
	// The residual is sender time minus receive time: the delay the mapping
	// removed. §8.5 item 4 asks for it to be recorded rather than assumed.
	if res.ResidualMs != -int64(delay) {
		t.Fatalf("residual %d ms, want %d (mapped minus receive)", res.ResidualMs, -int64(delay))
	}
}

func TestReplayStopsAtEndMs(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42), 1000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0x68), 1000)
	for i := 0; i < 10; i++ {
		ts := uint64(1000 + i*100)
		if i%5 == 0 {
			mustAppend(t, store, annexBNAL(nalTypeIDR, byte(i)), ts)
		} else {
			mustAppend(t, store, annexBNAL(1, byte(i)), ts)
		}
	}

	// Drive the real server handler over a pipe rather than a second copy of
	// its loop, so this exercises writeVideoReplay itself.
	all := serveAndDrain(t, store, VideoRequestExt{Mode: VideoModeReplay, StartTimeMs: 1000})
	bounded := serveAndDrain(t, store, VideoRequestExt{
		Mode: VideoModeReplayRange, StartTimeMs: 1000, EndTimeMs: 1300,
	})

	if len(bounded) >= len(all) {
		t.Fatalf("a bounded replay returned %d media frames and an unbounded one %d; endTimeMs is "+
			"not stopping the stream", len(bounded), len(all))
	}
	if len(bounded) == 0 {
		t.Fatal("a bounded replay returned nothing")
	}
	for _, ts := range bounded {
		if ts > 1300 {
			t.Fatalf("bounded replay emitted a frame at %d, past endTimeMs 1300", ts)
		}
	}
	if bounded[len(bounded)-1] != 1300 {
		t.Fatalf("bounded replay ended at %d, want 1300 (the last frame at or before B)",
			bounded[len(bounded)-1])
	}
}

// serveAndDrain runs serveVideoClient against a pipe and returns the media
// frame timestamps it wrote.
func serveAndDrain(t *testing.T, store *VideoStore, req VideoRequestExt) []uint64 {
	t.Helper()
	client, server := net.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go serveVideoClient(ctx, server, store)

	if err := EncodeVideoRequestExt(client, req); err != nil {
		t.Fatalf("write request: %v", err)
	}
	var out []uint64
	for {
		_ = client.SetReadDeadline(time.Now().Add(2 * time.Second))
		frame, err := DecodeVideoFrame(client)
		if err != nil {
			_ = client.Close()
			return out
		}
		if frame.Flags&VideoFlagConfig == 0 {
			out = append(out, frame.TimestampMs)
		}
	}
}

// The locate and range modes have to work over the wire, not just as store
// methods: a handler that never dispatches them delivers nothing.
func TestServeVideoClientLocateAndRange(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer store.Close()

	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 300_000)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 300_000)
	for g := 0; g < 3; g++ {
		startMs := uint64(300_000 + g*2000)
		mustAppend(t, store, annexBNAL(nalTypeIDR, byte(g), 0x77), startMs)
		for k := 1; k < 8; k++ {
			mustAppend(t, store, annexBNAL(1, byte(g), byte(k)), startMs+uint64(k)*33)
		}
	}

	// video_locate over the wire.
	client, server := net.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go serveVideoClient(ctx, server, store)
	if err := EncodeVideoRequestExt(client, VideoRequestExt{
		Mode: VideoModeLocate, StartTimeMs: 302_100, StreamID: 0, Prefetch: 2,
	}); err != nil {
		t.Fatalf("write locate: %v", err)
	}
	_ = client.SetReadDeadline(time.Now().Add(2 * time.Second))
	var lenBuf [4]byte
	if _, err := io.ReadFull(client, lenBuf[:]); err != nil {
		t.Fatalf("read locate length: %v", err)
	}
	payload := make([]byte, binary.BigEndian.Uint32(lenBuf[:]))
	if _, err := io.ReadFull(client, payload); err != nil {
		t.Fatalf("read locate payload: %v", err)
	}
	_ = client.Close()

	// Layout is documented on EncodeVideoLocateReply. Parsed by offset here so
	// a change to the wire format fails this test rather than only the C++ side.
	if payload[0] != videoLocateStatusOK {
		t.Fatalf("locate status %d, want OK", payload[0])
	}
	off := 1
	clockOffset := int64(binary.BigEndian.Uint64(payload[off : off+8]))
	off += 8
	prov := payload[off]
	off++
	residual := int64(binary.BigEndian.Uint64(payload[off : off+8]))
	off += 8
	totalRows := binary.BigEndian.Uint32(payload[off : off+4])
	off += 4
	off += 16 // minMs, maxMs
	spsLen := int(binary.BigEndian.Uint16(payload[off : off+2]))
	off += 2
	sps := payload[off : off+spsLen]
	off += spsLen
	ppsLen := int(binary.BigEndian.Uint16(payload[off : off+2]))
	off += 2
	off += ppsLen
	rowCount := int(binary.BigEndian.Uint16(payload[off : off+2]))
	off += 2
	rows := make([]GopRow, rowCount)
	for i := 0; i < rowCount; i++ {
		rows[i] = decodeGopRow(payload[off : off+vptRowSize])
		off += vptRowSize
	}
	covCount := int(binary.BigEndian.Uint16(payload[off : off+2]))
	off += 2
	off += covCount * 20
	if off != len(payload) {
		t.Fatalf("locate reply has %d trailing bytes; the layout does not match", len(payload)-off)
	}

	if clockOffset != 0 || prov != clockProvReceive {
		t.Fatalf("with no clock attached, offset must be 0 and provenance receive; got %d / %d",
			clockOffset, prov)
	}
	if residual != 0 {
		t.Fatalf("residual %d, want 0 when the wall clock is the receive time", residual)
	}
	if totalRows != 3 {
		t.Fatalf("totalRows %d, want 3", totalRows)
	}
	if rowCount != 2 {
		t.Fatalf("rowCount %d, want 2 (entry + 1 remaining prefetch GOP)", rowCount)
	}
	if rows[0].StartMs != 302_000 {
		t.Fatalf("entry startMs %d, want 302000", rows[0].StartMs)
	}
	if rows[1].StartMs != 304_000 {
		t.Fatalf("prefetch startMs %d, want 304000", rows[1].StartMs)
	}
	if covCount != 1 {
		t.Fatalf("coverage rows %d, want 1 (single segment)", covCount)
	}
	if nalTypeOfAnnexB(sps) != nalTypeSPS {
		t.Fatalf("locate reply SPS is NAL type %d", nalTypeOfAnnexB(sps))
	}

	// Byte-range read over the wire, using the pointer the locate just gave us.
	rangeClient, rangeServer := net.Pipe()
	go serveVideoClient(ctx, rangeServer, store)
	if err := EncodeVideoRequestExt(rangeClient, VideoRequestExt{
		Mode:      VideoModeRange,
		SegmentID: rows[0].SegmentID,
		Offset:    rows[0].ByteOffset,
		Length:    rows[0].ByteLength,
	}); err != nil {
		t.Fatalf("write range: %v", err)
	}
	_ = rangeClient.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := io.ReadFull(rangeClient, lenBuf[:]); err != nil {
		t.Fatalf("read range length: %v", err)
	}
	span := make([]byte, binary.BigEndian.Uint32(lenBuf[:]))
	if _, err := io.ReadFull(rangeClient, span); err != nil {
		t.Fatalf("read range payload: %v", err)
	}
	_ = rangeClient.Close()

	if len(span) != int(rows[0].ByteLength) {
		t.Fatalf("range read returned %d bytes, want %d", len(span), rows[0].ByteLength)
	}
	if nalTypeOfAnnexB(span) != nalTypeIDR {
		t.Fatalf("the entry GOP's range starts with NAL type %d, want IDR (%d): a decoder fed this "+
			"could not start", nalTypeOfAnnexB(span), nalTypeIDR)
	}
	if got := len(splitAnnexB(span)); got != int(rows[0].NalCount) {
		t.Fatalf("range holds %d NALs but the row claims %d", got, rows[0].NalCount)
	}
	// GOP 1's IDR carries a distinguishing first byte; it must not be in here.
	if bytes.Contains(span, annexBNAL(nalTypeIDR, 2, 0x77)) {
		t.Fatal("the range overruns into a later GOP")
	}
}

func TestVideoRequestExtKeepsNineByteModesIntact(t *testing.T) {
	// Old clients write exactly 9 bytes for live and replay. Growing the header
	// must not change that (§8.2).
	for _, mode := range []uint8{VideoModeLive, VideoModeReplay} {
		var buf []byte
		buf = append(buf, mode)
		var ts [8]byte
		binary.BigEndian.PutUint64(ts[:], 1784843212345)
		buf = append(buf, ts[:]...)
		if len(buf) != videoRequestBytes {
			t.Fatalf("mode %d header is %d bytes, want %d", mode, len(buf), videoRequestBytes)
		}
		// Trailing sentinel bytes stand in for whatever comes next on the
		// socket. Consuming any of them means an old 9-byte client's stream
		// would be misread, which is the compatibility this asserts.
		const sentinel = 4
		r := bytes.NewReader(append(append([]byte(nil), buf...), bytes.Repeat([]byte{0x5a}, sentinel)...))
		req, err := DecodeVideoRequestExt(r)
		if err != nil {
			t.Fatalf("mode %d decode: %v", mode, err)
		}
		if req.Mode != mode || req.StartTimeMs != 1784843212345 {
			t.Fatalf("mode %d decoded %+v", mode, req)
		}
		if n := r.Len(); n != sentinel {
			t.Fatalf("mode %d consumed %d bytes, want exactly %d: an old 9-byte client's header must "+
				"not grow", mode, videoRequestBytes+sentinel-n, videoRequestBytes)
		}
	}

	cases := []VideoRequestExt{
		{Mode: VideoModeReplayRange, StartTimeMs: 1000, EndTimeMs: 5000},
		{Mode: VideoModeLocate, StartTimeMs: 1784843212345, StreamID: 2, Prefetch: 4},
		{Mode: VideoModeRange, SegmentID: 1412, Offset: 8419328, Length: 41216},
	}
	for _, want := range cases {
		var buf bytes.Buffer
		if err := EncodeVideoRequestExt(&buf, want); err != nil {
			t.Fatalf("encode mode %d: %v", want.Mode, err)
		}
		got, err := DecodeVideoRequestExt(&buf)
		if err != nil {
			t.Fatalf("decode mode %d: %v", want.Mode, err)
		}
		if got != want {
			t.Fatalf("mode %d round trip:\n got %+v\nwant %+v", want.Mode, got, want)
		}
	}
}
