package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"errors"
	"io"
	"os"
	"time"
)

const (
	canpMagic      = 0x43414E31 // "CAN1"
	canpVersion    = 3
	canpHeaderSize = 20
	canpFrameSize  = 29
)

var canpMagicBytes = binary.BigEndian.AppendUint32(nil, canpMagic)

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

/*canpReader reads an ingress connection straight into pool buffers and submits
each buffer's whole batches to the parser, so every byte is copied once.

	cur: [ whole batches | read, not checked yet | free         ]
	     0             keep    scan            fill       len(cur)

Bytes between keep and scan were skipped. cur[:keep] is submitted when the
buffer is full or latency after its first whole batch, whichever comes first;
the batch still arriving at cur[scan:fill] moves to the front of a fresh
buffer.*/
type canpReader struct {
	r       io.Reader
	pool    *batchPool
	latency time.Duration // longest a whole batch waits in cur before it is submitted
	cur     []byte
	keep    int
	scan    int
	fill    int
	due     time.Time // when cur must be submitted; zero while it holds no whole batch
}

// readDeadliner is a connection the reader can wake from when latency is up
// and no more bytes arrive. Without one, a batch waits for the next read.
type readDeadliner interface {
	SetReadDeadline(time.Time) error
}

func newCanpReader(r io.Reader, pool *batchPool, latency time.Duration) *canpReader {
	return &canpReader{r: r, pool: pool, latency: latency}
}

// run reads until the connection ends or ctx is done, and returns why.
func (c *canpReader) run(ctx context.Context) error {
	buf, err := c.pool.get(ctx)
	if err != nil {
		return err
	}
	c.cur = buf
	defer c.finish()
	for {
		n, err := c.r.Read(c.cur[c.fill:])
		if n > 0 {
			c.fill += n
			ingressStats.reads.Add(1)
			ingressStats.lastReceiveMS.Store(time.Now().UnixMilli())
			c.check()
			if c.keep > 0 && c.due.IsZero() {
				c.setDue(time.Now().Add(c.latency))
			}
		}
		if err != nil && !errors.Is(err, os.ErrDeadlineExceeded) {
			return err
		}
		if err := c.flush(ctx); err != nil {
			return err
		}
	}
}

// setDue arms (or, with zero, disarms) the submit deadline on the connection.
func (c *canpReader) setDue(t time.Time) {
	c.due = t
	if d, ok := c.r.(readDeadliner); ok {
		_ = d.SetReadDeadline(t)
	}
}

// check moves scan over what is complete in cur[scan:fill]: whole batches join
// cur[:keep] and bad bytes are skipped. It stops at a batch still arriving.
func (c *canpReader) check() {
	for c.fill-c.scan >= canpHeaderSize {
		b := c.cur[c.scan:c.fill]
		n, err := batchLength(b)
		if err != nil {
			if i := bytes.Index(b[1:], canpMagicBytes); i >= 0 {
				c.skip(1 + i)
			} else {
				c.skip(len(b) - len(canpMagicBytes) + 1)
			}
			continue
		}
		if len(b) < n {
			return
		}
		if cut := cutShortAt(b[:n]); cut > 0 {
			ingressStats.cutBatches.Add(1)
			c.keepFrames(cut)
			continue
		}
		c.keepFrames(n)
	}
}

// keepFrames moves the first end bytes of the batch at scan onto cur[:keep]:
// its header with the count fixed and its whole frames. The partial frame is skipped.
func (c *canpReader) keepFrames(end int) {
	frames := (end - canpHeaderSize) / canpFrameSize
	if frames > 0 {
		kept := canpHeaderSize + frames*canpFrameSize
		if c.keep != c.scan {
			copy(c.cur[c.keep:], c.cur[c.scan:c.scan+kept])
		}
		binary.BigEndian.PutUint16(c.cur[c.keep+6:], uint16(frames))
		c.keep += kept
		c.scan += kept
		end -= kept
		ingressStats.batches.Add(1)
	}
	c.skip(end)
}

