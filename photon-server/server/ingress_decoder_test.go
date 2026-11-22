package main

import (
	"encoding/binary"
	"fmt"
	"server/cache"
	"testing"
)

// canpBatch encodes one CANP v3 batch the way photon/core/src/canp.cpp does.
func canpBatch(timestamp uint64, ids ...uint32) []byte {
	out := binary.BigEndian.AppendUint32(nil, 0x43414e31)
	out = binary.BigEndian.AppendUint16(out, 3)
	out = binary.BigEndian.AppendUint16(out, uint16(len(ids)))
	out = binary.BigEndian.AppendUint32(out, 7)
	out = binary.BigEndian.AppendUint64(out, timestamp)
	for _, id := range ids {
		out = binary.BigEndian.AppendUint32(out, id)
		out = append(out, 8, 1, 2, 3, 4, 5, 6, 7, 8)
		out = append(out, make([]byte, 16)...)
	}
	return out
}

func TestIngressDecoderReassemblesSplitBatch(t *testing.T) {
	batch := canpBatch(1234, 0x100, 0x200)
	var d ingressDecoder

	if frames := d.Decode(nil, batch[:30]); len(frames) != 0 {
		t.Fatalf("partial batch decoded %d frames", len(frames))
	}
	frames := d.Decode(nil, batch[30:])
	if len(frames) != 2 {
		t.Fatalf("got %d frames, want 2", len(frames))
	}
	if frames[1].CANID != 0x200 || frames[1].TimeMs != 1234 || frames[1].DLC != 8 || frames[1].Data[7] != 8 {
		t.Fatalf("bad frame: %+v", frames[1])
	}
	if len(d.pending) != 0 {
		t.Fatalf("%d bytes left pending", len(d.pending))
	}
}

func TestIngressDecoderAppliesFrameAge(t *testing.T) {
	batch := canpBatch(10_000, 0x100, 0x200)
	// δt[0] sits 13 bytes into each 29-byte packet.
	binary.BigEndian.PutUint16(batch[canpHeaderSize+13:], 250)
	binary.BigEndian.PutUint16(batch[canpHeaderSize+canpPacketSize+13:], 0)

	var d ingressDecoder
	frames := d.Decode(nil, batch)
	if len(frames) != 2 {
		t.Fatalf("got %d frames, want 2", len(frames))
	}
	if frames[0].TimeMs != 9_750 {
		t.Fatalf("aged frame TimeMs=%d, want 9750", frames[0].TimeMs)
	}
	if frames[1].TimeMs != 10_000 {
		t.Fatalf("zero-age frame TimeMs=%d, want 10000", frames[1].TimeMs)
	}
}

func TestIngressDecoderAgeOlderThanTimestamp(t *testing.T) {
	batch := canpBatch(100, 0x100)
	binary.BigEndian.PutUint16(batch[canpHeaderSize+13:], 500)

	var d ingressDecoder
	frames := d.Decode(nil, batch)
	if len(frames) != 1 || frames[0].TimeMs != 100 {
		t.Fatalf("got %+v, want TimeMs 100 (no underflow)", frames)
	}
}

func TestIngressDecoderSkipsGarbage(t *testing.T) {
	var d ingressDecoder
	in := append([]byte{0xde, 0xad, 0xbe}, canpBatch(1, 0x10)...)
	in = append(in, canpBatch(2, 0x20)...)

	frames := d.Decode(nil, in)
	if len(frames) != 2 || frames[0].CANID != 0x10 || frames[1].CANID != 0x20 {
		t.Fatalf("got %+v", frames)
	}
}

// Cutting the stream anywhere, into pieces of any size, must decode the same
// frames as the whole stream at once. The stream has garbage at the start,
// between batches, and a stray magic number, so every carry path runs.
func TestIngressDecoderAnySplitMatchesWhole(t *testing.T) {
	var stream []byte
	stream = append(stream, 0xde, 0xad)
	stream = append(stream, canpBatch(1000, 0x10, 0x11, 0x12)...)
	stream = append(stream, 0x43, 0x41, 0x4e, 0x31, 0, 9) // magic, bad version
	stream = append(stream, canpBatch(2000, 0x20)...)
	stream = append(stream, canpBatch(3000, 0x30, 0x31)...)
	stream = append(stream, 1, 2, 3)
	stream = append(stream, canpBatch(4000, 0x40)...)

	var whole ingressDecoder
	want := whole.Decode(nil, stream)
	if len(want) != 7 {
		t.Fatalf("whole stream decoded %d frames, want 7", len(want))
	}

	check := func(name string, pieces [][]byte) {
		t.Helper()
		var d ingressDecoder
		var got []cache.Frame
		for _, p := range pieces {
			got = d.Decode(got, p)
		}
		if len(got) != len(want) {
			t.Fatalf("%s: %d frames, want %d", name, len(got), len(want))
		}
		for i := range want {
			if got[i] != want[i] {
				t.Fatalf("%s: frame %d = %+v, want %+v", name, i, got[i], want[i])
			}
		}
	}
	for cut := 0; cut <= len(stream); cut++ {
		check(fmt.Sprintf("cut at %d", cut), [][]byte{stream[:cut], stream[cut:]})
	}
	for size := 1; size <= 64; size++ {
		var pieces [][]byte
		for s := stream; len(s) > 0; s = s[min(size, len(s)):] {
			pieces = append(pieces, s[:min(size, len(s))])
		}
		check(fmt.Sprintf("pieces of %d", size), pieces)
	}
}
