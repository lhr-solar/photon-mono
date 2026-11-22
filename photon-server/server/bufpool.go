package main

import (
	"context"
	"errors"
	"net"
	"os"
	"time"
)

const (
	// poolBuffers × poolBufferSize is allocated once at startup (16 MiB):
	// about 10 s of ingest at 1 MiB/s if anything downstream stalls.
	poolBuffers    = 1024
	poolBufferSize = 16 * 1024
	// poolFillWait: a buffer goes to the decoder when it is full or this long
	// after its first byte, whichever comes first.
	poolFillWait = 10 * time.Millisecond
)

// ingressBuf is one pool buffer. data is a fixed slice of the pool's slab.
type ingressBuf struct {
	data []byte
	n    int
}

func (b *ingressBuf) Bytes() []byte { return b.data[:b.n] }

// bufPool is the static ingest pool. Readers take a free buffer, fill it from
// the socket, and hand it to the decoder, which puts it back when done. When
// every buffer is in flight, readers wait, and TCP pushes back on the sender.
type bufPool struct {
	free chan *ingressBuf
}

func newBufPool(count, size int) *bufPool {
	slab := make([]byte, count*size)
	p := &bufPool{free: make(chan *ingressBuf, count)}
	for i := 0; i < count; i++ {
		p.free <- &ingressBuf{data: slab[i*size : (i+1)*size : (i+1)*size]}
	}
	return p
}

// Get waits for a free buffer. It returns nil once ctx is done.
func (p *bufPool) Get(ctx context.Context) *ingressBuf {
	select {
	case b := <-p.free:
		return b
	case <-ctx.Done():
		return nil
	}
}

// Put returns a buffer to the pool.
func (p *bufPool) Put(b *ingressBuf) {
	b.n = 0
	p.free <- b
}

// InUse is how many buffers are filling or waiting to be decoded.
func (p *bufPool) InUse() int { return cap(p.free) - len(p.free) }

// fillBuf reads conn into b until b is full or poolFillWait has passed since
// the first byte. It returns the read error that ended the connection, if any;
// bytes read before that error are still in b.
func fillBuf(conn net.Conn, b *ingressBuf) error {
	_ = conn.SetReadDeadline(time.Time{}) // wait as long as it takes for the first byte
	for b.n < len(b.data) {
		n, err := conn.Read(b.data[b.n:])
		if n > 0 && b.n == 0 {
			_ = conn.SetReadDeadline(time.Now().Add(poolFillWait))
		}
		b.n += n
		if err != nil {
			if errors.Is(err, os.ErrDeadlineExceeded) {
				return nil
			}
			return err
		}
	}
	return nil
}
