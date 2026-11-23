package main

import (
	"encoding/binary"
	"reflect"
	"testing"
)

// The LiveFrames round trip keeps every field, including extended IDs, empty
// data and the bus byte.
func TestV2LiveFramesRoundTrip(t *testing.T) {
	const base = int64(1_700_000_000_000_000_000)
	frames := []liveFrame{
		{tNS: base + 2_000_000, id: 0x100, n: 8, data: [8]byte{1, 2, 3, 4, 5, 6, 7, 8}},
		{tNS: base + 1_000_000_000, id: canpExtended | 0x18FEF100, n: 3, data: [8]byte{9, 8, 7}},
		{tNS: base, id: 0x7FF, n: 0},
	}
	buses := map[uint32]uint8{0x100: 2, canpExtended | 0x18FEF100: 0}
	b := encodeLiveFrames(frames, func(id uint32) uint8 {
		if bus, ok := buses[id]; ok {
			return bus
		}
		return v2BusUnknown
	})

	if b[0] != 'P' || b[1] != '2' || b[2] != 2 || b[3] != v2MsgLiveFrames {
		t.Fatalf("header bytes %x, want \"P2\", version 2, type 0x20", b[:4])
	}
	h, err := decodeV2Header(b)
	if err != nil {
		t.Fatal(err)
	}
	if h.Seq != 0 || h.Len != uint32(len(b)-v2HeaderSize) {
		t.Fatalf("header %+v, want seq 0 and len %d", h, len(b)-v2HeaderSize)
	}
	if count := binary.LittleEndian.Uint16(b[20:22]); count != 3 {
		t.Fatalf("count = %d, want 3", count)
	}

	recs, err := decodeLiveFrames(b)
	if err != nil {
		t.Fatal(err)
	}
	want := []liveRecord{
		{TNS: base + 2_000_000, ID: 0x100, Bus: 2, Data: []byte{1, 2, 3, 4, 5, 6, 7, 8}},
		{TNS: base + 1_000_000_000, ID: canpExtended | 0x18FEF100, Bus: 0, Data: []byte{9, 8, 7}},
		{TNS: base, ID: 0x7FF, Bus: v2BusUnknown, Data: []byte{}},
	}
	if !reflect.DeepEqual(recs, want) {
		t.Fatalf("decoded %+v\nwant    %+v", recs, want)
	}
}

// base_ns is the earliest time in the message, because car times can go
// backwards in arrival order, and every dt_us is after it.
func TestV2LiveFramesBaseIsMinimum(t *testing.T) {
	const base = int64(1_700_000_000_000_000_000)
	frames := []liveFrame{
		{tNS: base + 5_000_000, id: 0x100, n: 1},
		{tNS: base + 3_000_000, id: 0x101, n: 1},
		{tNS: base + 4_000_000, id: 0x102, n: 1},
	}
	b := encodeLiveFrames(frames, func(uint32) uint8 { return 0 })
	if got := int64(binary.LittleEndian.Uint64(b[12:20])); got != base+3_000_000 {
		t.Fatalf("base_ns = %d, want the earliest frame %d", got, base+3_000_000)
	}
	recs, err := decodeLiveFrames(b)
	if err != nil {
		t.Fatal(err)
	}
	for i := range frames {
		if recs[i].TNS != frames[i].tNS {
			t.Fatalf("frame %d time %d, want %d", i, recs[i].TNS, frames[i].tNS)
		}
	}
}

func TestV2LiveSpanFrames(t *testing.T) {
	const base = int64(1_700_000_000_000_000_000)
	near := []liveFrame{{tNS: base}, {tNS: base + 4_000_000_000_000}} // 4000 s: fits in 2^32 µs
	if got := liveSpanFrames(near); got != 2 {
		t.Fatalf("liveSpanFrames(4000 s apart) = %d, want 2", got)
	}
	far := []liveFrame{{tNS: base}, {tNS: base + 1_000_000_000}, {tNS: base + 5_000_000_000_000}} // 5000 s
	if got := liveSpanFrames(far); got != 2 {
		t.Fatalf("liveSpanFrames(5000 s apart) = %d, want the first two frames", got)
	}
}

func TestV2GapRoundTrip(t *testing.T) {
	b := encodeGap(1_700_000_000_000_000_000, 1_700_000_000_500_000_000)
	h, err := decodeV2Header(b)
	if err != nil {
		t.Fatal(err)
	}
	if h.Type != v2MsgGap || h.Len != 16 || len(b) != 28 {
		t.Fatalf("gap header %+v, length %d; want type 0x21, 16 payload bytes", h, len(b))
	}
	from, to, err := decodeGap(b)
	if err != nil {
		t.Fatal(err)
	}
	if from != 1_700_000_000_000_000_000 || to != 1_700_000_000_500_000_000 {
		t.Fatalf("gap from %d to %d", from, to)
	}
}

// A message that is cut short, or has the wrong magic or type, is refused.
func TestV2BinaryRejectsMalformed(t *testing.T) {
	gap := encodeGap(1, 2)
	if _, err := decodeV2Header(gap[:20]); err == nil {
		t.Fatal("a truncated header was accepted")
	}
	bad := append([]byte(nil), gap...)
	bad[0] = 0
	if _, err := decodeV2Header(bad); err == nil {
		t.Fatal("a bad magic was accepted")
	}
	if _, _, err := decodeGap(gap[:27]); err == nil {
		t.Fatal("a truncated gap was accepted")
	}
	live := encodeLiveFrames([]liveFrame{{tNS: 1, id: 1, n: 2}}, func(uint32) uint8 { return 0 })
	if _, err := decodeLiveFrames(live[:len(live)-1]); err == nil {
		t.Fatal("a LiveFrames message missing its last byte was accepted")
	}
	if _, err := decodeLiveFrames(gap); err == nil {
		t.Fatal("a Gap message was read as LiveFrames")
	}
}
