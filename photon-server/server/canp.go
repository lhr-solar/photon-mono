package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
)

const (
	canpMagic      = 0x43414E31 // "CAN1"
	canpVersion    = 3
	canpHeaderSize = 20
	canpFrameSize  = 29
)

func batchLength(hdr []byte) (int, error) {
	magic := binary.BigEndian.Uint32(hdr[0:4])
	ver := binary.BigEndian.Uint16(hdr[4:6])
	count := binary.BigEndian.Uint16(hdr[6:8])

	if magic != canpMagic {
		return 0, fmt.Errorf("bad magic %08x", magic)
	}

	if ver != canpVersion {
		return 0, fmt.Errorf("bad version %d", ver)
	}

	if count == 0 {
		return 0, fmt.Errorf("batch count empty: %d", count)
	}

	return canpHeaderSize + canpFrameSize*int(count), nil
}

// readBatch reads one whole CANP batch from r into a pool buffer.
// hdr is the caller's 20-byte scratch space, so the header is read before a
// buffer is taken and an idle connection holds none. Every failure after get
// puts the buffer back.
func readBatch(ctx context.Context, r io.Reader, hdr []byte, pool *batchPool) ([]byte, error) {
	if _, err := io.ReadFull(r, hdr); err != nil {
		return nil, err
	}
	n, err := batchLength(hdr)
	if err != nil {
		return nil, err
	}
	buf, err := pool.get(ctx)
	if err != nil {
		return nil, err
	}
	if n > len(buf) {
		pool.put(buf)
		return nil, fmt.Errorf("batch of %d bytes exceeds the %d-byte buffer", n, len(buf))
	}
	copy(buf, hdr)
	if _, err := io.ReadFull(r, buf[canpHeaderSize:n]); err != nil {
		pool.put(buf)
		return nil, err
	}
	return buf[:n], nil
}
