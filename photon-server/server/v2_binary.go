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

	v2MsgLiveSamples = 0x20
	v2MsgGap         = 0x21
	v2MsgTile        = 0x22
	v2MsgVideoChunk  = 0x23

	// v2LiveSamplesMax is the most records in one LiveSamples message: 64 KiB of them.
	v2LiveSamplesMax = 4096
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

// liveSample is one decoded value in a LiveSamples message, with its time made
// absolute. Signal is the catalog signal index.
type liveSample struct {
	TNS    int64
	Signal uint32
	Value  float64
}

// liveSampleSize is the bytes one record of a LiveSamples message takes.
const liveSampleSize = 16

// liveSampleMessages returns the LiveSamples messages that carry recs, in order. A
// message ends when it holds v2LiveSamplesMax records, and before a record that is
// earlier than the message's base time or more than 2^32 µs after it, since dt_us
// cannot say that. The result is empty for no records.
func liveSampleMessages(recs []liveSample) [][]byte {
	var msgs [][]byte
	for start := 0; start < len(recs); {
		base := recs[start].TNS
		end := start
		for end < len(recs) && end-start < v2LiveSamplesMax {
			dt := recs[end].TNS - base
			if dt < 0 || dt/1000 > math.MaxUint32 {
				break
			}
			end++
		}
		msgs = append(msgs, encodeLiveSamples(base, recs[start:end]))
		start = end
	}
	return msgs
}

