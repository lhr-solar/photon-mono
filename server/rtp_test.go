package main

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func TestRTPSingleNAL(t *testing.T) {
	raw := []byte{0x65, 0xAA, 0xBB, 0xCC} // IDR
	pkt := buildRTP(1, 90000, true, raw)

	d := &RTPDepacketizer{}
	nals := d.Push(pkt)
	if len(nals) != 1 {
		t.Fatalf("expected 1 NAL, got %d", len(nals))
	}
	want := append(append([]byte{}, annexBStartCode...), raw...)
	if !bytes.Equal(nals[0].Payload, want) {
		t.Fatalf("payload mismatch:\n got %x\nwant %x", nals[0].Payload, want)
	}
	if !nals[0].Keyframe {
		t.Fatal("expected IDR to be marked keyframe")
	}
	if nals[0].NALType != nalTypeIDR {
		t.Fatalf("expected NAL type %d, got %d", nalTypeIDR, nals[0].NALType)
	}
	if nals[0].RTPTimestamp != 90000 {
		t.Fatalf("expected RTP timestamp 90000, got %d", nals[0].RTPTimestamp)
	}
}

func TestRTPSingleNALSPS(t *testing.T) {
	raw := []byte{0x67, 0x42, 0x00, 0x0a}
	d := &RTPDepacketizer{}
	nals := d.Push(buildRTP(1, 1, false, raw))
	if len(nals) != 1 {
		t.Fatalf("expected 1 NAL, got %d", len(nals))
	}
	if !nals[0].Config {
		t.Fatal("expected SPS to be marked config")
	}
	if nals[0].Keyframe {
		t.Fatal("SPS must not be marked keyframe")
	}
}

func TestRTPFUAReassembly(t *testing.T) {
	nalType := byte(nalTypeIDR)
	nri := byte(0x60)
	body := bytes.Repeat([]byte{0x11, 0x22, 0x33, 0x44}, 20) // 80 bytes
	chunks := [][]byte{body[:30], body[30:60], body[60:]}

	d := &RTPDepacketizer{}
	var got []NALUnit
	for i, chunk := range chunks {
		start := i == 0
		end := i == len(chunks)-1
		pkt := buildFUA(uint16(i+1), 42, end, start, end, nri, nalType, chunk)
		got = append(got, d.Push(pkt)...)
	}
	if len(got) != 1 {
		t.Fatalf("expected 1 reassembled NAL, got %d", len(got))
	}
	wantRaw := append([]byte{nri | nalType}, body...)
	want := append(append([]byte{}, annexBStartCode...), wantRaw...)
	if !bytes.Equal(got[0].Payload, want) {
		t.Fatalf("FU-A payload mismatch:\n got %x\nwant %x", got[0].Payload, want)
	}
	if !got[0].Keyframe {
		t.Fatal("reassembled IDR should be a keyframe")
	}
}

func TestRTPSequenceGapDropsIncompleteFUA(t *testing.T) {
	nri := byte(0x60)
	d := &RTPDepacketizer{}

	// Start an IDR fragment at seq 1, never send the rest.
	start := buildFUA(1, 100, false, true, false, nri, nalTypeIDR, []byte{0x01, 0x02})
	if nals := d.Push(start); len(nals) != 0 {
		t.Fatalf("expected no NAL from FU-A start, got %d", len(nals))
	}

	// Gap to seq 5 with a complete non-IDR slice — incomplete FU must be discarded.
	slice := []byte{0x01, 0xDE, 0xAD}
	complete := buildRTP(5, 200, true, slice)
	nals := d.Push(complete)
	if len(nals) != 1 {
		t.Fatalf("expected 1 NAL after gap, got %d", len(nals))
	}
	want := append(append([]byte{}, annexBStartCode...), slice...)
	if !bytes.Equal(nals[0].Payload, want) {
		t.Fatalf("got %x, want %x", nals[0].Payload, want)
	}
	if nals[0].Keyframe {
		t.Fatal("gap recovery must not emit the discarded IDR fragment")
	}
}

func TestRTPOutOfOrderDropped(t *testing.T) {
	d := &RTPDepacketizer{}
	first := []byte{0x01, 0xA1}
	second := []byte{0x01, 0xB2}

	nals := d.Push(buildRTP(10, 1, true, first))
	if len(nals) != 1 {
		t.Fatalf("expected first packet to emit, got %d", len(nals))
	}

	// Late packet (seq 9) must be dropped.
	if late := d.Push(buildRTP(9, 1, true, second)); len(late) != 0 {
		t.Fatalf("expected out-of-order packet to be dropped, got %d NALs", len(late))
	}

	// In-order next (seq 11) is accepted.
	nals = d.Push(buildRTP(11, 2, true, second))
	if len(nals) != 1 {
		t.Fatalf("expected in-order packet after drop, got %d", len(nals))
	}
	want := append(append([]byte{}, annexBStartCode...), second...)
	if !bytes.Equal(nals[0].Payload, want) {
		t.Fatalf("got %x, want %x", nals[0].Payload, want)
	}
}

func TestRTPIgnoresWrongPayloadType(t *testing.T) {
	d := &RTPDepacketizer{}
	pkt := buildRTP(1, 1, true, []byte{0x01, 0x00})
	pkt[1] = (pkt[1] & 0x80) | 97
	if nals := d.Push(pkt); len(nals) != 0 {
		t.Fatalf("expected PT!=96 to be ignored, got %d", len(nals))
	}
}

func TestRTPSTAPA(t *testing.T) {
	sps := []byte{0x67, 0x42}
	pps := []byte{0x68, 0xCE}
	var payload []byte
	payload = append(payload, nalTypeSTAPA)
	payload = appendU16(payload, uint16(len(sps)))
	payload = append(payload, sps...)
	payload = appendU16(payload, uint16(len(pps)))
	payload = append(payload, pps...)

	d := &RTPDepacketizer{}
	nals := d.Push(buildRTP(1, 7, false, payload))
	if len(nals) != 2 {
		t.Fatalf("expected 2 STAP-A NALs, got %d", len(nals))
	}
	if !nals[0].Config || !nals[1].Config {
		t.Fatal("expected both STAP-A NALs to be config")
	}
}

func buildRTP(seq uint16, ts uint32, marker bool, payload []byte) []byte {
	pkt := make([]byte, 12+len(payload))
	pkt[0] = rtpVersion << 6
	pkt[1] = rtpPayloadTypeH264
	if marker {
		pkt[1] |= 0x80
	}
	binary.BigEndian.PutUint16(pkt[2:4], seq)
	binary.BigEndian.PutUint32(pkt[4:8], ts)
	binary.BigEndian.PutUint32(pkt[8:12], 0x11223344)
	copy(pkt[12:], payload)
	return pkt
}

func buildFUA(seq uint16, ts uint32, marker, start, end bool, nri, nalType byte, frag []byte) []byte {
	fu := make([]byte, 2+len(frag))
	fu[0] = nri | nalTypeFUA
	fu[1] = nalType
	if start {
		fu[1] |= 0x80
	}
	if end {
		fu[1] |= 0x40
	}
	copy(fu[2:], frag)
	return buildRTP(seq, ts, marker, fu)
}

func appendU16(b []byte, v uint16) []byte {
	return append(b, byte(v>>8), byte(v))
}
