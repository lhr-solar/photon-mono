package main

import (
	"encoding/binary"
	"math"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"

	"server/dbc"
)

// plainFrame is a Plain frame of frameTestDBC: Zeta and Alpha in bytes 0 and 1, Mid
// big-endian in bytes 2 and 3, and Speed as a little-endian float32 in bytes 4 to 7.
func plainFrame(zeta uint8, alpha int8, mid uint16, speed float32) []byte {
	d := make([]byte, 8)
	d[0] = zeta
	d[1] = byte(alpha)
	binary.BigEndian.PutUint16(d[2:4], mid)
	binary.LittleEndian.PutUint32(d[4:8], math.Float32bits(speed))
	return d
}

// muxFrame is a Muxed frame on page 1 or 2: Common in byte 0, the page's own signal
// (First on page 1, Second on page 2) in byte 1 or 2, and the page in byte 7.
func muxFrame(page byte, common, value byte) []byte {
	d := make([]byte, 8)
	d[0] = common
	d[7] = page
	if page == 1 {
		d[1] = value
	} else {
		d[2] = value
	}
	return d
}

// rollupTestLayouts is the layout of every message in frameTestDBC, by wire ID.
func rollupTestLayouts(t *testing.T) map[uint32]*frameLayout {
	t.Helper()
	cat, err := dbc.Load(writeFrameTestDBC(t), nil)
	if err != nil {
		t.Fatal(err)
	}
	return buildLayouts(cat)
}

// rollupRows returns the rollup_1s rows of lines, sorted.
func rollupRows(lines []string) []string {
	rows := rowsWithPrefix(lines, "rollup_1s,")
	slices.Sort(rows)
	return rows
}

