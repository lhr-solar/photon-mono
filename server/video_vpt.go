package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sort"
)

// video.vpt — the video pointer table (tsdb-architecture.md §8.1).
//
// One row per GOP, built live as NALs arrive rather than by a post-processing
// pass. A row is the byte span a decoder needs to start at a keyframe and play
// forward, so a timestamp can be turned into a seek without scanning the
// stream.
//
// The table is derived state: it is rebuilt from the authoritative per-segment
// .idx records on recovery, so deleting it loses nothing.
const (
	vptMagic      = uint32(0x56505431) // "VPT1"
	vptVersion    = uint16(1)
	vptHeaderSize = 64
	vptRowSize    = 40

	// Row flags.
	//
	// vptFlagConfigInline says the byte span begins with SPS/PPS, so a client
	// that fetches only this range can configure a decoder from it.
	vptFlagConfigInline = uint8(1 << 0)
	// vptFlagContinuation says the row does not begin at a keyframe. A GOP
	// split across a segment boundary (which the 128 MiB / 120 s hard rotation
	// cap can force) yields a keyframe row plus one continuation row. A
	// continuation row is a legal byte range to read but not a legal place to
	// start decoding, so Locate never returns one as an entry.
	vptFlagContinuation = uint8(1 << 1)
)

// Clock provenance for video timestamps (§8.5). Ordered worst to best so a
// caller can compare: anything below clockProvRTCP means A/V sync is unverified.
const (
	clockProvReceive = uint8(0)
	clockProvManual  = uint8(1)
	clockProvNTP     = uint8(2)
	clockProvRTCP    = uint8(3)
)

func clockProvenanceName(p uint8) string {
	switch p {
	case clockProvRTCP:
		return "rtcp"
	case clockProvNTP:
		return "ntp"
	case clockProvManual:
		return "manual"
	default:
		return "receive"
	}
}

// GopRow is the 40-byte on-disk pointer row. Field order and offsets are fixed
// by §8.1; do not reorder.
type GopRow struct {
	StartMs    uint64 // keyframe timestamp
	EndMs      uint64 // last NAL before the next keyframe
	ByteOffset uint64 // into the segment's .h264
	ByteLength uint32
	SegmentID  uint32
	NalCount   uint32
	StreamID   uint8
	Flags      uint8
}

func (r GopRow) IsContinuation() bool { return r.Flags&vptFlagContinuation != 0 }
func (r GopRow) HasInlineConfig() bool {
	return r.Flags&vptFlagConfigInline != 0
}

func encodeGopRow(dst []byte, r GopRow) {
	binary.BigEndian.PutUint64(dst[0:8], r.StartMs)
	binary.BigEndian.PutUint64(dst[8:16], r.EndMs)
	binary.BigEndian.PutUint64(dst[16:24], r.ByteOffset)
	binary.BigEndian.PutUint32(dst[24:28], r.ByteLength)
	binary.BigEndian.PutUint32(dst[28:32], r.SegmentID)
	binary.BigEndian.PutUint32(dst[32:36], r.NalCount)
	dst[36] = r.StreamID
	dst[37] = r.Flags
	binary.BigEndian.PutUint16(dst[38:40], 0)
}

func decodeGopRow(src []byte) GopRow {
	return GopRow{
		StartMs:    binary.BigEndian.Uint64(src[0:8]),
		EndMs:      binary.BigEndian.Uint64(src[8:16]),
		ByteOffset: binary.BigEndian.Uint64(src[16:24]),
		ByteLength: binary.BigEndian.Uint32(src[24:28]),
		SegmentID:  binary.BigEndian.Uint32(src[28:32]),
		NalCount:   binary.BigEndian.Uint32(src[32:36]),
		StreamID:   src[36],
		Flags:      src[37],
	}
}

// vptHeader is the 64-byte file header. It carries the clock offset and its
// provenance so a reader of the table alone knows whether video timestamps are
// comparable to telemetry timestamps (§8.5).
type vptHeader struct {
	rowCount      uint32
	minMs         uint64
	maxMs         uint64
	clockOffsetMs int64
	clockProv     uint8
}

