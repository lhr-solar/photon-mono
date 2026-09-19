package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
)

// video_locate and byte-range reads (tsdb-architecture.md §8.2).
//
// Returning byte ranges instead of frames is what makes seeking seamless: the
// client gets the entry GOP for a timestamp plus the pointers that follow, and
// walks forward through the byte stream instead of tearing down the connection
// and re-subscribing.

// VideoLocateResult is one answer to "where does this timestamp live".
type VideoLocateResult struct {
	Found bool

	// Entry is the GOP containing AtMs: the last keyframe at or before it.
	// A decoder starts here and discards output before AtMs.
	Entry GopRow
	// Prefetch are the GOPs immediately after Entry, so the client can request
	// ahead instead of stalling at each GOP boundary.
	Prefetch []GopRow
	Coverage []SegmentCoverage

	SPS []byte
	PPS []byte

	// ClockOffsetMs is the manual nudge a client must add to stored video
	// timestamps before comparing them to telemetry timestamps.
	ClockOffsetMs int64
	// ClockProv is how the entry GOP's own timestamps were derived. Anything
	// below clockProvRTCP means A/V sync is unverified (§8.5).
	ClockProv uint8
	// ResidualMs is mapped wall clock minus raw receive time for the entry
	// keyframe: network delay plus sender/receiver clock disagreement. §8.5
	// item 4 asks for this to be recorded rather than assumed away.
	ResidualMs int64

	TotalRows uint32
	MinMs     uint64
	MaxMs     uint64
}

// Locate maps a timestamp to a byte offset and the containing GOP.
func (s *VideoStore) Locate(streamID uint8, atMs uint64, prefetch int) VideoLocateResult {
	s.mu.Lock()
	defer s.mu.Unlock()

	rows := s.vpt.snapshot()
	lo, hi := s.vpt.bounds()
	out := VideoLocateResult{
		ClockOffsetMs: s.vpt.clockOffset,
		TotalRows:     uint32(len(rows)),
		MinMs:         lo,
		MaxMs:         hi,
		ClockProv:     s.vpt.clockProv,
	}
	// Publish coverage even on a miss. Historical requests after EOF must
	// not masquerade as successful reads of the final (often partial) GOP.
	out.Coverage = coverageOf(rows, streamID, lo, hi)
	streamLo, streamHi := streamBounds(rows, streamID)
	if streamLo == streamHi {
		return out
	}
	if atMs != 0 && atMs > rows[streamHi-1].EndMs {
		return out
	}
	// atMs == 0 is the live probe ("newest GOP"), not "before the epoch".
	// locateEntry(0) used to return the earliest keyframe, so Live View
	// froze on the first still and every later t=0 locate was the same GOP.
	idx := -1
	if atMs == 0 {
		idx = lastKeyframe(rows, streamID)
	} else {
		idx = locateEntry(rows, streamID, atMs)
	}
	if idx < 0 {
		return out
	}
	entry := rows[idx]
	// locateEntry is "last keyframe at or before atMs". Across a recording
	// hole that is the final GOP of the previous clip, which is what made
	// Sep 7 11:16 catalog pins replay the Sep 6 22:50 0.45s tail after the
	// camera resumed. Consecutive GOPs are 1–2 s apart; anything farther
	// is a gap, not "hold the last picture".
	if atMs != 0 && atMs > entry.EndMs+videoLocateHoldMs {
		next := nextKeyframe(rows, idx, streamID)
		if next < 0 || rows[next].StartMs > atMs+videoLocateHoldMs {
			return out
		}
	}
	out.Found = true
	out.Entry = entry

	if prefetch > 0 {
		_, streamHi := streamBounds(rows, streamID)
		end := idx + 1 + prefetch
		if end > streamHi {
			end = streamHi
		}
		if end > idx+1 {
			out.Prefetch = append(out.Prefetch, rows[idx+1:end]...)
		}
	}

	// Coverage spans the whole table rather than the requested window: §8.2
	// says return the coverage map for the range plus the single entry
	// pointer, and at segment granularity the whole table is ~240 rows for 4 h.
	out.Coverage = coverageOf(rows, streamID, lo, hi)

	// SPS/PPS as of the entry keyframe, so a decoder configured from this
	// reply matches the bytes it is about to be fed.
	if e := s.entryIndexInRowLocked(out.Entry); e >= 0 {
		out.SPS = s.configForEntryLocked(e, nalTypeSPS)
		out.PPS = s.configForEntryLocked(e, nalTypePPS)
		out.ClockProv = s.entries[e].clockProv
		out.ResidualMs = int64(s.entries[e].timestampMs) - int64(s.entries[e].receiveMs)
	} else {
		out.SPS = cloneBytes(s.sps)
		out.PPS = cloneBytes(s.pps)
	}
	return out
}