// TestRollupRowsAreExact writes frames over three seconds of Plain, and two of Muxed
// on two pages, and checks every rollup_1s row: the min, max, sum and count of each
// signal in each second, integers as integers, a NaN left out, and the row written
// at finish.
func TestRollupRowsAreExact(t *testing.T) {
	layouts := rollupTestLayouts(t)
	plain, muxed := layouts[100], layouts[101]
	sink := &recordSink{}
	now := testNow
	s := newTestSession(sink, &now)

	base := int64(testTSMS) * nsPerMs
	at := func(ms int64) int64 { return base + ms*nsPerMs }
	s.appendFrame(plain, plainFrame(10, -5, 0x1000, 1.5), at(0))
	s.appendFrame(plain, plainFrame(20, 0, 0x2000, 0.25), at(500))
	s.appendFrame(plain, plainFrame(11, -4, 0x1001, 2.5), at(1000))
	s.appendFrame(plain, plainFrame(21, 1, 0x2001, float32(math.NaN())), at(1500))
	s.appendFrame(plain, plainFrame(12, -3, 0x1002, 3.5), at(2000))
	s.appendFrame(plain, plainFrame(22, 2, 0x2002, 0.75), at(2500))
	s.appendFrame(muxed, muxFrame(1, 7, 66), at(0))
	s.appendFrame(muxed, muxFrame(2, 7, 67), at(300))
	s.appendFrame(muxed, muxFrame(1, 9, 68), at(1000))
	s.appendFrame(muxed, muxFrame(1, 9, 80), at(1100))

	// The second 0 of Plain is closed by the frame at 2.5 s, which is 2 s on. Nothing
	// else is closed yet.
	s.flush()
	if got := rollupRows(sink.lines(t)); len(got) != 1 || !strings.HasPrefix(got[0], "rollup_1s,session="+testSID+",msg=Plain ") {
		t.Fatalf("before finish: rollup rows %q, want only Plain's first second", got)
	}

	s.Close(time.Second)
	want := []string{
		"rollup_1s,session=" + testSID + ",msg=Plain Alpha_min=-5i,Alpha_max=0i,Alpha_sum=-5,Alpha_n=2i," +
			"Mid_min=4096i,Mid_max=8192i,Mid_sum=12288,Mid_n=2i,Speed_min=0.25,Speed_max=1.5,Speed_sum=1.75,Speed_n=2i," +
			"Zeta_min=10i,Zeta_max=20i,Zeta_sum=30,Zeta_n=2i " + "1700000000000000000",
		"rollup_1s,session=" + testSID + ",msg=Plain Alpha_min=-4i,Alpha_max=1i,Alpha_sum=-3,Alpha_n=2i," +
			"Mid_min=4097i,Mid_max=8193i,Mid_sum=12290,Mid_n=2i,Speed_min=2.5,Speed_max=2.5,Speed_sum=2.5,Speed_n=1i," +
			"Zeta_min=11i,Zeta_max=21i,Zeta_sum=32,Zeta_n=2i " + "1700000001000000000",
		"rollup_1s,session=" + testSID + ",msg=Plain Alpha_min=-3i,Alpha_max=2i,Alpha_sum=-1,Alpha_n=2i," +
			"Mid_min=4098i,Mid_max=8194i,Mid_sum=12292,Mid_n=2i,Speed_min=0.75,Speed_max=3.5,Speed_sum=4.25,Speed_n=2i," +
			"Zeta_min=12i,Zeta_max=22i,Zeta_sum=34,Zeta_n=2i " + "1700000002000000000",
		"rollup_1s,session=" + testSID + ",msg=Muxed,tap=1 Common_min=7i,Common_max=7i,Common_sum=7,Common_n=1i," +
			"First_min=66i,First_max=66i,First_sum=66,First_n=1i " + "1700000000000000000",
		"rollup_1s,session=" + testSID + ",msg=Muxed,tap=1 Common_min=9i,Common_max=9i,Common_sum=18,Common_n=2i," +
			"First_min=68i,First_max=80i,First_sum=148,First_n=2i " + "1700000001000000000",
		"rollup_1s,session=" + testSID + ",msg=Muxed,tap=2 Common_min=7i,Common_max=7i,Common_sum=7,Common_n=1i," +
			"Second_min=67i,Second_max=67i,Second_sum=67,Second_n=1i " + "1700000000000000000",
	}
	slices.Sort(want)
	if got := rollupRows(sink.lines(t)); !slices.Equal(got, want) {
		t.Fatalf("rollup rows\n%s\nwant\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}
	if s.rollupLate != 0 {
		t.Fatalf("%d late frames, want none", s.rollupLate)
	}
	// Each frame row is still there: six Plain and four Muxed.
	if n := len(rowsWithPrefix(sink.lines(t), "frame,")); n != 10 {
		t.Fatalf("%d frame rows, want 10", n)
	}
}

// A frame for a second whose bucket is already written is late: it is counted, it is
// not in the rollup, and its frame row is written.
func TestRollupLateFrameIsCountedNotRolledUp(t *testing.T) {
	layouts := rollupTestLayouts(t)
	sink := &recordSink{}
	now := testNow
	s := newTestSession(sink, &now)
	base := int64(testTSMS) * nsPerMs
	for k := range 4 {
		s.appendFrame(layouts[100], plainFrame(uint8(10+k), 0, 0, 0), base+int64(k)*1000*nsPerMs)
	}
	// 100 ms into the first second, when the newest frame is 3 s on: that second is written.
	s.appendFrame(layouts[100], plainFrame(99, 0, 0, 0), base+100*nsPerMs)
	if s.rollupLate != 1 {
		t.Fatalf("rollupLate = %d, want 1", s.rollupLate)
	}
	s.Close(time.Second)

	var firstSecond []string
	for _, r := range rollupRows(sink.lines(t)) {
		if strings.HasSuffix(r, " "+strconv.FormatInt(base, 10)) {
			firstSecond = append(firstSecond, r)
		}
	}
	if len(firstSecond) != 1 || !strings.Contains(firstSecond[0], "Zeta_max=10i,Zeta_sum=10,Zeta_n=1i") {
		t.Fatalf("first second's rollup rows %q, want the single frame with Zeta 10", firstSecond)
	}
	if n := len(rowsWithPrefix(sink.lines(t), "frame,")); n != 5 {
		t.Fatalf("%d frame rows, want 5 (the late frame too)", n)
	}
}

// A message's buckets are closed by its own frames, 2 s on, so a message that stops
// sending keeps its last second open while other messages go on. The silence closes
// it, or finish does.
func TestRollupStoppedMessageWaitsForSilenceOrFinish(t *testing.T) {
	layouts := rollupTestLayouts(t)
	sink := &recordSink{}
	now := testNow
	s := newTestSession(sink, &now)
	base := int64(testTSMS) * nsPerMs
	s.appendFrame(layouts[100], plainFrame(10, 0, 0, 0), base)
	for k := int64(1); k <= 2; k++ {
		s.appendFrame(layouts[101], muxFrame(1, 7, 66), base+k*1000*nsPerMs)
	}
	s.flush()
	if rows := rollupRows(sink.lines(t)); len(rows) != 0 {
		t.Fatalf("rollup rows %q, want none: Plain's second is not 2 s behind its own frames", rows)
	}
	now = now.Add(rollupIdle)
	s.closeIdleRollups()
	rows := rollupRows(sink.lines(t))
	if len(rows) != 3 || !strings.Contains(strings.Join(rows, "\n"), "msg=Plain Alpha_min=0i") {
		t.Fatalf("rollup rows after the silence %q, want Plain's second and both Muxed seconds", rows)
	}
	s.Close(time.Second)
}

// A session whose wall clock has been silent for rollupIdle closes its open buckets,
// so the last seconds of a run that stopped sending reach rollup_1s.
func TestRollupIdleSilenceClosesBuckets(t *testing.T) {
	layouts := rollupTestLayouts(t)
	sink := &recordSink{}
	now := testNow
	s := newTestSession(sink, &now)
	base := int64(testTSMS) * nsPerMs
	s.appendFrame(layouts[100], plainFrame(10, 0, 0, 0), base)
	s.flush()
	if rows := rollupRows(sink.lines(t)); len(rows) != 0 {
		t.Fatalf("rollup rows %q before any silence", rows)
	}

	s.closeIdleRollups() // not yet silent
	if rows := rollupRows(sink.lines(t)); len(rows) != 0 {
		t.Fatalf("rollup rows %q after a flush with no silence", rows)
	}
	now = now.Add(rollupIdle)
	s.closeIdleRollups()
	rows := rollupRows(sink.lines(t))
	if len(rows) != 1 || !strings.HasSuffix(rows[0], " "+strconv.FormatInt(base, 10)) {
		t.Fatalf("rollup rows after the silence %q, want the first second", rows)
	}
	s.Close(time.Second)
}

// Once the buffers have room, a frame costs no allocation, rollup rows included: a
// second's buckets are reused, not made again.
func TestRollupAppendDoesNotAllocate(t *testing.T) {
	layouts := rollupTestLayouts(t)
	s := newFrameSessionID(testSID, dryRunSink{})
	s.buf = make([]byte, 0, 1<<20)
	base := int64(testTSMS) * nsPerMs
	i := int64(0)
	step := func() {
		tNS := base + i*nsPerMs // one frame a millisecond, so a second is closed every 1000 frames
		if i%2 == 0 {
			s.appendFrame(layouts[100], plainFrame(10, -5, 0x1000, 1.5), tNS)
		} else {
			page := byte(1 + (i/2)%2) // pages 1 and 2 in turn
			s.appendFrame(layouts[101], muxFrame(page, 7, 66), tNS)
		}
		i++
		if i%100 == 0 {
			s.flush()
		}
	}
	for range 5000 { // warm up over five seconds, so every bucket and series exists
		step()
	}
	if allocs := testing.AllocsPerRun(5000, step); allocs != 0 {
		t.Fatalf("a frame allocates %v times in steady state, want 0", allocs)
	}
	if s.rollupLate != 0 {
		t.Fatalf("%d late frames in the allocation run", s.rollupLate)
	}
}
