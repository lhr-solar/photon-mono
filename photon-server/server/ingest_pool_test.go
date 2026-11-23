package main

import (
	"context"
	"encoding/binary"
	"net"
	"testing"
	"time"
)

// testBatch is one CANP v3 batch of count frames stamped tsMS.
func testBatch(count int, tsMS uint64) []byte {
	b := binary.BigEndian.AppendUint32(nil, canpMagic)
	b = binary.BigEndian.AppendUint16(b, canpVersion)
	b = binary.BigEndian.AppendUint16(b, uint16(count))
	b = binary.BigEndian.AppendUint32(b, 0)
	b = binary.BigEndian.AppendUint64(b, tsMS)
	for i := 0; i < count; i++ {
		rec := make([]byte, canpFrameSize)
		binary.BigEndian.PutUint32(rec, 0x100+uint32(i))
		rec[4] = 8
		b = append(b, rec...)
	}
	return b
}

func TestPoolSizingFromInputs(t *testing.T) {
	s := poolSizing{Rate: DefaultIngestRate, Latency: DefaultIngestLatency, Stall: DefaultIngestStall}
	// 1.16 MB/s x 10 ms = 11.6 KB -> 16 KiB; 10 s / 10 ms = 1000 buffers.
	if got := s.bufferSize(); got != 16<<10 {
		t.Fatalf("bufferSize = %d, want 16 KiB", got)
	}
	if got := s.count(); got != 1000 {
		t.Fatalf("count = %d, want 1000", got)
	}
	tiny := poolSizing{Rate: 1, Latency: time.Millisecond, Stall: time.Millisecond}
	if got := tiny.bufferSize(); got < canpHeaderSize+canpFrameSize {
		t.Fatalf("bufferSize = %d, smaller than one header and frame", got)
	}
	if got := tiny.count(); got != 2 {
		t.Fatalf("count = %d, want the minimum of 2", got)
	}
}

func TestPoolHighWaterAndWait(t *testing.T) {
	p := newBatchPool(2, 64)
	ctx := context.Background()
	a, _ := p.get(ctx)
	b, _ := p.get(ctx)
	if p.inUse() != 2 || p.highWater.Load() != 2 {
		t.Fatalf("inUse=%d highWater=%d, want 2 and 2", p.inUse(), p.highWater.Load())
	}
	go func() {
		time.Sleep(20 * time.Millisecond)
		p.put(a)
	}()
	if _, err := p.get(ctx); err != nil { // waits for a
		t.Fatal(err)
	}
	if p.longestNS.Load() < int64(10*time.Millisecond) {
		t.Fatalf("longest wait %s, want about 20 ms", time.Duration(p.longestNS.Load()))
	}
	p.put(b)
}

// runReader starts a canpReader on one end of a pipe and returns the other.
func runReader(t *testing.T, pool *batchPool, latency time.Duration) net.Conn {
	t.Helper()
	srv, cli := net.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		defer close(done)
		_ = newCanpReader(srv, pool, latency).run(ctx)
	}()
	t.Cleanup(func() {
		cancel()
		_ = cli.Close()
		_ = srv.Close()
		<-done
	})
	return cli
}

func waitReady(pool *batchPool, within time.Duration) ([]byte, bool) {
	select {
	case b := <-pool.ready:
		return b, true
	case <-time.After(within):
		return nil, false
	}
}

// A lone batch waits for the latency timer, then goes out even though no
// more bytes arrive.
func TestCanpReaderSubmitsAfterLatency(t *testing.T) {
	pool := newBatchPool(4, 4096)
	cli := runReader(t, pool, 50*time.Millisecond)
	batch := testBatch(2, 1_700_000_000_000)
	if _, err := cli.Write(batch); err != nil {
		t.Fatal(err)
	}
	if _, ok := waitReady(pool, 20*time.Millisecond); ok {
		t.Fatal("batch submitted before the latency was up")
	}
	got, ok := waitReady(pool, time.Second)
	if !ok {
		t.Fatal("batch not submitted after the latency")
	}
	if len(got) != len(batch) {
		t.Fatalf("submitted %d bytes, want %d", len(got), len(batch))
	}
}

// A full buffer goes out at once, whatever the timer says.
func TestCanpReaderSubmitsWhenFull(t *testing.T) {
	batch := testBatch(1, 1_700_000_000_000)
	pool := newBatchPool(4, 4*len(batch))
	cli := runReader(t, pool, time.Hour)
	for i := 0; i < 4; i++ {
		if _, err := cli.Write(batch); err != nil {
			t.Fatal(err)
		}
	}
	got, ok := waitReady(pool, time.Second)
	if !ok {
		t.Fatal("full buffer not submitted")
	}
	if len(got) != 4*len(batch) {
		t.Fatalf("submitted %d bytes, want the whole buffer (%d)", len(got), 4*len(batch))
	}
}

func TestCANDecoderDropsBadClock(t *testing.T) {
	cfg := DefaultCANConfig()
	cfg.DBCDir = "../" + cfg.DBCDir
	c, err := newCANDecoder(cfg, nil)
	if err != nil {
		t.Skip(err)
	}
	now := uint64(time.Now().UnixMilli())
	buf := testBatch(3, 1000)                                       // 1970: car clock not set
	buf = append(buf, testBatch(2, now+uint64(time.Hour/1e6))...)   // an hour ahead
	buf = append(buf, testBatch(4, now)...)                         // good
	c.decode(buf)
	if c.badClock != 5 {
		t.Fatalf("badClock = %d, want 5", c.badClock)
	}
	if c.dec.Stats.Frames != 4 {
		t.Fatalf("decoded %d frames, want 4", c.dec.Stats.Frames)
	}
}
