package main

import (
	"bytes"
	"encoding/binary"
	"context"
	"io"
	"os"
	"testing"
	"time"
)

// Ingest benchmark on the recorded Brainerd clip (ported from the cache
// branch's pipeline benchmark): socket reads -> canpReader -> pool -> walk.
// Run: go test ./server -run '^$' -bench Ingest -benchmem

const benchClip = "../../examples/brainerd-30s/2026-07-23-03-30s.log"

// clipBatches returns the clip's CANP batches back to back, as the car sends
// them (the clip is a stored segment, so its VSD1 framing is dropped).
func clipBatches(b *testing.B) (stream []byte, frames int) {
	b.Helper()
	seg, err := os.ReadFile(benchClip)
	if err != nil {
		b.Skip(err)
	}
	// Each stored record is a 24-byte header and the bytes as they arrived;
	// joined, the payloads are the original TCP stream.
	var raw []byte
	for len(seg) >= frameHeaderBytes && binary.BigEndian.Uint32(seg) == frameMagic {
		n := int(binary.BigEndian.Uint32(seg[4:8]))
		if len(seg) < frameHeaderBytes+n {
			break
		}
		raw = append(raw, seg[frameHeaderBytes:frameHeaderBytes+n]...)
		seg = seg[frameHeaderBytes+n:]
	}
	for i := 0; ; {
		k := bytes.Index(raw[i:], canpMagicBytes)
		if k < 0 || len(raw)-i-k < canpHeaderSize {
			break
		}
		i += k
		n, err := batchLength(raw[i:])
		if err != nil || len(raw)-i < n {
			i++
			continue
		}
		// The recording holds batches cut off after 46 frames (1356 bytes)
		// with the next batch starting there. Keep their whole frames under a
		// corrected count, as a clean sender would have sent them.
		if c := cutShortAt(raw[i : i+n]); c > 0 {
			n = canpHeaderSize + (c-canpHeaderSize)/canpFrameSize*canpFrameSize
			hdr := append([]byte(nil), raw[i:i+canpHeaderSize]...)
			binary.BigEndian.PutUint16(hdr[6:8], uint16((n-canpHeaderSize)/canpFrameSize))
			stream = append(stream, hdr...)
			stream = append(stream, raw[i+canpHeaderSize:i+n]...)
			frames += (n - canpHeaderSize) / canpFrameSize
			i += c
			continue
		}
		stream = append(stream, raw[i:i+n]...)
		frames += (n - canpHeaderSize) / canpFrameSize
		i += n
	}
	if frames == 0 {
		b.Skip("no CANP batches in the clip")
	}
	return stream, frames
}

// tcpReader hands out the stream in MSS-sized reads, the way a socket does.
type tcpReader struct {
	data []byte
	mss  int
}

func (r *tcpReader) Read(p []byte) (int, error) {
	if len(r.data) == 0 {
		return 0, io.EOF
	}
	n := copy(p[:min(len(p), r.mss)], r.data)
	r.data = r.data[n:]
	return n, nil
}

func BenchmarkIngestBrainerd(b *testing.B) {
	stream, nFrames := clipBatches(b)
	s := poolSizing{Rate: DefaultIngestRate, Latency: DefaultIngestLatency, Stall: DefaultIngestStall}
	pool := newBatchPool(s.count(), s.bufferSize())

	got := 0
	count := func(uint32, string, []byte, int64) { got++ }
	b.SetBytes(int64(len(stream)))
	b.ReportAllocs()
	b.ResetTimer()
	start := time.Now()
	for i := 0; i < b.N; i++ {
		done := make(chan struct{})
		go func() {
			defer close(done)
			for buf := range pool.ready {
				walkCANP3(buf, count)
				pool.put(buf)
			}
		}()
		r := newCanpReader(&tcpReader{data: stream, mss: 1460}, pool, s.Latency)
		_ = r.run(context.Background())
		close(pool.ready)
		<-done
		pool.ready = make(chan []byte, cap(pool.free))
	}
	b.StopTimer()
	if got != b.N*nFrames {
		b.Fatalf("walked %d frames, want %d", got, b.N*nFrames)
	}
	b.ReportMetric(float64(time.Since(start).Nanoseconds())/float64(b.N*nFrames), "ns/frame")
}