func encodeVptHeader(h vptHeader) []byte {
	buf := make([]byte, vptHeaderSize)
	binary.BigEndian.PutUint32(buf[0:4], vptMagic)
	binary.BigEndian.PutUint16(buf[4:6], vptVersion)
	binary.BigEndian.PutUint16(buf[6:8], uint16(vptHeaderSize))
	binary.BigEndian.PutUint16(buf[8:10], uint16(vptRowSize))
	binary.BigEndian.PutUint32(buf[12:16], h.rowCount)
	binary.BigEndian.PutUint64(buf[16:24], h.minMs)
	binary.BigEndian.PutUint64(buf[24:32], h.maxMs)
	binary.BigEndian.PutUint64(buf[32:40], uint64(h.clockOffsetMs))
	buf[40] = h.clockProv
	return buf
}

func decodeVpt(data []byte) (vptHeader, []GopRow, error) {
	if len(data) < vptHeaderSize {
		return vptHeader{}, nil, errors.New("video.vpt is shorter than its header")
	}
	if binary.BigEndian.Uint32(data[0:4]) != vptMagic {
		return vptHeader{}, nil, errors.New("video.vpt magic mismatch")
	}
	if binary.BigEndian.Uint16(data[4:6]) != vptVersion {
		return vptHeader{}, nil, fmt.Errorf("video.vpt version %d unsupported", binary.BigEndian.Uint16(data[4:6]))
	}
	headerBytes := int(binary.BigEndian.Uint16(data[6:8]))
	rowBytes := int(binary.BigEndian.Uint16(data[8:10]))
	if headerBytes < vptHeaderSize || rowBytes != vptRowSize || headerBytes > len(data) {
		return vptHeader{}, nil, errors.New("video.vpt header is malformed")
	}
	h := vptHeader{
		rowCount:      binary.BigEndian.Uint32(data[12:16]),
		minMs:         binary.BigEndian.Uint64(data[16:24]),
		maxMs:         binary.BigEndian.Uint64(data[24:32]),
		clockOffsetMs: int64(binary.BigEndian.Uint64(data[32:40])),
		clockProv:     data[40],
	}
	rows := make([]GopRow, 0, h.rowCount)
	for off := headerBytes; off+rowBytes <= len(data); off += rowBytes {
		rows = append(rows, decodeGopRow(data[off:off+rowBytes]))
	}
	return h, rows, nil
}

// vptBuilder turns the NAL stream into GOP rows as it arrives.
//
// It is fed in append order and holds exactly one open row: the GOP that the
// next keyframe will close. The open row is visible to queries immediately but
// is only appended to disk once closed, because its length and end timestamp
// are still growing.
type vptBuilder struct {
	rows []GopRow

	open      bool
	cur       GopRow
	persisted int // rows already appended to the file

	// Trailing run of config NALs (SPS/PPS) seen since the last keyframe.
	// A keyframe pulls them into its byte span so the GOP is independently
	// decodable, which is what vptFlagConfigInline advertises.
	cfgRun      bool
	cfgSegment  uint32
	cfgOffset   uint64
	cfgLength   uint32
	cfgCount    uint32
	cfgStartMs  uint64
	path        string
	clockOffset int64
	clockProv   uint8
	dirty       bool
}

func (b *vptBuilder) reset(path string) {
	b.rows = nil
	b.open = false
	b.cur = GopRow{}
	b.persisted = 0
	b.clearConfigRun()
	b.path = path
	b.dirty = true
}

func (b *vptBuilder) clearConfigRun() {
	b.cfgRun = false
	b.cfgSegment = 0
	b.cfgOffset = 0
	b.cfgLength = 0
	b.cfgCount = 0
	b.cfgStartMs = 0
}

func (b *vptBuilder) closeOpen() {
	if !b.open {
		return
	}
	b.rows = append(b.rows, b.cur)
	b.open = false
	b.cur = GopRow{}
	b.dirty = true
}

