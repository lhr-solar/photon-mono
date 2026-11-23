package main

import (
	"encoding/binary"
	"math"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"server/dbc"
)

// frameTestDBC is a small DBC for the frame row tests. Plain has always-present
// signals and a float, which gets no field. Muxed is multiplexed on Page, which
// is the last byte.
const frameTestDBC = `BO_ 100 Plain: 8 Node
 SG_ Zeta : 0|8@1+ (1,0) [0|255] "" Node
 SG_ Alpha : 8|8@1- (1,0) [-128|127] "V" Node
 SG_ Mid : 23|16@0+ (1,0) [0|65535] "" Node
 SG_ Speed : 32|32@1+ (1,0) [0|0] "rpm" Node

BO_ 101 Muxed: 8 Node
 SG_ Common : 0|8@1+ (1,0) [0|255] "" Node
 SG_ First m1 : 8|8@1+ (1,0) [0|255] "" Node
 SG_ Second m2 : 16|8@1+ (1,0) [0|255] "" Node
 SG_ Page M : 56|8@1+ (1,0) [0|255] "" Node

SIG_VALTYPE_ 100 Speed : 1;
`

const (
	testSID = "20261122T093000Z"
	// testTSMS is a frame time in ms, 2023, after the car clock is valid.
	testTSMS = uint64(1_700_000_000_000)
	// testRowTime is testTSMS in line protocol nanoseconds.
	testRowTime = "1700000000000000000"
)

var (
	testNow = time.Date(2026, 11, 22, 9, 30, 0, 0, time.UTC)
	// plainData decodes on Plain as Zeta=200, Alpha=-5, Mid=4660; Speed is a float.
	plainData = []byte{0xC8, 0xFB, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04}
)

// muxData decodes on Muxed as Common=7 and First or Second=66, on page page.
func muxData(page byte) []byte {
	return []byte{0x07, 0x42, 0x43, 0, 0, 0, 0, page}
}

// testFrame is one CAN frame for frameBatch.
type testFrame struct {
	id   uint32
	data []byte
}

// frameBatch builds one CANP v3 batch holding frames, all stamped tsMS with no age.
func frameBatch(tsMS uint64, frames ...testFrame) []byte {
	b := binary.BigEndian.AppendUint32(nil, canpMagic)
	b = binary.BigEndian.AppendUint16(b, canpVersion)
	b = binary.BigEndian.AppendUint16(b, uint16(len(frames)))
	b = binary.BigEndian.AppendUint32(b, 0)
	b = binary.BigEndian.AppendUint64(b, tsMS)
	for _, f := range frames {
		rec := make([]byte, canpFrameSize)
		binary.BigEndian.PutUint32(rec, f.id)
		rec[4] = byte(len(f.data))
		copy(rec[5:13], f.data)
		b = append(b, rec...)
	}
	return b
}

// writeFrameTestDBC writes frameTestDBC into a new directory, as bus "Test".
func writeFrameTestDBC(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "Test.dbc"), []byte(frameTestDBC), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

// recordSink keeps every write, and whether it was closed.
type recordSink struct {
	mu     sync.Mutex
	bodies []string
	counts []int
	closed bool
}

func (r *recordSink) Write(rows []byte, n int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.bodies = append(r.bodies, string(rows))
	r.counts = append(r.counts, n)
}

func (r *recordSink) Close(time.Duration) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.closed = true
}

// lines returns every row written so far, in order. It checks that each write
// holds as many rows as its count says.
func (r *recordSink) lines(t *testing.T) []string {
	t.Helper()
	r.mu.Lock()
	defer r.mu.Unlock()
	var out []string
	for i, body := range r.bodies {
		if got := strings.Count(body, "\n"); got != r.counts[i] {
			t.Fatalf("write %d holds %d rows, its count says %d", i, got, r.counts[i])
		}
		out = append(out, strings.Split(strings.TrimSuffix(body, "\n"), "\n")...)
	}
	return out
}

