package main

import (
	"encoding/binary"
	"math/rand"
	"strconv"
	"testing"

	"server/dbc"
)

// ---- sinks ---------------------------------------------------------------

// countSink only counts values: cost of extraction alone.
type countSink struct{ rows, values int }

func (s *countSink) Row(_, _ string, _ int64, names []string, _ []float64) {
	s.rows++
	s.values += len(names)
}
func (s *countSink) Raw(string, uint32, []byte, int64, string) {}

// lpSink formats InfluxDB line protocol into a reused buffer, the way a
// writer would before posting: measurement,bus=X a=1,b=2 1700000000000000000\n
type lpSink struct {
	buf    []byte
	values int
	bytes  int
}

func (s *lpSink) Row(meas, bus string, tNS int64, names []string, vals []float64) {
	b := s.buf[:0]
	b = append(b, meas...)
	b = append(b, ",bus="...)
	b = append(b, bus...)
	b = append(b, ' ')
	for i, n := range names {
		if i > 0 {
			b = append(b, ',')
		}
		b = append(b, n...)
		b = append(b, '=')
		b = strconv.AppendFloat(b, vals[i], 'g', -1, 64)
	}
	b = append(b, ' ')
	b = strconv.AppendInt(b, tNS, 10)
	b = append(b, '\n')
	s.bytes += len(b)
	s.values += len(names)
	s.buf = b
}
func (s *lpSink) Raw(string, uint32, []byte, int64, string) {}

// ---- data ----------------------------------------------------------------

type frame struct {
	id   uint32
	data []byte
}

func loadCatalog(b *testing.B) *dbc.Catalog {
	c, err := dbc.Load("../dbc/spec/dbc", DefaultCANConfig().SampleRates)
	if err != nil {
		b.Fatal(err)
	}
	return c
}

// frames returns n frames drawn uniformly from the catalog's messages,
// each with random payload of the message's DBC length.
func frames(c *dbc.Catalog, n int, keep func(*dbc.Message) bool) []frame {
	var ms []*dbc.Message
	for _, m := range c.Messages {
		if keep(m) {
			ms = append(ms, m)
		}
	}
	r := rand.New(rand.NewSource(1))
	out := make([]frame, n)
	for i := range out {
		m := ms[r.Intn(len(ms))]
		d := make([]byte, max(int(m.Length), 1))
		r.Read(d)
		out[i] = frame{m.ID, d}
	}
	return out
}

func all(*dbc.Message) bool       { return true }
func classic(m *dbc.Message) bool { return m.Length <= 8 }
func daq(m *dbc.Message) bool     { return len(m.Groups) > 0 }

// ---- benchmarks: Decoder alone --------------------------------------------

func benchDecode(b *testing.B, keep func(*dbc.Message) bool, lp bool) {
	c := loadCatalog(b)
	fs := frames(c, 4096, keep)
	var sink dbc.Sink
	cs, ls := &countSink{}, &lpSink{buf: make([]byte, 0, 4096)}
	if lp {
		sink = ls
	} else {
		sink = cs
	}
	d := dbc.NewDecoder(c, sink)
	b.ResetTimer()
	t := int64(1_700_000_000_000_000_000)
	for i := 0; i < b.N; i++ {
		f := fs[i&4095]
		t += 100_000
		d.Decode(f.id, "can0", f.data, t)
	}
	b.StopTimer()
	vals := cs.values + ls.values
	el := b.Elapsed().Seconds()
	b.ReportMetric(float64(b.N)/el, "frames/s")
	b.ReportMetric(float64(vals)/el, "values/s")
	b.ReportMetric(float64(vals)/float64(b.N), "values/frame")
	if lp {
		b.ReportMetric(float64(ls.bytes)/el/1e6, "LP_MB/s")
	}
}

func BenchmarkDecodeAll(b *testing.B)          { benchDecode(b, all, false) }
func BenchmarkDecodeAllLineProto(b *testing.B) { benchDecode(b, all, true) }
func BenchmarkDecodeDAq(b *testing.B)          { benchDecode(b, daq, false) }
func BenchmarkDecodeDAqLineProto(b *testing.B) { benchDecode(b, daq, true) }

// ---- benchmark: the real ingest path (walkCANP3 over 64 KiB buffers) -------

// canpBuffer packs classic frames into CANP v3 batches of 64 frames until
// the buffer is full, the way the reader hands them to appendLoop.
func canpBuffer(fs []frame, size int) (buf []byte, nframes int) {
	buf = make([]byte, 0, size)
	k := 0
	for {
		if len(buf)+canpHeaderSize+64*canpFrameSize > size {
			return buf, nframes
		}
		hdr := make([]byte, canpHeaderSize)
		binary.BigEndian.PutUint32(hdr[0:4], canpMagic)
		binary.BigEndian.PutUint16(hdr[4:6], canpVersion)
		binary.BigEndian.PutUint16(hdr[6:8], 64)
		binary.BigEndian.PutUint64(hdr[12:20], 1_700_000_000_000)
		buf = append(buf, hdr...)
		for j := 0; j < 64; j++ {
			f := fs[k%len(fs)]
			k++
			rec := make([]byte, canpFrameSize)
			id := f.id
			if id&canpExtended == 0 {
				id |= canpBus0Bit
			}
			binary.BigEndian.PutUint32(rec[0:4], id)
			rec[4] = byte(len(f.data))
			copy(rec[5:13], f.data)
			buf = append(buf, rec...)
		}
		nframes += 64
	}
}

func BenchmarkIngestPath64KiB(b *testing.B) {
	c := loadCatalog(b)
	fs := frames(c, 4096, classic)
	buf, n := canpBuffer(fs, 64<<10)
	ls := &lpSink{buf: make([]byte, 0, 4096)}
	d := dbc.NewDecoder(c, ls)
	b.SetBytes(int64(len(buf)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		walkCANP3(buf, d.Decode)
	}
	b.StopTimer()
	el := b.Elapsed().Seconds()
	b.ReportMetric(float64(b.N*n)/el, "frames/s")
	b.ReportMetric(float64(ls.values)/el, "values/s")
}
