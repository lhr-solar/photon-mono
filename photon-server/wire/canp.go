package wire

import (
	"encoding/binary"
	"fmt"
	"io"
	"server/cache"
)

const MaxBatchBytes = 20 + 64*29

type Batch struct {
	Sequence uint32
	TimeMS   uint64
	Frames   []cache.Frame
	Raw      []byte
}

// ReadBatch reads exactly one batch. Each TCP connection must own its reader.
func ReadBatch(r io.Reader) (Batch, error) {
	var b Batch
	h := make([]byte, 20)
	if _, err := io.ReadFull(r, h); err != nil {
		return b, err
	}
	if binary.BigEndian.Uint32(h) != 0x43414e31 {
		return b, fmt.Errorf("invalid CANP magic")
	}
	v := binary.BigEndian.Uint16(h[4:])
	size := 29
	if v == 2 {
		size = 21
	} else if v != 3 {
		return b, fmt.Errorf("unsupported CANP version %d", v)
	}
	n := int(binary.BigEndian.Uint16(h[6:]))
	if n < 1 || n > 64 {
		return b, fmt.Errorf("invalid CANP count %d", n)
	}
	b.Sequence = binary.BigEndian.Uint32(h[8:])
	b.TimeMS = binary.BigEndian.Uint64(h[12:])
	b.Raw = make([]byte, 20+n*size)
	copy(b.Raw, h)
	if _, err := io.ReadFull(r, b.Raw[20:]); err != nil {
		return Batch{}, err
	}
	for i := 0; i < n; i++ {
		p := b.Raw[20+i*size:]
		f := cache.Frame{TimeMs: b.TimeMS, CANID: binary.BigEndian.Uint32(p), DLC: int(p[4])}
		if f.DLC > 8 {
			return Batch{}, fmt.Errorf("invalid CAN DLC %d", f.DLC)
		}
		if f.CANID&0x80000000 != 0 {
			f.Flags = 1
		}
		f.CANID &= 0x1fffffff
		copy(f.Data[:], p[5:13])
		for j := 0; j < 8; j++ {
			if v == 3 {
				f.DT[j] = binary.BigEndian.Uint16(p[13+j*2:])
			} else {
				f.DT[j] = uint16(p[13+j])
			}
		}
		b.Frames = append(b.Frames, f)
	}
	return b, nil
}
func EncodeBatch(frames []cache.Frame, seq uint32) []byte {
	if len(frames) == 0 || len(frames) > 64 {
		return nil
	}
	b := make([]byte, 20+29*len(frames))
	binary.BigEndian.PutUint32(b, 0x43414e31)
	binary.BigEndian.PutUint16(b[4:], 3)
	binary.BigEndian.PutUint16(b[6:], uint16(len(frames)))
	binary.BigEndian.PutUint32(b[8:], seq)
	binary.BigEndian.PutUint64(b[12:], frames[0].TimeMs)
	for i, f := range frames {
		p := b[20+29*i:]
		id := f.CANID
		if f.Flags&1 != 0 {
			id |= 0x80000000
		}
		binary.BigEndian.PutUint32(p, id)
		p[4] = byte(f.DLC)
		copy(p[5:13], f.Data[:])
		for j, d := range f.DT {
			binary.BigEndian.PutUint16(p[13+2*j:], d)
		}
	}
	return b
}