// rowsWithPrefix keeps the lines that start with prefix.
func rowsWithPrefix(lines []string, prefix string) []string {
	var out []string
	for _, l := range lines {
		if strings.HasPrefix(l, prefix) {
			out = append(out, l)
		}
	}
	return out
}

// newTestSession is a session for testSID. Its clock is *now, which the test moves.
func newTestSession(sink lineSink, now *time.Time) *frameSession {
	s := newFrameSession(testNow, sink)
	s.now = func() time.Time { return *now }
	return s
}

// newTestDecoder is a decoder on the synthetic DBC in dir, writing frame rows to s.
func newTestDecoder(t *testing.T, dir string, s *frameSession) *canDecoder {
	t.Helper()
	c, err := newCANDecoder(CANConfig{Decode: true, DBCDir: dir}, s)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

// integerNames returns the names of m's integer signals that are on page
// (-1 for none), sorted, leaving out the selector.
func integerNames(m *dbc.Message, page int16) []string {
	var names []string
	for i := range m.Signals {
		sig := &m.Signals[i]
		if i == m.Selector || sig.Kind != dbc.Integer {
			continue
		}
		if sig.Mux >= 0 && sig.Mux != page {
			continue
		}
		names = append(names, sig.Name)
	}
	slices.Sort(names)
	return names
}

// fieldNames returns the field names of a frame row's field section, in order.
func fieldNames(fields string) []string {
	var names []string
	for _, f := range strings.Split(fields, ",") {
		name, _, _ := strings.Cut(f, "=")
		names = append(names, name)
	}
	return names
}

// TestFrameRowCases decodes one frame at a time through the decoder and checks
// the frame row it writes.
func TestFrameRowCases(t *testing.T) {
	dir := writeFrameTestDBC(t)
	tests := []struct {
		name  string
		frame testFrame
		want  string // the row after the session tag; "" means no frame row
	}{
		{
			name:  "every signal, signed one negative, float32 as a float",
			frame: testFrame{id: 100, data: plainData},
			want:  ",msg=Plain Alpha=-5i,Mid=4660i,Speed=1.5399896e-36,Zeta=200i",
		},
		{
			name:  "a NaN float is left out",
			frame: testFrame{id: 100, data: []byte{0xC8, 0xFB, 0x12, 0x34, 0x00, 0x00, 0xC0, 0x7F}},
			want:  ",msg=Plain Alpha=-5i,Mid=4660i,Zeta=200i",
		},
		{
			name:  "short frame keeps the signals it can read",
			frame: testFrame{id: 100, data: plainData[:2]},
			want:  ",msg=Plain Alpha=-5i,Zeta=200i",
		},
		{
			name:  "mux page 1 writes its own signal",
			frame: testFrame{id: 101, data: muxData(1)},
			want:  ",msg=Muxed,tap=1 Common=7i,First=66i",
		},
		{
			name:  "mux page 2 writes its own signal",
			frame: testFrame{id: 101, data: muxData(2)},
			want:  ",msg=Muxed,tap=2 Common=7i,Second=67i",
		},
		{
			name:  "page with no signals keeps the always-present ones",
			frame: testFrame{id: 101, data: muxData(9)},
			want:  ",msg=Muxed,tap=9 Common=7i",
		},
		{
			name:  "selector out of reach: no tap, mux signals left out",
			frame: testFrame{id: 101, data: []byte{0x07}},
			want:  ",msg=Muxed Common=7i",
		},
		{
			name:  "unknown ID writes no row",
			frame: testFrame{id: 2047, data: plainData},
			want:  "",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			sink := &recordSink{}
			now := testNow
			s := newTestSession(sink, &now)
			c := newTestDecoder(t, dir, s)
			c.decode(frameBatch(testTSMS, tc.frame))

			rows := rowsWithPrefix(sink.lines(t), "frame,")
			if tc.want == "" {
				if len(rows) != 0 {
					t.Fatalf("got frame rows %q, want none", rows)
				}
				return
			}
			want := "frame,session=" + testSID + tc.want + " " + testRowTime
			if len(rows) != 1 || rows[0] != want {
				t.Fatalf("frame rows %q, want [%q]", rows, want)
			}
		})
	}
}

