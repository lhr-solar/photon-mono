package main

import (
	"context"
	"fmt"
	"math/bits"
	"sync/atomic"
	"time"
)

// poolSizing is what the ingest pool is sized from. Every number in the pool
// comes from these three inputs (handoff "Where the numbers come from"):
//
//	buffer size = Rate × Latency, rounded up to a power of two
//	count       = Stall ÷ Latency
//	memory      ≈ Rate × Stall
type poolSizing struct {
	Rate    int           // peak ingest, bytes/s
	Latency time.Duration // a buffer goes to the decoder at most this long after its first batch
	Stall   time.Duration // how long the decoder can stall before readers stop and TCP pushes back
}

// bufferSize is Rate × Latency rounded up to a power of two, and never less
// than one CANP header and frame.
func (s poolSizing) bufferSize() int {
	n := int(int64(s.Rate) * int64(s.Latency) / int64(time.Second))
	n = max(n, canpHeaderSize+canpFrameSize)
	return 1 << bits.Len(uint(n-1))
}

// count is Stall ÷ Latency, at least two so one buffer can fill while
// another is decoded.
func (s poolSizing) count() int {
	return max(int(s.Stall/s.Latency), 2)
}

func (s poolSizing) String() string {
	size, count := s.bufferSize(), s.count()
	return fmt.Sprintf("%d x %d KiB = %.1f MiB (rate %.2f MiB/s, latency %s, stall %s)",
		count, size>>10, float64(size*count)/(1<<20), float64(s.Rate)/(1<<20), s.Latency, s.Stall)
}

// batchPool is the static ingest pool. Every buffer is cut from one slab
// allocated at startup. Readers take a free buffer, fill it, and submit it;
// the decoder puts it back. When every buffer is in flight, readers wait and
// TCP pushes back on the sender.
type batchPool struct {
	free  chan []byte // empty buffers the reader can take
	ready chan []byte // filled batches waiting for the parser, oldest first
	size  int         // bytes in each buffer

	highWater atomic.Int64 // most buffers ever in flight at once
	longestNS atomic.Int64 // longest a reader has waited for a free buffer
}

func newBatchPool(count, size int) *batchPool {
	if size < canpHeaderSize+canpFrameSize {
		panic("batch pool buffers must hold a CANP header and one frame")
	}
	p := &batchPool{free: make(chan []byte, count), ready: make(chan []byte, count), size: size}
	slab := make([]byte, count*size)
	for i := 0; i < count; i++ {
		p.free <- slab[i*size : (i+1)*size : (i+1)*size]
	}
	return p
}

// get takes a free buffer, waiting while every buffer is in flight.
func (p *batchPool) get(ctx context.Context) ([]byte, error) {
	select {
	case buf := <-p.free:
		p.noteInUse()
		return buf, nil
	default:
	}
	started := time.Now()
	select {
	case buf := <-p.free:
		storeMax(&p.longestNS, time.Since(started).Nanoseconds())
		p.noteInUse()
		return buf, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (p *batchPool) put(buf []byte) {
	p.free <- buf[:cap(buf)]
}

func (p *batchPool) submit(buf []byte) {
	p.ready <- buf
}

// inUse is how many buffers are filling, queued, or being decoded.
func (p *batchPool) inUse() int { return cap(p.free) - len(p.free) }

func (p *batchPool) noteInUse() { storeMax(&p.highWater, int64(p.inUse())) }

func storeMax(a *atomic.Int64, v int64) {
	for old := a.Load(); v > old; old = a.Load() {
		if a.CompareAndSwap(old, v) {
			return
		}
	}
}
