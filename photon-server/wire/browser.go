package wire

import (
	"encoding/binary"
	"math"
	"server/cache"
)

func BrowserFrame(kind byte, seq uint32, payload []byte) []byte {
	b := make([]byte, 12+len(payload))
	binary.LittleEndian.PutUint16(b, 0x5057)
	b[2] = 1
	b[3] = kind
	binary.LittleEndian.PutUint32(b[4:], seq)
	binary.LittleEndian.PutUint32(b[8:], uint32(len(payload)))
	copy(b[12:], payload)
	return b
}
func CANMonitor(frames []cache.Frame) []byte {
	b := make([]byte, 24*len(frames))
	for i, f := range frames {
		p := b[24*i:]
		binary.LittleEndian.PutUint32(p, f.CANID)
		p[4] = byte(f.DLC)
		p[5] = f.Flags
		binary.LittleEndian.PutUint64(p[8:], f.TimeMs)
		copy(p[16:24], f.Data[:])
	}
	return b
}
func Telemetry(at uint64, values []cache.Value, wanted map[uint32]bool) []byte {
	var b []byte
	for _, v := range values {
		ids := []uint32{v.Def.Number}
		if v.Tap >= 0 {
			ids = append(ids, cache.TapNumber(v.Def, v.Tap))
		}
		for _, id := range ids {
			if !wanted[id] {
				continue
			}
			p := make([]byte, 24)
			binary.LittleEndian.PutUint32(p, id)
			binary.LittleEndian.PutUint16(p[4:], 1)
			binary.LittleEndian.PutUint64(p[8:], math.Float64bits(float64(at)))
			binary.LittleEndian.PutUint64(p[16:], math.Float64bits(v.Physical))
			b = append(b, p...)
		}
	}
	return b
}
func Timeline(mode byte, cursor, min, max, total uint64) []byte {
	b := make([]byte, 33)
	b[0] = mode
	for i, v := range []uint64{cursor, min, max, total} {
		binary.LittleEndian.PutUint64(b[1+i*8:], v)
	}
	return b
}