// TestSignalDefsAndSessionRows checks the order of the rows a decoder writes,
// that signal_def rows come once per decoder, and when the session row goes out.
func TestSignalDefsAndSessionRows(t *testing.T) {
	dir := writeFrameTestDBC(t)
	sink := &recordSink{}
	now := testNow
	s := newTestSession(sink, &now)
	c1 := newTestDecoder(t, dir, s)
	const t0 = testTSMS

	c1.decode(frameBatch(t0, testFrame{id: 100, data: plainData}))       // first frame: defs, then session
	c1.decode(frameBatch(t0+1000, testFrame{id: 101, data: muxData(1)})) // same second: no session row
	now = now.Add(time.Second)
	c1.decode(frameBatch(t0+2000, testFrame{id: 100, data: plainData})) // a second later: session row
	c1.decode(frameBatch(t0+3000, testFrame{id: 100, data: plainData})) // same second again

	// Second decoder on the same session: its own signal_def rows.
	c2 := newTestDecoder(t, dir, s)
	c2.decode(frameBatch(t0+4000, testFrame{id: 100, data: plainData}))
	s.Close(time.Second)

	if !sink.closed {
		t.Fatal("sink not closed")
	}
	lines := sink.lines(t)

	// Eight signals (Speed is a float32 and has a row too), so eight rows per decoder.
	defs := rowsWithPrefix(lines, "signal_def,")
	if len(defs) != 16 {
		t.Fatalf("%d signal_def rows, want 16 (8 for each of two decoders)", len(defs))
	}
	if i := slices.IndexFunc(defs, func(d string) bool { return strings.Contains(d, "signal=Plain.Speed ") }); i < 0 ||
		!strings.Contains(defs[i], `kind="f32"`) {
		t.Fatalf("Plain.Speed signal_def missing or not kind f32: %q", defs)
	}
	// Each decoder stamps its signal_def rows with its own first frame's time.
	for i, d := range defs {
		first := t0
		if i >= 8 {
			first = t0 + 4000 // c2's first frame
		}
		if want := fmt.Sprintf(" %d", int64(first)*nsPerMs); !strings.HasSuffix(d, want) {
			t.Fatalf("signal_def %q not stamped %s", d, want)
		}
	}
	// The first decoder's eight signal_def rows come straight before its first frame row.
	firstFrame := slices.IndexFunc(lines, func(l string) bool { return strings.HasPrefix(l, "frame,") })
	if firstFrame != 8 {
		t.Fatalf("first frame row at line %d, want 8 (after the eight signal_def rows)", firstFrame)
	}
	for _, l := range lines[:firstFrame] {
		if !strings.HasPrefix(l, "signal_def,") {
			t.Fatalf("line before the first frame row is %q, want signal_def", l)
		}
	}

	sessionRow := func(end uint64) string {
		return fmt.Sprintf(`session,id=%s dbc_label="Test",dbc_sha="%s",start=%di,end=%di %d`,
			testSID, c1.catalog.SHA, t0, end, int64(t0)*nsPerMs)
	}
	// One session row at the first flush, one a second later, one at Close with
	// the final end. The rewrite at +3000 is not due, so it is not written.
	want := []string{sessionRow(t0), sessionRow(t0 + 2000), sessionRow(t0 + 4000)}
	if got := rowsWithPrefix(lines, "session,"); !slices.Equal(got, want) {
		t.Fatalf("session rows:\n%q\nwant:\n%q", got, want)
	}
}

