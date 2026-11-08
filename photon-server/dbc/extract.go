package dbc

import "math"

// Value is the signal's physical value in data: raw × Factor + Offset.
// ok is false when data is too short to hold the signal.
func (s *Signal) Value(data []byte) (v float64, ok bool) {
	raw, ok := s.Raw(data)
	if !ok {
		return 0, false
	}
	switch s.Kind {
	case Float32:
		v = float64(math.Float32frombits(uint32(raw)))
	case Float64:
		v = math.Float64frombits(raw)
	default:
		if s.Signed {
			v = float64(signExtend(raw, s.Bits))
		} else {
			v = float64(raw)
		}
	}
	return v*s.Factor + s.Offset, true
}

// Raw is the signal's bits as they sit in data, unscaled and not sign-extended.
// The mux selector and Timestamp are read this way.
func (s *Signal) Raw(data []byte) (uint64, bool) {
	if int(s.need) > len(data) {
		return 0, false
	}
	n := int(s.Bits)
	bit := int(s.Start)
	var raw uint64
	if s.LittleEndian {
		// Intel: Start is the least significant bit; the rest follow upward.
		for i := 0; i < n; i++ {
			raw |= uint64(data[bit/8]>>(bit%8)&1) << i
			bit++
		}
		return raw, true
	}
	// Motorola: Start is the most significant bit. Within a byte bits run 7→0,
	// then continue at bit 7 of the next byte, which is 15 bit numbers up.
	for i := 0; i < n; i++ {
		raw = raw<<1 | uint64(data[bit/8]>>(bit%8)&1)
		if bit%8 == 0 {
			bit += 15
		} else {
			bit--
		}
	}
	return raw, true
}

func signExtend(raw uint64, bits uint8) int64 {
	shift := 64 - bits
	return int64(raw<<shift) >> shift
}

// span is how many bytes of a frame the signal reaches into: one more than
// the index of the last byte holding any of its bits.
func (s *Signal) span() int {
	bit := int(s.Start)
	if s.LittleEndian {
		return (bit+int(s.Bits)-1)/8 + 1
	}
	last := bit / 8
	for i := 1; i < int(s.Bits); i++ {
		if bit%8 == 0 {
			bit += 15
		} else {
			bit--
		}
		last = max(last, bit/8)
	}
	return last + 1
}
