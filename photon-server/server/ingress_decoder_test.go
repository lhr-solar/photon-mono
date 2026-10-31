package main

import (
	"encoding/binary"
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

	if frames := d.Decode(batch[:30]); len(frames) != 0 {
		t.Fatalf("partial batch decoded %d frames", len(frames))
	}
	frames := d.Decode(batch[30:])
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
	frames := d.Decode(batch)
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
	frames := d.Decode(batch)
	if len(frames) != 1 || frames[0].TimeMs != 100 {
		t.Fatalf("got %+v, want TimeMs 100 (no underflow)", frames)
	}
}

func TestIngressDecoderSkipsGarbage(t *testing.T) {
	var d ingressDecoder
	in := append([]byte{0xde, 0xad, 0xbe}, canpBatch(1, 0x10)...)
	in = append(in, canpBatch(2, 0x20)...)

	frames := d.Decode(in)
	if len(frames) != 2 || frames[0].CANID != 0x10 || frames[1].CANID != 0x20 {
		t.Fatalf("got %+v", frames)
	}
}