// TestFrameRowsRealSpec writes the frame rows of two messages from the real
// spec: a plain one, and the Elcon mux message on two pages.
func TestFrameRowsRealSpec(t *testing.T) {
	c, err := dbc.Load("../dbc/spec/dbc", DefaultCANConfig().SampleRates)
	if err != nil {
		t.Fatal(err)
	}
	layouts := buildLayouts(c)

	var plain *dbc.Message
	for _, m := range c.Messages {
		// CANP carries at most 8 data bytes, so only messages that fit in 8 are whole on the wire.
		if m.Selector < 0 && m.Length <= 8 && len(integerNames(m, -1)) >= 2 {
			plain = m
			break
		}
	}
	elcon := c.Lookup(2550589172) // Elcon_Control_Mux
	if plain == nil || elcon == nil || elcon.Selector < 0 {
		t.Fatal("spec lacks the messages this test needs")
	}

	// The Elcon selector, Elcon_MuxPage, is bits 63..56: the last byte.
	elconData := func(page byte) []byte {
		d := make([]byte, 8)
		d[7] = page
		return d
	}
	// All ones: signed signals read as -1, so the sign extension shows.
	ones := make([]byte, 8)
	for i := range ones {
		ones[i] = 0xFF
	}
	tests := []struct {
		m    *dbc.Message
		page int16 // -1 when the message has no tap
		data []byte
	}{
		{plain, -1, ones},
		{elcon, 2, elconData(2)},
		{elcon, 4, elconData(4)},
	}
	now := testNow
	s := newTestSession(&recordSink{}, &now)
	tNS := int64(testTSMS) * nsPerMs
	for _, tc := range tests {
		s.buf, s.n = s.buf[:0], 0
		s.appendFrame(layouts[tc.m.ID], tc.data, tNS)
		row := strings.TrimSuffix(string(s.buf), "\n")
		parts := strings.Split(row, " ")
		if len(parts) != 3 {
			t.Fatalf("%s: row %q has %d space-separated parts, want 3", tc.m.Name, row, len(parts))
		}
		if want := "frame,session=" + testSID + ",msg=" + tc.m.Name; !strings.HasPrefix(parts[0], want) {
			t.Fatalf("%s: tags %q, want prefix %q", tc.m.Name, parts[0], want)
		}
		wantTap := ""
		if tc.page >= 0 {
			wantTap = fmt.Sprintf(",tap=%d", tc.page)
		}
		if got := strings.TrimPrefix(parts[0], "frame,session="+testSID+",msg="+tc.m.Name); got != wantTap {
			t.Fatalf("%s: tags after msg %q, want %q", tc.m.Name, got, wantTap)
		}
		if parts[2] != testRowTime {
			t.Fatalf("%s: timestamp %q, want %s", tc.m.Name, parts[2], testRowTime)
		}
		if got, want := fieldNames(parts[1]), integerNames(tc.m, tc.page); !slices.Equal(got, want) {
			t.Fatalf("%s page %d: fields %v, want %v", tc.m.Name, tc.page, got, want)
		}
		// Each field holds its signal's raw integer.
		for _, f := range strings.Split(parts[1], ",") {
			name, val, _ := strings.Cut(f, "=")
			i := slices.IndexFunc(tc.m.Signals, func(s dbc.Signal) bool { return s.Name == name })
			raw, ok := tc.m.Signals[i].RawInt(tc.data)
			if !ok || val != fmt.Sprintf("%di", raw) {
				t.Fatalf("%s: field %q, want %s=%di", tc.m.Name, f, name, raw)
			}
		}
	}
}

// TestAppendFrameDoesNotAllocate checks that a row costs no allocation once the
// buffer has room, with and without a mux selector.
func TestAppendFrameDoesNotAllocate(t *testing.T) {
	cat, err := dbc.Load(writeFrameTestDBC(t), nil)
	if err != nil {
		t.Fatal(err)
	}
	layouts := buildLayouts(cat)
	now := testNow
	s := newTestSession(&recordSink{}, &now)
	s.buf = make([]byte, 0, 4096)
	tNS := int64(testTSMS) * nsPerMs
	for _, f := range []testFrame{{id: 100, data: plainData}, {id: 101, data: muxData(1)}} {
		l := layouts[f.id]
		allocs := testing.AllocsPerRun(100, func() {
			s.buf, s.n = s.buf[:0], 0
			s.appendFrame(l, f.data, tNS)
		})
		if allocs != 0 {
			t.Fatalf("appendFrame allocates %v times per row (ID %d), want 0", allocs, f.id)
		}
	}
}