// add folds one appended NAL into the table.
func (b *vptBuilder) add(streamID uint8, segmentID uint32, timestampMs uint64, offset uint64, size uint32,
	flags uint8, nalType uint8) {

	if flags&VideoFlagConfig != 0 {
		// Extend or start the trailing config run, but only while it stays
		// contiguous in one segment. A gap means the bytes cannot be handed
		// out as a single range.
		if b.cfgRun && b.cfgSegment == segmentID && b.cfgOffset+uint64(b.cfgLength) == offset {
			b.cfgLength += size
			b.cfgCount++
		} else {
			b.cfgRun = true
			b.cfgSegment = segmentID
			b.cfgOffset = offset
			b.cfgLength = size
			b.cfgCount = 1
			b.cfgStartMs = timestampMs
		}
		// Config NALs also belong to whatever GOP is open, so its byte span
		// stays contiguous with the stream. EndMs deliberately does not move:
		// a config NAL that turns out to precede the next keyframe gets pulled
		// into that keyframe's row, and if EndMs had followed it the two rows
		// would overlap and a timestamp on the boundary would be in both.
		if b.open && b.cur.SegmentID == segmentID &&
			b.cur.ByteOffset+uint64(b.cur.ByteLength) == offset {
			b.cur.ByteLength += size
			b.cur.NalCount++
		}
		_ = nalType
		return
	}

	if flags&VideoFlagKeyframe != 0 {
		start := offset
		length := size
		nals := uint32(1)
		rowFlags := uint8(0)
		startMs := timestampMs
		// Pull an immediately preceding, contiguous config run into this GOP.
		if b.cfgRun && b.cfgSegment == segmentID && b.cfgOffset+uint64(b.cfgLength) == offset {
			// Those bytes may already be counted in the GOP we are about to
			// close; trim them off it so no byte is claimed twice.
			if b.open && b.cur.SegmentID == segmentID &&
				b.cur.ByteLength >= b.cfgLength &&
				b.cur.ByteOffset+uint64(b.cur.ByteLength) == offset {
				b.cur.ByteLength -= b.cfgLength
				if b.cur.NalCount >= b.cfgCount {
					b.cur.NalCount -= b.cfgCount
				}
			}
			start = b.cfgOffset
			length += b.cfgLength
			nals += b.cfgCount
			rowFlags |= vptFlagConfigInline
			if b.cfgStartMs < startMs {
				startMs = b.cfgStartMs
			}
		}
		b.closeOpen()
		b.clearConfigRun()
		b.open = true
		b.cur = GopRow{
			StartMs:    startMs,
			EndMs:      timestampMs,
			ByteOffset: start,
			ByteLength: length,
			SegmentID:  segmentID,
			NalCount:   nals,
			StreamID:   streamID,
			Flags:      rowFlags,
		}
		return
	}

	b.clearConfigRun()

	// Non-keyframe. Extend the open GOP when it stays contiguous, otherwise
	// start a continuation row: a forced rotation can move the rest of a GOP
	// into the next segment, and those bytes still have to be addressable.
	if b.open && b.cur.SegmentID == segmentID &&
		b.cur.ByteOffset+uint64(b.cur.ByteLength) == offset {
		b.cur.ByteLength += size
		b.cur.NalCount++
		if timestampMs > b.cur.EndMs {
			b.cur.EndMs = timestampMs
		}
		return
	}
	prevStream := streamID
	if b.open {
		prevStream = b.cur.StreamID
	}
	b.closeOpen()
	b.open = true
	b.cur = GopRow{
		StartMs:    timestampMs,
		EndMs:      timestampMs,
		ByteOffset: offset,
		ByteLength: size,
		SegmentID:  segmentID,
		NalCount:   1,
		StreamID:   prevStream,
		Flags:      vptFlagContinuation,
	}
}

// snapshot returns every row including the still-growing open one, sorted by
// (streamId, startMs) as §8.1 requires. Rows arrive in time order for a single
// stream, so the sort only does work once multi-camera is real.
func (b *vptBuilder) snapshot() []GopRow {
	out := make([]GopRow, 0, len(b.rows)+1)
	out = append(out, b.rows...)
	if b.open {
		out = append(out, b.cur)
	}
	if !sort.SliceIsSorted(out, func(i, j int) bool {
		if out[i].StreamID != out[j].StreamID {
			return out[i].StreamID < out[j].StreamID
		}
		return out[i].StartMs < out[j].StartMs
	}) {
		sort.SliceStable(out, func(i, j int) bool {
			if out[i].StreamID != out[j].StreamID {
				return out[i].StreamID < out[j].StreamID
			}
			return out[i].StartMs < out[j].StartMs
		})
	}
	return out
}

func (b *vptBuilder) bounds() (uint64, uint64) {
	var lo, hi uint64
	first := true
	for _, r := range b.snapshot() {
		if first || r.StartMs < lo {
			lo = r.StartMs
		}
		if first || r.EndMs > hi {
			hi = r.EndMs
		}
		first = false
	}
	return lo, hi
}

