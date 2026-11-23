package main

import (
	"encoding/binary"
	"errors"
	"math"
)

// Binary messages of protocol v2 (photon-next/docs/protocol-v2.md). Every binary
// message starts with a 12-byte header; all integers are little-endian.
const (
	v2Magic      = 0x3250 // "P2" on the wire
	v2Version    = 2
	v2HeaderSize = 12

	v2MsgLiveFrames = 0x20
	v2MsgGap        = 0x21
	v2MsgTile       = 0x22

	// v2BusUnknown is the bus of a frame whose ID no DBC defines, or of every
	// frame when there is no catalog.
	v2BusUnknown = 0xFF
)

// v2Header is a binary message's header, as the tests read it.
type v2Header struct {
	Type byte
	Seq  uint32
	Len  uint32 // payload bytes after the header
}

// appendV2Header appends a header for a message of type typ with n payload
// bytes. seq is 0: the writer stamps it just before the message is sent.
func appendV2Header(b []byte, typ byte, n int) []byte {
	b = binary.LittleEndian.AppendUint16(b, v2Magic)
	b = append(b, v2Version, typ)
	b = binary.LittleEndian.AppendUint32(b, 0)
	return binary.LittleEndian.AppendUint32(b, uint32(n))
}

// liveSpanFrames is how many leading frames fit in one LiveFrames message. The
// message holds each frame's time as microseconds after the earliest one in it,
// in 32 bits, so the times must span less than 2^32 µs. The result is at least 1.
func liveSpanFrames(frames []liveFrame) int {
	lo, hi := frames[0].tNS, frames[0].tNS
	for i, f := range frames {
		lo, hi = min(lo, f.tNS), max(hi, f.tNS)
		if (hi-lo)/1000 > math.MaxUint32 {
			return i
		}
	}
	return len(frames)
}

// encodeLiveFrames returns a LiveFrames message holding frames, which must not be
// empty and must pass liveSpanFrames. The base time is the earliest frame's, not
// the first one's: the ring holds arrival order, and car times can go backwards.
// busOf gives each frame's bus index in the catalog, or v2BusUnknown.
func encodeLiveFrames(frames []liveFrame, busOf func(id uint32) uint8) []byte {
	base := frames[0].tNS
	size := v2HeaderSize + 12 // header, then base_ns, count and reserved
	for _, f := range frames {
		base = min(base, f.tNS)
		size += 10 + int(f.n) // dt_us, can_id, bus, len, data
	}
	b := make([]byte, 0, size)
	b = appendV2Header(b, v2MsgLiveFrames, size-v2HeaderSize)
	b = binary.LittleEndian.AppendUint64(b, uint64(base))
	b = binary.LittleEndian.AppendUint16(b, uint16(len(frames)))
	b = binary.LittleEndian.AppendUint16(b, 0)
	for _, f := range frames {
		b = binary.LittleEndian.AppendUint32(b, uint32((f.tNS-base)/1000))
		b = binary.LittleEndian.AppendUint32(b, f.id)
		b = append(b, busOf(f.id), f.n)
		b = append(b, f.data[:f.n]...)
	}
	return b
}

// encodeGap returns a Gap message: the client fell behind, and the frames between
// fromNS and toNS were skipped.
func encodeGap(fromNS, toNS int64) []byte {
	b := make([]byte, 0, v2HeaderSize+16)
	b = appendV2Header(b, v2MsgGap, 16)
	b = binary.LittleEndian.AppendUint64(b, uint64(fromNS))
	return binary.LittleEndian.AppendUint64(b, uint64(toNS))
}

// Tile message flags, and the size limit and fixed part of a Tile piece.
const (
	v2TileSealed   = 1 << 0 // the tile never changes
	v2TileLast     = 1 << 1 // the last piece of the tile
	v2TilePieceMax = 64 << 10
	v2TileFixed    = 28 // the Tile payload before its samples: qid to n
)

// tileData is the samples of one tile, in time order. A level 0 tile has raw
// values; a level above 0 has the min, max and mean of each bucket.
type tileData struct {
	level uint8
	index int64
	tMS   []float64 // sample times, Unix ms
	raw   []float64 // level 0
	lo    []float64 // level above 0: min
	hi    []float64 // level above 0: max
	mean  []float64 // level above 0: mean
}

// values returns the value arrays that follow the times on the wire.
func (t *tileData) values() [][]float64 {
	if t.level == 0 {
		return [][]float64{t.raw}
	}
	return [][]float64{t.lo, t.hi, t.mean}
}

// encodeTile returns the Tile messages that answer request qid, one per piece. A
// piece holds as many samples as fit in v2TilePieceMax bytes, and the last piece
// has the last flag; a tile with no samples is one piece with n 0. The messages
// have seq 0: the writer stamps it when it sends them.
func encodeTile(qid, signal uint32, sealed bool, t *tileData) [][]byte {
	cols := 2 // t_ms and raw
	if t.level > 0 {
		cols = 4 // t_ms, min, max, mean
	}
	perPiece := (v2TilePieceMax - v2HeaderSize - v2TileFixed) / (8 * cols)
	n := len(t.tMS)
	var msgs [][]byte
	for off := 0; ; {
		k := min(perPiece, n-off)
		flags := byte(0)
		if sealed {
			flags |= v2TileSealed
		}
		last := off+k == n
		if last {
			flags |= v2TileLast
		}
		msgs = append(msgs, encodeTilePiece(qid, signal, flags, t, off, k))
		if last {
			return msgs
		}
		off += k
	}
}