// entryIndexInRowLocked finds the keyframe NAL inside a GOP row.
//
// Entries are appended in (segment, offset) order, so this is a binary search
// rather than a scan.
func (s *VideoStore) entryIndexInRowLocked(row GopRow) int {
	seg := uint64(row.SegmentID)
	start := int64(row.ByteOffset)
	end := start + int64(row.ByteLength)
	i := sort.Search(len(s.entries), func(k int) bool {
		e := s.entries[k]
		if e.segmentID != seg {
			return e.segmentID > seg
		}
		return e.offset >= start
	})
	for ; i < len(s.entries); i++ {
		e := s.entries[i]
		if e.segmentID != seg || e.offset >= end {
			break
		}
		if e.flags&VideoFlagKeyframe != 0 {
			return i
		}
	}
	return -1
}

// ReadRange returns a byte span from a segment so a pointer can be fetched
// directly, without a keyframe search (§8.2 transport change 2).
func (s *VideoStore) ReadRange(segmentID uint32, offset uint64, length uint32) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if length == 0 {
		return nil, nil
	}
	if length > videoRangeMaxBytes {
		return nil, fmt.Errorf("range of %d bytes exceeds the %d byte limit", length, videoRangeMaxBytes)
	}
	seg, ok := s.segments[uint64(segmentID)]
	if !ok {
		return nil, fmt.Errorf("video segment %d is missing", segmentID)
	}
	if int64(offset) >= seg.size {
		return nil, fmt.Errorf("offset %d is past the end of segment %d (%d bytes)", offset, segmentID, seg.size)
	}
	// Clamp rather than fail: the tail GOP of an active segment grows, so a
	// pointer fetched a moment ago can name more bytes than are on disk.
	avail := seg.size - int64(offset)
	if int64(length) > avail {
		length = uint32(avail)
	}
	file, err := s.segmentReadFileLocked(seg)
	if err != nil {
		return nil, err
	}
	buf := make([]byte, length)
	if _, err := file.ReadAt(buf, int64(offset)); err != nil && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return buf, nil
}

// PointerRows exposes the table for tests and diagnostics.
func (s *VideoStore) PointerRows() []GopRow {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.vpt.snapshot()
}

// rebuildVptLocked regenerates every row from the .idx-derived entries.
//
// The table is derived state, so this is both the recovery path and the
// self-heal for a truncated or deleted video.vpt.
func (s *VideoStore) rebuildVptLocked() {
	rows := s.vpt.rows
	_ = rows
	path := s.vpt.path
	offset := s.vpt.clockOffset
	prov := s.vpt.clockProv
	s.vpt.reset(path)
	s.vpt.clockOffset = offset
	s.vpt.clockProv = prov
	s.vpt.persisted = 0
	for _, e := range s.entries {
		s.vpt.add(videoDefaultStreamID, uint32(e.segmentID), e.timestampMs, uint64(e.offset), uint32(e.size),
			e.flags, e.nalType)
	}
	// Truncate the file so stale rows behind the rebuilt set cannot survive.
	if path != "" {
		if err := os.Truncate(path, 0); err != nil && !os.IsNotExist(err) {
			// Non-fatal: flush() rewrites the header and truncates to the new
			// length anyway. Reported so a broken table is not silent.
			fmt.Printf("[photon-server] video.vpt truncate failed: %v\n", err)
		}
	}
}

// backfillClockLocked re-derives wall clock for NALs already stored with
// receive time, now that an RTCP mapping exists.
//
// Bounded to the active segment: an RTCP sender report arrives within seconds
// of the stream starting, so the receive-stamped head of a recording is short.
// Without this, the first segment of every recording carries unknown skew even
// though the information needed to fix it was recorded alongside it.
func (s *VideoStore) backfillClockLocked() error {
	s.backfilled = true
	if s.clock == nil || s.active == nil {
		return nil
	}
	activeID := s.active.id
	changed := 0
	for i := range s.entries {
		e := &s.entries[i]
		if e.segmentID != activeID || e.clockProv != clockProvReceive || e.rtpTimestamp == 0 {
			continue
		}
		mapped, prov := s.clock.WallMs(e.rtpTimestamp, e.receiveMs)
		if prov != clockProvRTCP {
			continue
		}
		e.timestampMs = mapped
		e.clockProv = prov
		changed++
	}
	if changed == 0 {
		return nil
	}
	// Rewrite the segment's records so the correction survives a restart.
	if err := s.rewriteActiveIndexLocked(); err != nil {
		return err
	}
	s.rebuildVptLocked()
	fmt.Printf("[photon-server] video clock backfill: %d NALs in segment %d re-stamped from RTP\n",
		changed, activeID)
	return nil
}

