package main

import "context"

type batchPool struct {
	free  chan []byte // empty buffers the reader can take
	ready chan []byte // filled batches waiting for the parser, oldest first
}

func newBatchPool(count, size int) *batchPool {
	f := make(chan []byte, count)
	r := make(chan []byte, count)
	p := &batchPool{free: f, ready: r}

	for i := 0; i < count; i++ {
		p.free <- make([]byte, size)
	}
	return p
}

func (p *batchPool) get(ctx context.Context) ([]byte, error) {

	select {
	case buf := <-p.free:
		return buf, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (p *batchPool) put(buf []byte) {
	buf = buf[:cap(buf)]
	p.free <- buf
}

func (p *batchPool) submit(buf []byte) {
	p.ready <- buf
}

func (p *batchPool) next(ctx context.Context) ([]byte, error) {
	select {
	case buf := <-p.ready:
		return buf, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}
