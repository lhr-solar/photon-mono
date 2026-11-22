package main

import (
	"context"
	"fmt"
	"net"
	"server/cache"
	"strings"
	"testing"
	"time"
)

const frameTestDBC = `BO_ 256 Speed: 8 Car
 SG_ Kph : 0|16@1+ (0.1,0) [0|6553.5] "km/h" Vector__XXX
`

// frameT0 is a Unix second in the valid range; tests run the server clock
// just after it.
const frameT0 = 1_784_825_000

// memSink records what a frameWriter sends.
type memSink struct{ lines []string }

// Write splits the block back into rows and checks the count matches.
func (m *memSink) Write(rows []byte, n int) {
	if n == 0 {
		if len(rows) != 0 {
			panic("rows with n == 0")
		}
		return
	}
	lines := strings.Split(strings.TrimSuffix(string(rows), "\n"), "\n")
	if len(lines) != n || !strings.HasSuffix(string(rows), "\n") {
		panic(fmt.Sprintf("Write n=%d for %d rows: %q", n, len(lines), rows))
	}
	m.lines = append(m.lines, lines...)
}
func (m *memSink) Close(time.Duration) {}

func (m *memSink) frames() int {
	n := 0
	for _, l := range m.lines {
		if strings.HasPrefix(l, "frame,") {
			n++
		}
	}
	return n
}

func newTestFrameWriter(t *testing.T) (*frameWriter, *memSink, *time.Time) {
	t.Helper()
	c, err := cache.ParseDBC(frameTestDBC)
	if err != nil {
		t.Fatal(err)
	}
	c.DBCSHA = "abc"
	sink := &memSink{}
	fw := newFrameWriter(c, "s1", "test", sink)
	now := time.Unix(frameT0+20, 0)
	fw.now = func() time.Time { return now }
	return fw, sink, &now
}

// batchAt is one Speed frame at Unix second sec (+ms).
func batchAt(sec int64, ms uint64) []byte {
	return canpBatch(uint64(sec)*1000+ms, 0x100)
}

func TestFrameWriterSendsOneRowPerFrame(t *testing.T) {
	fw, sink, now := newTestFrameWriter(t)

	var d ingressDecoder
	// Kph raw = 0x0201 = 513 in every packet canpBatch builds; 0x200 is not in the DBC.
	fw.Fold(d.Decode(nil, canpBatch(frameT0*1000, 0x100, 0x200, 0x100)))
	*now = now.Add(sessionRowEvery)
	fw.Fold(d.Decode(nil, batchAt(frameT0+10, 250)))
	fw.Close(0)

	// The first frame sends signal_def and session ahead of any frame row;
	// the session row is rewritten each second when its end moved.
	want := []string{
		`signal_def,dbc_sha=abc,signal=Speed.Kph factor=0.1,offset=0,unit="km/h" 1784825000000000000`,
		`session,id=s1 dbc_label="test",dbc_sha="abc",start=1784825000000i,end=1784825000000i 1784825000000000000`,
		`frame,session=s1,msg=Speed Kph=513i 1784825000000000000`,
		`frame,session=s1,msg=Speed Kph=513i 1784825000000000000`,
		`frame,session=s1,msg=Speed Kph=513i 1784825010250000000`,
		`session,id=s1 dbc_label="test",dbc_sha="abc",start=1784825000000i,end=1784825010250i 1784825000000000000`,
	}
	if strings.Join(sink.lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("sent:\n%s\nwant:\n%s", strings.Join(sink.lines, "\n"), strings.Join(want, "\n"))
	}
}

func TestFrameWriterSkipsBadCarClock(t *testing.T) {
	fw, sink, _ := newTestFrameWriter(t)

	var d ingressDecoder
	fw.Fold(d.Decode(nil, batchAt(5, 0)))                              // before SNTP sync, 1970
	fw.Fold(d.Decode(nil, batchAt(frameT0+20+frameMaxFutureSec+1, 0))) // ahead of the server
	if fw.badClock != 2 || len(sink.lines) != 0 {
		t.Fatalf("badClock=%d sent=%d, want 2 and 0", fw.badClock, len(sink.lines))
	}
	fw.Fold(d.Decode(nil, batchAt(frameT0, 0)))
	if sink.frames() != 1 {
		t.Fatalf("good frame not sent after bad ones")
	}
}

func TestBufPoolGetWaitsWhenEmpty(t *testing.T) {
	p := newBufPool(1, 8)
	b := p.Get(context.Background())
	if b == nil || len(b.data) != 8 || p.InUse() != 1 {
		t.Fatalf("got %+v in use %d", b, p.InUse())
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if p.Get(ctx) != nil {
		t.Fatal("empty pool handed out a buffer")
	}
	b.n = 5
	p.Put(b)
	if b2 := p.Get(context.Background()); b2 != b || b2.n != 0 {
		t.Fatalf("Put did not return a clean buffer")
	}
}

func TestFillBufHandsOverAfterWait(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	go client.Write([]byte("abc"))
	b := newBufPool(1, 64).Get(context.Background())
	start := time.Now()
	if err := fillBuf(server, b); err != nil {
		t.Fatal(err)
	}
	if string(b.Bytes()) != "abc" {
		t.Fatalf("got %q", b.Bytes())
	}
	if took := time.Since(start); took < poolFillWait {
		t.Fatalf("handed over after %v, before the %v wait", took, poolFillWait)
	}
}

func TestFillBufHandsOverWhenFull(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	go client.Write([]byte("abcdefgh"))
	b := newBufPool(1, 4).Get(context.Background())
	if err := fillBuf(server, b); err != nil {
		t.Fatal(err)
	}
	if string(b.Bytes()) != "abcd" {
		t.Fatalf("got %q", b.Bytes())
	}
}
