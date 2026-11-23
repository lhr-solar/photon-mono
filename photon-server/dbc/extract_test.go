package dbc

import "testing"

func TestRawIntSignExtends(t *testing.T) {
	// Intel 8-bit at bit 0: 0xFB is -5 when signed, 251 when not.
	s := Signal{Start: 0, Bits: 8, LittleEndian: true, Signed: true, need: 1}
	if v, ok := s.RawInt([]byte{0xFB}); !ok || v != -5 {
		t.Fatalf("signed Intel: got %d, %v; want -5, true", v, ok)
	}
	s.Signed = false
	if v, ok := s.RawInt([]byte{0xFB}); !ok || v != 251 {
		t.Fatalf("unsigned Intel: got %d, %v; want 251, true", v, ok)
	}

	// Motorola 16-bit starting at bit 7 (bytes 0 and 1): 0xFFFE is -2 when signed.
	m := Signal{Start: 7, Bits: 16, Signed: true, need: 2}
	if v, ok := m.RawInt([]byte{0xFF, 0xFE}); !ok || v != -2 {
		t.Fatalf("signed Motorola: got %d, %v; want -2, true", v, ok)
	}
	if _, ok := m.RawInt([]byte{0xFF}); ok {
		t.Fatal("short data: ok = true, want false")
	}
}