// encodeLiveSamples returns a LiveSamples message holding recs, which must be
// within the limits liveSampleMessages keeps: at or after baseNS, at most 2^32 µs
// after it, and at most v2LiveSamplesMax of them.
func encodeLiveSamples(baseNS int64, recs []liveSample) []byte {
	size := v2HeaderSize + 12 + liveSampleSize*len(recs) // header, then base_ns, count and reserved
	b := make([]byte, 0, size)
	b = appendV2Header(b, v2MsgLiveSamples, size-v2HeaderSize)
	b = binary.LittleEndian.AppendUint64(b, uint64(baseNS))
	b = binary.LittleEndian.AppendUint16(b, uint16(len(recs)))
	b = binary.LittleEndian.AppendUint16(b, 0)
	for _, r := range recs {
		b = binary.LittleEndian.AppendUint32(b, uint32((r.TNS-baseNS)/1000))
		b = binary.LittleEndian.AppendUint32(b, r.Signal)
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(r.Value))
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

// tileData holds the samples of one Tile piece, in time order, as the tile reader
// fills them. A level 0 piece has raw values; a level above 0 has the min, max and
// mean of each bucket.
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

// tilePieceSamples is how many samples a Tile piece of level holds at most: as many
// as fit in v2TilePieceMax bytes.
func tilePieceSamples(level int) int {
	cols := 2 // t_ms and raw
	if level > 0 {
		cols = 4 // t_ms, min, max, mean
	}
	return (v2TilePieceMax - v2HeaderSize - v2TileFixed) / (8 * cols)
}

// encodeTilePiece returns the Tile message with the samples of t. They are the
// samples of the tile from off on. The message has seq 0: the writer stamps it when
// it sends it.
func encodeTilePiece(qid, signal uint32, flags byte, off uint32, t *tileData) []byte {
	arrays := append([][]float64{t.tMS}, t.values()...)
	n := len(t.tMS)
	size := v2HeaderSize + v2TileFixed + 8*n*len(arrays)
	b := make([]byte, 0, size)
	b = appendV2Header(b, v2MsgTile, size-v2HeaderSize)
	b = binary.LittleEndian.AppendUint32(b, qid)
	b = binary.LittleEndian.AppendUint32(b, signal)
	b = append(b, t.level, flags, 0, 0) // level, flags, reserved
	b = binary.LittleEndian.AppendUint64(b, uint64(t.index))
	b = binary.LittleEndian.AppendUint32(b, off)
	b = binary.LittleEndian.AppendUint32(b, uint32(n))
	for _, a := range arrays {
		for _, v := range a {
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

// decodeLiveSamples reads a LiveSamples message back into its records, with their
// times made absolute.
func decodeLiveSamples(b []byte) ([]liveSample, error) {
	h, err := decodeV2Header(b)
	if err != nil || h.Type != v2MsgLiveSamples || h.Len < 12 {
		return nil, errV2Message
	}
	p := b[v2HeaderSize:]
	base := int64(binary.LittleEndian.Uint64(p[0:8]))
	count := int(binary.LittleEndian.Uint16(p[8:10]))
	p = p[12:]
	if len(p) != count*liveSampleSize {
		return nil, errV2Message
	}
	recs := make([]liveSample, 0, count)
	for range count {
		recs = append(recs, liveSample{
			TNS:    base + int64(binary.LittleEndian.Uint32(p[0:4]))*1000,
			Signal: binary.LittleEndian.Uint32(p[4:8]),
			Value:  math.Float64frombits(binary.LittleEndian.Uint64(p[8:16])),
		})
		p = p[liveSampleSize:]
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

// VideoChunk flags beyond the keyframe and config bits, which are VideoFlagKeyframe
// (bit 0) and VideoFlagConfig (bit 1) as the store has them. Replay marks the video
// a video_subscribe replay or video_seek asked for.
const (
	v2VideoFirst    = 1 << 2   // the first piece of a frame
	v2VideoLast     = 1 << 3   // the last piece of a frame
	v2VideoReplay   = 1 << 4   // replayed video, not live
	v2VideoPieceMax = 16 << 10 // payload bytes in one VideoChunk, at most
	v2VideoFixed    = 16       // the VideoChunk payload before its bytes: t_ms to reserved
)

// videoChunk is one piece of a frame as it goes on the wire.
type videoChunk struct {
	TMS     uint64 // Unix ms of the frame
	Frame   uint32 // the store's sequence number of the frame; 0 for a config NAL sent from the store's latest copy
	Flags   uint8
	Stream  uint8  // camera index
	Payload []byte // this piece of the frame's Annex-B bytes
}

// encodeVideoChunk returns the VideoChunk message for ch. It has seq 0: the writer
// stamps it when it sends the message.
func encodeVideoChunk(ch videoChunk) []byte {
	size := v2HeaderSize + v2VideoFixed + len(ch.Payload)
	b := make([]byte, 0, size)
	b = appendV2Header(b, v2MsgVideoChunk, size-v2HeaderSize)
	b = binary.LittleEndian.AppendUint64(b, ch.TMS)
	b = binary.LittleEndian.AppendUint32(b, ch.Frame)
	b = append(b, ch.Flags, ch.Stream, 0, 0) // flags, stream, reserved
	return append(b, ch.Payload...)
}

// decodeVideoChunk reads a VideoChunk message back into its piece. The payload is a copy.
func decodeVideoChunk(b []byte) (videoChunk, error) {
	h, err := decodeV2Header(b)
	if err != nil || h.Type != v2MsgVideoChunk || h.Len < v2VideoFixed {
		return videoChunk{}, errV2Message
	}
	p := b[v2HeaderSize:]
	return videoChunk{
		TMS:     binary.LittleEndian.Uint64(p[0:8]),
		Frame:   binary.LittleEndian.Uint32(p[8:12]),
		Flags:   p[12],
		Stream:  p[13],
		Payload: append([]byte(nil), p[v2VideoFixed:]...),
	}, nil
}

// videoPieces cuts one frame into VideoChunk messages of at most v2VideoPieceMax
// payload bytes each. flags holds the frame's own bits; the first and last bits are
// set here, so a frame that fits is one piece with both. Each message has seq 0.
func videoPieces(tms uint64, frame uint32, flags uint8, payload []byte) [][]byte {
	n := max(1, (len(payload)+v2VideoPieceMax-1)/v2VideoPieceMax)
	pieces := make([][]byte, 0, n)
	for i := range n {
		lo := min(i*v2VideoPieceMax, len(payload))
		hi := min(lo+v2VideoPieceMax, len(payload))
		f := flags
		if i == 0 {
			f |= v2VideoFirst
		}
		if i == n-1 {
			f |= v2VideoLast
		}
		pieces = append(pieces, encodeVideoChunk(videoChunk{TMS: tms, Frame: frame, Flags: f, Payload: payload[lo:hi]}))
	}
	return pieces
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
