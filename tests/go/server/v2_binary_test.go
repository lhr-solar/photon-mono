package main

import (
	"encoding/binary"
	"math"
	"reflect"
	"slices"
	"testing"
)

// The LiveSamples round trip keeps every field: times, signal indexes and values,
// including negative and fractional ones. The header and the first record sit at
// the offsets the protocol gives.
func TestV2LiveSamplesRoundTrip(t *testing.T) {
	const base = int64(1_700_000_000_000_000_000)
	recs := []liveSample{
		{TNS: base + 2_000_000, Signal: 0, Value: 12.34},
		{TNS: base + 1_000_000_000, Signal: 17, Value: -0.5},
		{TNS: base, Signal: 3, Value: 0},
	}
	b := encodeLiveSamples(base, recs)

	if b[0] != 'P' || b[1] != '2' || b[2] != 2 || b[3] != v2MsgLiveSamples {
		t.Fatalf("header bytes %x, want \"P2\", version 2, type 0x20", b[:4])
	}
	h, err := decodeV2Header(b)
	if err != nil {
		t.Fatal(err)
	}
	if h.Seq != 0 || h.Len != uint32(12+liveSampleSize*len(recs)) {
		t.Fatalf("header %+v, want seq 0 and 12 + 16 bytes per record", h)
	}
	if got := int64(binary.LittleEndian.Uint64(b[12:20])); got != base {
		t.Fatalf("base_ns = %d, want %d", got, base)
	}
	if count := binary.LittleEndian.Uint16(b[20:22]); count != 3 {
		t.Fatalf("count = %d, want 3", count)
	}
	// The first record: dt_us 2000, signal 0, then its value as a float64.
	if dt := binary.LittleEndian.Uint32(b[24:28]); dt != 2000 {
		t.Fatalf("first dt_us = %d, want 2000", dt)
	}
	if v := math.Float64frombits(binary.LittleEndian.Uint64(b[32:40])); v != 12.34 {
		t.Fatalf("first value = %v, want 12.34", v)
	}

	back, err := decodeLiveSamples(b)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(back, recs) {
		t.Fatalf("decoded %+v\nwant    %+v", back, recs)
	}
}

// A message holds at most v2LiveSamplesMax records. It also ends before a record
// earlier than its first, and before one more than 2^32 µs after it: dt_us cannot
// say either.
func TestV2LiveSamplesSplits(t *testing.T) {
	const base = int64(1_700_000_000_000_000_000)
	sizes := func(recs []liveSample) []int {
		var out []int
		for _, m := range liveSampleMessages(recs) {
			got, err := decodeLiveSamples(m)
			if err != nil {
				t.Fatal(err)
			}
			out = append(out, len(got))
		}
		return out
	}

	many := make([]liveSample, v2LiveSamplesMax+5)
	for i := range many {
		many[i] = liveSample{TNS: base + int64(i)*1000, Signal: 1, Value: float64(i)}
	}
	if got := sizes(many); !slices.Equal(got, []int{v2LiveSamplesMax, 5}) {
		t.Fatalf("%d records split into %v, want 4096 and 5", len(many), got)
	}
	if n := len(encodeLiveSamples(base, many[:v2LiveSamplesMax])); n != v2HeaderSize+12+16*v2LiveSamplesMax {
		t.Fatalf("a full message is %d bytes, want 65560", n)
	}

	// The last record is exactly 2^32 µs after the first, which fits; the next one
	// is past it.
	edge := []liveSample{{TNS: base}, {TNS: base + int64(math.MaxUint32)*1000}, {TNS: base + int64(math.MaxUint32+1)*1000}}
	if got := sizes(edge); !slices.Equal(got, []int{2, 1}) {
		t.Fatalf("records at the 2^32 µs limit split into %v, want 2 and 1", got)
	}

	// A record before the first starts a message of its own, based on its time.
	back := []liveSample{{TNS: base + 5_000_000}, {TNS: base + 3_000_000}}
	if got := sizes(back); !slices.Equal(got, []int{1, 1}) {
		t.Fatalf("records out of order split into %v, want 1 and 1", got)
	}
	second, err := decodeLiveSamples(liveSampleMessages(back)[1])
	if err != nil {
		t.Fatal(err)
	}
	if second[0].TNS != base+3_000_000 {
		t.Fatalf("second message at %d, want %d", second[0].TNS, base+3_000_000)
	}

	if msgs := liveSampleMessages(nil); len(msgs) != 0 {
		t.Fatalf("no records made %d messages", len(msgs))
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
	samples := encodeLiveSamples(1, []liveSample{{TNS: 1, Signal: 1, Value: 2}})
	if _, err := decodeLiveSamples(samples[:len(samples)-1]); err == nil {
		t.Fatal("a LiveSamples message missing its last byte was accepted")
	}
	if _, err := decodeLiveSamples(gap); err == nil {
		t.Fatal("a Gap message was read as LiveSamples")
	}
}