func (s *VideoStore) rewriteActiveIndexLocked() error {
	if s.active == nil || s.active.idxFile == nil {
		return nil
	}
	activeID := s.active.id
	var records []videoNALRecord
	for _, e := range s.entries {
		if e.segmentID != activeID {
			continue
		}
		records = append(records, videoNALRecord{
			TimestampMs:  e.timestampMs,
			Offset:       uint64(e.offset),
			Size:         uint32(e.size),
			Flags:        e.flags,
			NALType:      e.nalType,
			ClockProv:    e.clockProv,
			RTPTimestamp: e.rtpTimestamp,
			ReceiveMs:    e.receiveMs,
		})
	}
	buf := make([]byte, len(records)*videoNALRecordSizeV2)
	for i, rec := range records {
		encodeNALRecord(buf[i*videoNALRecordSizeV2:(i+1)*videoNALRecordSizeV2], rec)
	}
	if _, err := s.active.idxFile.WriteAt(buf, int64(videoIndexHeaderSize)); err != nil {
		return err
	}
	if _, err := s.active.idxFile.Seek(0, io.SeekEnd); err != nil {
		return err
	}
	// Segment bounds move with the timestamps.
	if len(records) > 0 {
		s.active.startMs = records[0].TimestampMs
		s.active.endMs = records[len(records)-1].TimestampMs
		for _, rec := range records {
			if rec.TimestampMs < s.active.startMs {
				s.active.startMs = rec.TimestampMs
			}
			if rec.TimestampMs > s.active.endMs {
				s.active.endMs = rec.TimestampMs
			}
		}
		s.lastNALMS = s.active.endMs
	}
	return nil
}

// ---------------------------------------------------------------------------
// :6700 wire additions
// ---------------------------------------------------------------------------

// Extended subscribe modes. Modes 0 and 1 keep their exact 9-byte header so
// existing clients are unaffected; the mode byte selects the length, which is
// what makes growing the header backward-compatible (§8.2).
const (
	VideoModeReplayRange = uint8(2) // replay bounded by endTimeMs
	VideoModeLocate      = uint8(3) // video_locate
	VideoModeRange       = uint8(4) // byte-range read
	VideoModeClock       = uint8(5) // persist manual A/V nudge

	videoRangeMaxBytes = uint32(32 * 1024 * 1024)

	videoLocateStatusOK       = uint8(0)
	videoLocateStatusNotFound = uint8(1)
	videoLocateStatusError    = uint8(2)

	// Consecutive IDRs are 1–2 s apart. A cursor this far past a GOP is a
	// recording hole, not a frame-interval gap. Do not raise this to "until
	// EOF": that reintroduces the last-clip freeze.
	videoLocateHoldMs = uint64(2500)
)

// VideoRequestExt is the decoded subscribe header for any mode.
type VideoRequestExt struct {
	Mode        uint8
	StartTimeMs uint64
	EndTimeMs   uint64
	StreamID    uint8
	Prefetch    uint16
	SegmentID   uint32
	Offset      uint64
	Length      uint32
}

// DecodeVideoRequestExt reads the mode byte, then the remainder that mode
// implies. Modes 0 and 1 consume exactly videoRequestBytes.
func DecodeVideoRequestExt(r io.Reader) (VideoRequestExt, error) {
	var head [videoRequestBytes]byte
	if _, err := io.ReadFull(r, head[:]); err != nil {
		return VideoRequestExt{}, err
	}
	req := VideoRequestExt{
		Mode:        head[0],
		StartTimeMs: binary.BigEndian.Uint64(head[1:9]),
	}
	switch req.Mode {
	case VideoModeLive, VideoModeReplay:
		return req, nil
	case VideoModeReplayRange:
		var tail [8]byte
		if _, err := io.ReadFull(r, tail[:]); err != nil {
			return VideoRequestExt{}, err
		}
		req.EndTimeMs = binary.BigEndian.Uint64(tail[:])
		return req, nil
	case VideoModeLocate:
		var tail [3]byte
		if _, err := io.ReadFull(r, tail[:]); err != nil {
			return VideoRequestExt{}, err
		}
		req.StreamID = tail[0]
		req.Prefetch = binary.BigEndian.Uint16(tail[1:3])
		return req, nil
	case VideoModeClock:
		// StartTimeMs carries the signed offset as two's-complement bits.
		return req, nil
	case VideoModeRange:
		// StartTimeMs is unused here; the span is named explicitly.
		var tail [16]byte
		if _, err := io.ReadFull(r, tail[:]); err != nil {
			return VideoRequestExt{}, err
		}
		req.SegmentID = binary.BigEndian.Uint32(tail[0:4])
		req.Offset = binary.BigEndian.Uint64(tail[4:12])
		req.Length = binary.BigEndian.Uint32(tail[12:16])
		return req, nil
	default:
		return req, nil
	}
}