// TestFrameRowsSameMillisecond checks that two frames of one series in the same
// millisecond get distinct times (InfluxDB keeps one point per series and time),
// that different mux pages are separate series, and that an older time is kept.
func TestFrameRowsSameMillisecond(t *testing.T) {
	sink := &recordSink{}
	now := testNow
	s := newTestSession(sink, &now)
	c := newTestDecoder(t, writeFrameTestDBC(t), s)

	c.decode(frameBatch(testTSMS,
		testFrame{id: 100, data: plainData},
		testFrame{id: 100, data: plainData},
		testFrame{id: 101, data: muxData(1)},
		testFrame{id: 101, data: muxData(2)},
	))
	c.decode(frameBatch(testTSMS-2, testFrame{id: 100, data: plainData})) // 2 ms back: a real older time

	t0 := int64(testTSMS) * nsPerMs
	want := []struct {
		prefix string
		ns     int64
	}{
		{",msg=Plain ", t0},
		{",msg=Plain ", t0 + 1},
		{",msg=Muxed,tap=1 ", t0},
		{",msg=Muxed,tap=2 ", t0},
		{",msg=Plain ", t0 - 2*nsPerMs},
	}
	rows := rowsWithPrefix(sink.lines(t), "frame,")
	if len(rows) != len(want) {
		t.Fatalf("got %d frame rows, want %d:\n%s", len(rows), len(want), strings.Join(rows, "\n"))
	}
	for i, w := range want {
		if !strings.Contains(rows[i], w.prefix) {
			t.Fatalf("row %d = %q, want it to contain %q", i, rows[i], w.prefix)
		}
		if got := rows[i][strings.LastIndexByte(rows[i], ' ')+1:]; got != strconv.FormatInt(w.ns, 10) {
			t.Fatalf("row %d time = %s, want %d", i, got, w.ns)
		}
	}
}

// TestNilSessionCloseIsNoop checks that a server with InfluxDB off can call Close.
func TestNilSessionCloseIsNoop(t *testing.T) {
	var s *frameSession
	s.Close(time.Second)
}

// The motor controller's IEEE float signals reach the frame row as floats.
func TestFrameRowsMotorControllerFloats(t *testing.T) {
	cat, err := dbc.Load("../dbc/spec/dbc", DefaultCANConfig().SampleRates)
	if err != nil {
		t.Skip(err)
	}
	var m *dbc.Message
	for _, msg := range cat.Messages {
		if msg.Name == "MC_VelocityMeasurement" {
			m = msg
		}
	}
	if m == nil {
		t.Skip("MC_VelocityMeasurement not in the spec")
	}
	// Both signals are float32: put 12.5 in bytes 0-3 and 34.25 in bytes 4-7, little-endian.
	data := binary.LittleEndian.AppendUint32(nil, math.Float32bits(12.5))
	data = binary.LittleEndian.AppendUint32(data, math.Float32bits(34.25))
	sink := &recordSink{}
	now := testNow
	s := newTestSession(sink, &now)
	s.appendFrame(buildLayouts(cat)[m.ID], data, int64(testTSMS)*nsPerMs)
	s.flush()
	rows := rowsWithPrefix(sink.lines(t), "frame,")
	if len(rows) != 1 {
		t.Fatalf("got %d frame rows, want 1", len(rows))
	}
	for _, sig := range m.Signals {
		v := 12.5
		if sig.Start >= 32 {
			v = 34.25
		}
		if want := sig.Name + "=" + strconv.FormatFloat(v, 'g', -1, 32); !strings.Contains(rows[0], want) {
			t.Fatalf("row %q lacks %q", rows[0], want)
		}
	}
}
