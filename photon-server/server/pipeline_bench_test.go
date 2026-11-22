package main

import (
	"encoding/binary"
	"os"
	"server/cache"
	"strings"
	"testing"
	"time"
)

// Benchmarks for the decoder stage: pool buffer -> frames -> rows in buffer A.
// Run: go test ./server -run '^$' -bench Pipeline -benchmem

const benchClipDir = "../../examples/brainerd-30s/"

// benchStream re-encodes the Brainerd clip as clean CANP v3 batches of 64
// packets, the wire format the car sends, and returns it with its DBC.
func benchStream(b *testing.B) ([]byte, *cache.SessionCache, int) {
	b.Helper()
	raw, err := os.ReadFile(benchClipDir + "2026-07-23-03-30s.log")
	if err != nil {
		b.Skip(err)
	}
	c, err := cache.LoadDBC(benchClipDir + "HighNoon.dbc")
	if err != nil {
		b.Fatal(err)
	}
	var d ingressDecoder
	frames := d.Decode(nil, raw)

	var out []byte
	for i := 0; i < len(frames); i += canpMaxBatch {
		batch := frames[i:min(i+canpMaxBatch, len(frames))]
		out = binary.BigEndian.AppendUint32(out, canpMagic)
		out = binary.BigEndian.AppendUint16(out, canpVersion)
		out = binary.BigEndian.AppendUint16(out, uint16(len(batch)))
		out = binary.BigEndian.AppendUint32(out, 0)
		out = binary.BigEndian.AppendUint64(out, batch[len(batch)-1].TimeMs)
		for _, fr := range batch {
			out = binary.BigEndian.AppendUint32(out, fr.CANID)
			out = append(out, byte(fr.DLC))
			out = append(out, fr.Data[:]...)
			age := batch[len(batch)-1].TimeMs - fr.TimeMs
			out = binary.BigEndian.AppendUint16(out, uint16(min(age, 0xffff)))
			out = append(out, make([]byte, canpPacketSize-15)...)
		}
	}
	return out, c, len(frames)
}

// chunks cuts the stream into pool-buffer-sized pieces, so batches straddle
// buffer edges the way they do on a live connection.
func chunks(stream []byte) [][]byte {
	var out [][]byte
	for len(stream) > 0 {
		n := min(poolBufferSize, len(stream))
		out = append(out, stream[:n])
		stream = stream[n:]
	}
	return out
}

func BenchmarkPipelineDecode(b *testing.B) {
	stream, _, nFrames := benchStream(b)
	parts := chunks(stream)
	b.SetBytes(int64(len(stream)))
	b.ReportAllocs()
	b.ResetTimer()
	got := 0
	var scratch []cache.Frame
	for range b.N {
		var d ingressDecoder
		got = 0
		for _, p := range parts {
			scratch = d.Decode(scratch[:0], p)
			got += len(scratch)
		}
	}
	b.ReportMetric(float64(b.Elapsed().Nanoseconds())/float64(b.N*nFrames), "ns/frame")
	if got != nFrames {
		b.Fatalf("decoded %d frames, want %d", got, nFrames)
	}
}

// bufSink copies rows into one reused byte buffer, as dbWriter fills A.
type bufSink struct {
	body []byte
	rows int
}

func (s *bufSink) Write(rows []byte, n int) {
	s.body = append(s.body, rows...)
	s.rows += n
	if s.rows >= dbFlushRows {
		s.body, s.rows = s.body[:0], 0
	}
}
func (s *bufSink) Close(time.Duration) {}

func BenchmarkPipelineRows(b *testing.B) {
	stream, c, nFrames := benchStream(b)
	parts := chunks(stream)
	sink := &bufSink{body: make([]byte, 0, dbBufferBytes)}
	b.SetBytes(int64(len(stream)))
	b.ReportAllocs()
	b.ResetTimer()
	var scratch []cache.Frame
	for range b.N {
		var d ingressDecoder
		fw := newFrameWriter(c, "20260723T030000Z", "HighNoon", sink)
		for _, p := range parts {
			scratch = d.Decode(scratch[:0], p)
			fw.Fold(scratch)
		}
	}
	b.ReportMetric(float64(b.Elapsed().Nanoseconds())/float64(b.N*nFrames), "ns/frame")
	if !strings.HasPrefix(string(sink.body), "frame,") && sink.rows > 0 {
		b.Fatalf("unexpected rows: %.80q", sink.body)
	}
}

// BenchmarkPipelineRowsSteady reuses one writer, so the one-time signal_def
// rows are out of the loop: this is the per-frame cost on a running session.
func BenchmarkPipelineRowsSteady(b *testing.B) {
	stream, c, nFrames := benchStream(b)
	parts := chunks(stream)
	sink := &bufSink{body: make([]byte, 0, dbBufferBytes)}
	fw := newFrameWriter(c, "20260723T030000Z", "HighNoon", sink)
	var d ingressDecoder
	var scratch []cache.Frame
	for _, p := range parts { // warm up: session start, buffer growth
		scratch = d.Decode(scratch[:0], p)
		fw.Fold(scratch)
	}
	b.SetBytes(int64(len(stream)))
	b.ReportAllocs()
	b.ResetTimer()
	for range b.N {
		for _, p := range parts {
			scratch = d.Decode(scratch[:0], p)
			fw.Fold(scratch)
		}
	}
	b.ReportMetric(float64(b.Elapsed().Nanoseconds())/float64(b.N*nFrames), "ns/frame")
}
