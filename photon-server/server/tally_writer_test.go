package main

import (
	"server/cache"
	"strings"
	"testing"
	"time"
)

const tallyTestDBC = `BO_ 256 Speed: 8 Car
 SG_ Kph : 0|16@1+ (0.1,0) [0|6553.5] "km/h" Vector__XXX
`

// tallyT0 is a Unix second in the valid range; tests run the server clock
// just after it.
const tallyT0 = 1_784_825_000

// memSink records what a tallyWriter sends.
type memSink struct{ lines []string }

func (m *memSink) Send(lines []string) { m.lines = append(m.lines, lines...) }
func (m *memSink) Close(time.Duration) {}

func (m *memSink) samples() int {
	n := 0
	for _, l := range m.lines {
		if strings.HasPrefix(l, "sample,") {
			n++
		}
	}
	return n
}

func newTestTally(t *testing.T) (*tallyWriter, *cache.SessionCache, *memSink) {
	t.Helper()
	c, err := cache.ParseDBC(tallyTestDBC)
	if err != nil {
		t.Fatal(err)
	}
	c.DBCSHA = "abc"
	sink := &memSink{}
	tw := newTallyWriter(c, "s1", tallyT0, sink)
	tw.now = func() time.Time { return time.Unix(tallyT0+20, 0) }
	return tw, c, sink
}

// batchAt is one Speed frame at Unix second sec (+ms).
func batchAt(sec int64, ms uint64) []byte {
	return canpBatch(uint64(sec)*1000+ms, 0x100)
}

func TestTallyWriterDrainsClosedSeconds(t *testing.T) {
	tw, c, sink := newTestTally(t)

	var d ingressDecoder
	// Kph raw = 0x0201 = 513 in every packet canpBatch builds.
	tw.Fold(d.Decode(canpBatch(tallyT0*1000, 0x100, 0x100)))
	tw.Fold(d.Decode(batchAt(tallyT0+10, 0)))
	tw.FlushClosed()
	if c.Len() != 1 {
		t.Fatalf("open buckets = %d, want 1 (second T0+10 still open)", c.Len())
	}
	tw.Close(0)

	want := []string{
		`signal_def,dbc=abc,signal=Speed.Kph can_id=256i,start_bit=0i,bit_length=16i,little_endian=true,signed=false,value_type=0i,factor=0.1,offset=0,unit="km/h" 1784825000000000000`,
		`sample,session=s1,signal=Speed.Kph min=513i,max=513i,sum=1026i,count=2i 1784825000000000000`,
		`sample,session=s1,signal=Speed.Kph min=513i,max=513i,sum=513i,count=1i 1784825010000000000`,
	}
	if strings.Join(sink.lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("sent:\n%s\nwant:\n%s", strings.Join(sink.lines, "\n"), strings.Join(want, "\n"))
	}
}

func TestTallyWriterDropsLateFrames(t *testing.T) {
	tw, c, _ := newTestTally(t)

	var d ingressDecoder
	tw.Fold(d.Decode(batchAt(tallyT0, 0)))
	tw.Fold(d.Decode(batchAt(tallyT0+10, 0)))
	tw.FlushClosed()                         // sends second T0
	tw.Fold(d.Decode(batchAt(tallyT0, 500))) // late frame for second T0
	if tw.late != 1 || c.Len() != 1 {
		t.Fatalf("late=%d open=%d, want 1 and 1", tw.late, c.Len())
	}
}

func TestTallyWriterSkipsBadCarClock(t *testing.T) {
	tw, c, _ := newTestTally(t)

	var d ingressDecoder
	tw.Fold(d.Decode(batchAt(5, 0)))                              // before SNTP sync, 1970
	tw.Fold(d.Decode(batchAt(tallyT0+20+tallyMaxFutureSec+1, 0))) // ahead of the server
	if tw.badClock != 2 || c.Len() != 0 || tw.newest != 0 {
		t.Fatalf("badClock=%d open=%d newest=%d, want 2, 0, 0", tw.badClock, c.Len(), tw.newest)
	}
	tw.Fold(d.Decode(batchAt(tallyT0, 0)))
	if c.Len() != 1 {
		t.Fatalf("good frame not folded after bad ones")
	}
}

func TestTallyWriterRecoversFromClockStepBack(t *testing.T) {
	tw, c, sink := newTestTally(t)

	var d ingressDecoder
	tw.Fold(d.Decode(batchAt(tallyT0+15, 0)))
	tw.Fold(d.Decode(batchAt(tallyT0+16, 0)))
	tw.FlushClosed()
	// SNTP steps the car back 100 s; new frames must still be tallied.
	back := int64(tallyT0 + 16 - 100)
	tw.Fold(d.Decode(batchAt(back, 0)))
	tw.Fold(d.Decode(batchAt(back+1, 0)))
	if tw.late != 0 || tw.newest != back+1 {
		t.Fatalf("late=%d newest=%d, want 0 and %d", tw.late, tw.newest, back+1)
	}
	if c.Len() != 2 {
		t.Fatalf("open=%d, want 2 seconds from the new timeline", c.Len())
	}
	tw.Close(0)
	if n := sink.samples(); n != 4 {
		t.Fatalf("%d sample rows sent, want 4:\n%s", n, strings.Join(sink.lines, "\n"))
	}
}