// encodeTilePiece returns the Tile message with samples off to off+n of t.
func encodeTilePiece(qid, signal uint32, flags byte, t *tileData, off, n int) []byte {
	arrays := append([][]float64{t.tMS}, t.values()...)
	size := v2HeaderSize + v2TileFixed + 8*n*len(arrays)
	b := make([]byte, 0, size)
	b = appendV2Header(b, v2MsgTile, size-v2HeaderSize)
	b = binary.LittleEndian.AppendUint32(b, qid)
	b = binary.LittleEndian.AppendUint32(b, signal)
	b = append(b, t.level, flags, 0, 0) // level, flags, reserved
	b = binary.LittleEndian.AppendUint64(b, uint64(t.index))
	b = binary.LittleEndian.AppendUint32(b, uint32(off))
	b = binary.LittleEndian.AppendUint32(b, uint32(n))
	for _, a := range arrays {
		for _, v := range a[off : off+n] {
			b = binary.LittleEndian.AppendUint64(b, math.Float64bits(v))
		}
	}
	return b
}

var errV2Message = errors.New("protocol v2: malformed binary message")

// decodeV2Header reads the header of b and checks that b holds exactly the
// payload it announces.
func decodeV2Header(b []byte) (v2Header, error) {
	if len(b) < v2HeaderSize || binary.LittleEndian.Uint16(b[0:2]) != v2Magic || b[2] != v2Version {
		return v2Header{}, errV2Message
	}
	h := v2Header{Type: b[3], Seq: binary.LittleEndian.Uint32(b[4:8]), Len: binary.LittleEndian.Uint32(b[8:12])}
	if uint64(len(b)) != v2HeaderSize+uint64(h.Len) {
		return v2Header{}, errV2Message
	}
	return h, nil
}

// liveRecord is one frame of a LiveFrames message, with its time made absolute.
type liveRecord struct {
	TNS  int64
	ID   uint32
	Bus  uint8
	Data []byte
}

// decodeLiveFrames reads a LiveFrames message back into its frames.
func decodeLiveFrames(b []byte) ([]liveRecord, error) {
	h, err := decodeV2Header(b)
	if err != nil || h.Type != v2MsgLiveFrames || h.Len < 12 {
		return nil, errV2Message
	}
	p := b[v2HeaderSize:]
	base := int64(binary.LittleEndian.Uint64(p[0:8]))
	count := int(binary.LittleEndian.Uint16(p[8:10]))
	p = p[12:]
	recs := make([]liveRecord, 0, count)
	for range count {
		if len(p) < 10 {
			return nil, errV2Message
		}
		n := int(p[9])
		if n > 64 || len(p) < 10+n {
			return nil, errV2Message
		}
		recs = append(recs, liveRecord{
			TNS:  base + int64(binary.LittleEndian.Uint32(p[0:4]))*1000,
			ID:   binary.LittleEndian.Uint32(p[4:8]),
			Bus:  p[8],
			Data: p[10 : 10+n],
		})
		p = p[10+n:]
	}
	if len(p) != 0 {
		return nil, errV2Message
	}
	return recs, nil
}

// decodeGap reads a Gap message back into its times.
func decodeGap(b []byte) (fromNS, toNS int64, err error) {
	h, err := decodeV2Header(b)
	if err != nil || h.Type != v2MsgGap || h.Len != 16 {
		return 0, 0, errV2Message
	}
	return int64(binary.LittleEndian.Uint64(b[12:20])), int64(binary.LittleEndian.Uint64(b[20:28])), nil
}

// v2Tile is one Tile message as the tests read it. Raw is set for level 0; Min,
// Max and Mean for a level above 0.
type v2Tile struct {
	QID, Signal uint32
	Level       uint8
	Sealed      bool
	Last        bool
	Index       int64
	Offset, N   uint32
	TMS         []float64
	Raw         []float64
	Min         []float64
	Max         []float64
	Mean        []float64
}

// decodeTile reads a Tile message back into its piece.
func decodeTile(b []byte) (v2Tile, error) {
	h, err := decodeV2Header(b)
	if err != nil || h.Type != v2MsgTile || h.Len < v2TileFixed {
		return v2Tile{}, errV2Message
	}
	p := b[v2HeaderSize:]
	t := v2Tile{
		QID:    binary.LittleEndian.Uint32(p[0:4]),
		Signal: binary.LittleEndian.Uint32(p[4:8]),
		Level:  p[8],
		Sealed: p[9]&v2TileSealed != 0,
		Last:   p[9]&v2TileLast != 0,
		Index:  int64(binary.LittleEndian.Uint64(p[12:20])),
		Offset: binary.LittleEndian.Uint32(p[20:24]),
		N:      binary.LittleEndian.Uint32(p[24:28]),
	}
	arrays := 2
	if t.Level > 0 {
		arrays = 4
	}
	n := uint64(t.N)
	if uint64(h.Len) != v2TileFixed+8*uint64(arrays)*n {
		return v2Tile{}, errV2Message
	}
	read := func(k int) []float64 {
		out := make([]float64, t.N)
		for i := range out {
			out[i] = math.Float64frombits(binary.LittleEndian.Uint64(p[v2TileFixed+8*(uint64(k)*n+uint64(i)):]))
		}
		return out
	}
	t.TMS = read(0)
	if t.Level == 0 {
		t.Raw = read(1)
	} else {
		t.Min, t.Max, t.Mean = read(1), read(2), read(3)
	}
	return t, nil
}