// flush appends newly closed rows to video.vpt and refreshes the header.
//
// Only closed rows are persisted, which keeps the file append-only in the
// common case: one 40-byte write per GOP, ~1800 rows/hour.
func (b *vptBuilder) flush() error {
	if b.path == "" {
		return nil
	}
	pending := len(b.rows) - b.persisted
	if pending <= 0 && !b.dirty {
		return nil
	}
	file, err := os.OpenFile(b.path, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer file.Close()

	if pending > 0 {
		buf := make([]byte, pending*vptRowSize)
		for i := 0; i < pending; i++ {
			encodeGopRow(buf[i*vptRowSize:(i+1)*vptRowSize], b.rows[b.persisted+i])
		}
		off := int64(vptHeaderSize) + int64(b.persisted)*vptRowSize
		if _, err := file.WriteAt(buf, off); err != nil {
			return err
		}
		b.persisted = len(b.rows)
	}
	lo, hi := b.bounds()
	hdr := encodeVptHeader(vptHeader{
		rowCount:      uint32(b.persisted),
		minMs:         lo,
		maxMs:         hi,
		clockOffsetMs: b.clockOffset,
		clockProv:     b.clockProv,
	})
	if _, err := file.WriteAt(hdr, 0); err != nil {
		return err
	}
	// A partial rewrite could leave a stale tail behind a shorter table.
	if err := file.Truncate(int64(vptHeaderSize) + int64(b.persisted)*vptRowSize); err != nil {
		return err
	}
	b.dirty = false
	return file.Sync()
}

// flushAll persists the open row too. Used at shutdown, where there is no
// following keyframe to close it.
func (b *vptBuilder) flushAll() error {
	b.closeOpen()
	return b.flush()
}

func nextKeyframe(rows []GopRow, idx int, streamID uint8) int {
	_, hi := streamBounds(rows, streamID)
	for j := idx + 1; j < hi; j++ {
		if !rows[j].IsContinuation() {
			return j
		}
	}
	return -1
}

func lastKeyframe(rows []GopRow, streamID uint8) int {
	lo, hi := streamBounds(rows, streamID)
	now := videoNowMS()
	// Skip only absurdly future rows (broken RTCP, hours ahead). A 2.5 s cap
	// treated RTP/NTP skew of ~1 min as "future" and walked back to an old
	// still — Live View froze on that shop GOP at 0 fps while new segments
	// were still being written.
	const futureSkewMs = 10 * 60 * 1000
	for j := hi - 1; j >= lo; j-- {
		if rows[j].IsContinuation() {
			continue
		}
		if rows[j].StartMs > now+futureSkewMs {
			continue
		}
		return j
	}
	return -1
}

// locateEntry finds the GOP a timestamp lands in: the last keyframe row at or
// before atMs. Returns the index into rows, or -1.
//
// Rows are sorted, so this is the O(log K) search §8 says already exists for
// keyframes, lifted to GOP granularity.
func locateEntry(rows []GopRow, streamID uint8, atMs uint64) int {
	lo, hi := streamBounds(rows, streamID)
	if lo == hi {
		return -1
	}
	// First row of this stream that starts strictly after atMs.
	i := lo + sort.Search(hi-lo, func(k int) bool { return rows[lo+k].StartMs > atMs })
	for j := i - 1; j >= lo; j-- {
		if !rows[j].IsContinuation() {
			return j
		}
	}
	// atMs is before every keyframe: the earliest one is the only place a
	// decoder can start, so hand that back rather than nothing.
	for j := lo; j < hi; j++ {
		if !rows[j].IsContinuation() {
			return j
		}
	}
	return -1
}

func streamBounds(rows []GopRow, streamID uint8) (int, int) {
	lo := sort.Search(len(rows), func(k int) bool { return rows[k].StreamID >= streamID })
	hi := sort.Search(len(rows), func(k int) bool { return rows[k].StreamID > streamID })
	return lo, hi
}

// SegmentCoverage is one row of the coarse coverage map (§8.2): segment
// granularity, so a 4 h range is ~240 rows and the scrubber can shade where
// video exists instead of the user finding gaps by landing in one.
type SegmentCoverage struct {
	SegmentID uint32
	StartMs   uint64
	EndMs     uint64
}

func coverageOf(rows []GopRow, streamID uint8, fromMs, toMs uint64) []SegmentCoverage {
	lo, hi := streamBounds(rows, streamID)
	var out []SegmentCoverage
	for i := lo; i < hi; i++ {
		r := rows[i]
		if r.EndMs < fromMs || r.StartMs > toMs {
			continue
		}
		if n := len(out); n > 0 && out[n-1].SegmentID == r.SegmentID {
			if r.EndMs > out[n-1].EndMs {
				out[n-1].EndMs = r.EndMs
			}
			continue
		}
		out = append(out, SegmentCoverage{SegmentID: r.SegmentID, StartMs: r.StartMs, EndMs: r.EndMs})
	}
	return out
}