func EncodeVideoRequestExt(w io.Writer, req VideoRequestExt) error {
	buf := make([]byte, 0, videoRequestBytes+16)
	buf = append(buf, req.Mode)
	var u64 [8]byte
	binary.BigEndian.PutUint64(u64[:], req.StartTimeMs)
	buf = append(buf, u64[:]...)
	switch req.Mode {
	case VideoModeReplayRange:
		binary.BigEndian.PutUint64(u64[:], req.EndTimeMs)
		buf = append(buf, u64[:]...)
	case VideoModeLocate:
		var u16 [2]byte
		binary.BigEndian.PutUint16(u16[:], req.Prefetch)
		buf = append(buf, req.StreamID)
		buf = append(buf, u16[:]...)
	case VideoModeRange:
		var u32 [4]byte
		binary.BigEndian.PutUint32(u32[:], req.SegmentID)
		buf = append(buf, u32[:]...)
		binary.BigEndian.PutUint64(u64[:], req.Offset)
		buf = append(buf, u64[:]...)
		binary.BigEndian.PutUint32(u32[:], req.Length)
		buf = append(buf, u32[:]...)
	}
	_, err := w.Write(buf)
	return err
}

// EncodeVideoLocateReply serialises a locate answer.
//
//	u32  payloadLen        bytes that follow
//	u8   status
//	i64  clockOffsetMs
//	u8   clockProvenance
//	i64  residualMs
//	u32  totalRows
//	u64  minMs
//	u64  maxMs
//	u16  spsLen, sps
//	u16  ppsLen, pps
//	u16  rowCount, rows       entry first, then prefetch, 40 B each
//	u16  coverageCount, cov   u32 segmentId + u64 startMs + u64 endMs
func EncodeVideoLocateReply(w io.Writer, res VideoLocateResult) error {
	body := make([]byte, 0, 128+len(res.Prefetch)*vptRowSize+len(res.Coverage)*20)
	status := videoLocateStatusOK
	if !res.Found {
		status = videoLocateStatusNotFound
	}
	body = append(body, status)
	body = vptAppendU64(body, uint64(res.ClockOffsetMs))
	body = append(body, res.ClockProv)
	body = vptAppendU64(body, uint64(res.ResidualMs))
	body = vptAppendU32(body, res.TotalRows)
	body = vptAppendU64(body, res.MinMs)
	body = vptAppendU64(body, res.MaxMs)
	body = vptAppendBytes(body, res.SPS)
	body = vptAppendBytes(body, res.PPS)

	rows := make([]GopRow, 0, 1+len(res.Prefetch))
	if res.Found {
		rows = append(rows, res.Entry)
		rows = append(rows, res.Prefetch...)
	}
	body = vptAppendU16(body, uint16(len(rows)))
	for _, r := range rows {
		var rowBuf [vptRowSize]byte
		encodeGopRow(rowBuf[:], r)
		body = append(body, rowBuf[:]...)
	}
	body = vptAppendU16(body, uint16(len(res.Coverage)))
	for _, c := range res.Coverage {
		body = vptAppendU32(body, c.SegmentID)
		body = vptAppendU64(body, c.StartMs)
		body = vptAppendU64(body, c.EndMs)
	}

	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(body)))
	if _, err := w.Write(lenBuf[:]); err != nil {
		return err
	}
	_, err := w.Write(body)
	return err
}

// EncodeVideoRangeReply serialises a byte-range answer: u32 length + bytes.
// A zero length with a non-OK status byte would be ambiguous, so failures are
// reported by closing without a reply, matching how the live path signals a
// dead subscription.
func EncodeVideoClockReply(w io.Writer, offsetMs int64, prov uint8) error {
	body := make([]byte, 0, 9)
	body = vptAppendU64(body, uint64(offsetMs))
	body = append(body, prov)
	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(body)))
	if _, err := w.Write(lenBuf[:]); err != nil {
		return err
	}
	_, err := w.Write(body)
	return err
}

func EncodeVideoRangeReply(w io.Writer, payload []byte) error {
	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(payload)))
	if _, err := w.Write(lenBuf[:]); err != nil {
		return err
	}
	if len(payload) == 0 {
		return nil
	}
	_, err := w.Write(payload)
	return err
}

func vptAppendU16(dst []byte, v uint16) []byte {
	var b [2]byte
	binary.BigEndian.PutUint16(b[:], v)
	return append(dst, b[:]...)
}

func vptAppendU32(dst []byte, v uint32) []byte {
	var b [4]byte
	binary.BigEndian.PutUint32(b[:], v)
	return append(dst, b[:]...)
}

func vptAppendU64(dst []byte, v uint64) []byte {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], v)
	return append(dst, b[:]...)
}

func vptAppendBytes(dst []byte, payload []byte) []byte {
	dst = vptAppendU16(dst, uint16(len(payload)))
	return append(dst, payload...)
}