func (c *canpReader) skip(n int) {
	ingressStats.skippedBytes.Add(uint64(n))
	c.scan += n
}

// flush submits the whole batches once the buffer is full or they are due,
// and leaves room for the next read.
func (c *canpReader) flush(ctx context.Context) error {
	full := c.fill == len(c.cur)
	timeUp := c.keep > 0 && !time.Now().Before(c.due)
	if !full && !timeUp {
		return nil // room to read more, and nothing due yet
	}
	if c.keep == 0 && c.scan > 0 { // Full of skipped bytes ahead of a batch still arriving
		c.fill = copy(c.cur, c.cur[c.scan:c.fill])
		c.scan = 0
		return nil
	}
	next, err := c.pool.get(ctx)
	if err != nil {
		return err
	}
	if c.keep == 0 {
		// One batch fills the whole buffer and is still not whole
		// cut off extra and put into a new buffer
		cut := cutShortAt(c.cur[:c.fill])
		if cut == 0 {
			c.split(next)
			return nil
		}
		ingressStats.cutBatches.Add(1)
		c.keepFrames(cut)
		c.check()
	}
	n := copy(next, c.cur[c.scan:c.fill])
	if c.keep > 0 {
		c.submit()
	} else {
		c.pool.put(c.cur)
	}
	c.cur, c.keep, c.scan, c.fill = next, 0, 0, n
	c.setDue(time.Time{})
	return nil
}

// split submits the header and whole frames of a batch bigger than a buffer, and
// starts next with a copy of the header counting the frames still to come.
func (c *canpReader) split(next []byte) {
	frames := (c.fill - canpHeaderSize) / canpFrameSize
	kept := canpHeaderSize + frames*canpFrameSize
	count := binary.BigEndian.Uint16(c.cur[6:8])
	copy(next, c.cur[:canpHeaderSize])
	binary.BigEndian.PutUint16(next[6:8], count-uint16(frames))
	n := copy(next[canpHeaderSize:], c.cur[kept:c.fill])
	binary.BigEndian.PutUint16(c.cur[6:8], uint16(frames))
	c.keep = kept
	ingressStats.batches.Add(1)
	c.submit()
	c.cur, c.keep, c.scan, c.fill = next, 0, 0, canpHeaderSize+n
	c.setDue(time.Time{})
}

// submit hands cur[:keep] to the parser, which puts the buffer back when done.
func (c *canpReader) submit() {
	ingressStats.bytes.Add(uint64(c.keep))
	c.pool.submit(c.cur[:c.keep])
	observeQueueDepth(len(c.pool.ready))
}

// finish ends the connection: a batch it stopped partway through keeps its
// whole frames, and the last buffer is submitted or put back.
func (c *canpReader) finish() {
	c.check()
	for c.fill-c.scan >= canpHeaderSize {
		b := c.cur[c.scan:c.fill]
		end := len(b)
		if cut := cutShortAt(b); cut > 0 {
			end = cut
		}
		ingressStats.cutBatches.Add(1)
		c.keepFrames(end)
		c.check()
	}
	c.skip(c.fill - c.scan)
	if c.keep > 0 {
		c.submit()
	} else {
		c.pool.put(c.cur)
	}
	c.cur = nil
}

//  returns where a new batch header starts inside batch, or 0 if none does
func cutShortAt(batch []byte) int {
	for k := 1; k < len(batch); k++ {
		i := bytes.Index(batch[k:], canpMagicBytes)
		if i < 0 {
			return 0
		}
		k += i
		if looksLikeHeader(batch[k:min(k+canpHeaderSize, len(batch))]) {
			return k
		}
	}
	return 0
}

//  checks whatever part of a header starting with the magic is in h
func looksLikeHeader(h []byte) bool {
	if len(h) >= 6 && binary.BigEndian.Uint16(h[4:6]) != canpVersion {
		return false
	}
	return len(h) < 8 || binary.BigEndian.Uint16(h[6:8]) != 0
}
