package dbc

import (
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"strconv"
	"testing"
)

// cantoolsFrames is written by testdata/gen_cantools.py. Tests run in the
// package folder, photon-server/dbc.
const cantoolsFrames = "../../tests/go/dbc/testdata/cantools_frames.json"

type cantoolsCase struct {
	Bus     string            `json:"bus"`
	Message string            `json:"message"`
	ID      uint32            `json:"id"`
	Data    string            `json:"data"`
	Values  map[string]string `json:"values"`
}

// Every signal in every message must decode to cantools' value, bit for bit.
func TestValueMatchesCantools(t *testing.T) {
	c, err := Load("spec/dbc", testRates)
	if err != nil {
		t.Fatal(err)
	}
	text, err := os.ReadFile(cantoolsFrames)
	if err != nil {
		t.Fatal(err)
	}
	var cases []cantoolsCase
	if err := json.Unmarshal(text, &cases); err != nil {
		t.Fatal(err)
	}
	if len(cases) < 1000 {
		t.Fatalf("only %d reference frames", len(cases))
	}

	checked, failures := 0, 0
	for _, tc := range cases {
		m := c.Lookup(tc.ID)
		if m == nil || m.Name != tc.Message {
			t.Fatalf("%s 0x%X: catalog has %v", tc.Message, tc.ID, m)
		}
		data, _ := hex.DecodeString(tc.Data)
		for name, text := range tc.Values {
			want, err := strconv.ParseFloat(text, 64)
			if err != nil {
				t.Fatalf("%s.%s: bad reference %q", tc.Message, name, text)
			}
			s := signal(m, name)
			if s == nil {
				t.Fatalf("%s: cantools decoded %s, which the catalog does not have", tc.Message, name)
			}
			got, ok := s.Value(data)
			checked++
			if !ok || !same(got, want) {
				failures++
				if failures <= 10 {
					t.Errorf("%s.%s data=%s: got %v (ok=%v), want %v", tc.Message, name, tc.Data, got, ok, want)
				}
			}
		}
	}
	if failures > 0 {
		t.Errorf("%d of %d values differ", failures, checked)
	}
	t.Logf("%d values in %d frames match cantools", checked, len(cases))
}

// same treats NaN as equal to NaN; random bytes make NaN float32 values.
func same(a, b float64) bool {
	return a == b || (math.IsNaN(a) && math.IsNaN(b))
}

func signal(m *Message, name string) *Signal {
	for i := range m.Signals {
		if m.Signals[i].Name == name {
			return &m.Signals[i]
		}
	}
	return nil
}

// A frame too short for a signal reports it instead of reading past the end.
func TestValueShortFrame(t *testing.T) {
	c, err := Load("spec/dbc", testRates)
	if err != nil {
		t.Fatal(err)
	}
	m := c.Lookup(0x611) // GPSData, 16 bytes; GPSAltitude sits in bytes 12–14
	s := signal(m, "GPSAltitude")
	if s == nil {
		t.Fatal("GPSData has no GPSAltitude")
	}
	if _, ok := s.Value(make([]byte, 8)); ok {
		t.Errorf("GPSAltitude decoded from 8 bytes")
	}
	if _, ok := s.Value(make([]byte, 16)); !ok {
		t.Errorf("GPSAltitude did not decode from 16 bytes")
	}
}

// Hand-checked layouts, so a mistake shared with cantools would still show.
func TestValueKnownLayouts(t *testing.T) {
	intel := Signal{Start: 4, Bits: 12, LittleEndian: true, Factor: 1}
	intel.need = uint8(intel.span())
	// bits 4..15: low nibble of 0x34's high half (3), then 0x12 → 0x123
	if v, _ := intel.Value([]byte{0x30, 0x12}); v != 0x123 {
		t.Errorf("intel 12-bit = %v, want %v", v, 0x123)
	}

	moto := Signal{Start: 7, Bits: 16, Factor: 1} // Motorola, MSB at bit 7: bytes 0..1 big-endian
	moto.need = uint8(moto.span())
	if v, _ := moto.Value([]byte{0x12, 0x34}); v != 0x1234 {
		t.Errorf("motorola 16-bit = %v, want %v", v, 0x1234)
	}

	signed := Signal{Start: 0, Bits: 8, LittleEndian: true, Signed: true, Factor: 0.5, Offset: -1}
	signed.need = uint8(signed.span())
	if v, _ := signed.Value([]byte{0xFE}); v != -2 { // -2 × 0.5 − 1
		t.Errorf("signed = %v, want -2", v)
	}

	f32 := Signal{Start: 0, Bits: 32, LittleEndian: true, Kind: Float32, Factor: 1}
	f32.need = uint8(f32.span())
	b := make([]byte, 4)
	bits := math.Float32bits(3.5)
	b[0], b[1], b[2], b[3] = byte(bits), byte(bits>>8), byte(bits>>16), byte(bits>>24)
	if v, _ := f32.Value(b); v != 3.5 {
		t.Errorf("float32 = %v, want 3.5", v)
	}
}
