package nominal

import (
	"math"
	"os"
	"sync"
	"testing"
	"time"
)

type memWriter struct {
	mu     sync.Mutex
	points []Point
}

func (m *memWriter) write(channel, tap string, timeNs int64, value float64) error {
	m.mu.Lock()
	m.points = append(m.points, Point{Channel: channel, Tap: tap, TimeNs: timeNs, Value: value})
	m.mu.Unlock()
	return nil
}

func (m *memWriter) close() error { return nil }

func TestSinkDecodesOffLoop(t *testing.T) {
	path := t.TempDir() + "/mini.dbc"
	body := "BO_ 256 PACK_State: 8 Vector__XXX\n" +
		" SG_ Pack_Voltage : 16|24@1+ (0.001,0) [0|0] \"V\" Vector__XXX\n"
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	db, err := LoadDBC(path)
	if err != nil {
		t.Fatal(err)
	}
	out := &memWriter{}
	s := startSink(NewDecoder(db), out)
	frame := canpFrame(256, 8, []byte{0, 0, 0x10, 0x27, 0x00, 0, 0, 0}, 1_700_000_000_000)
	s.Feed(frame[:8])
	s.Feed(frame[8:])
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s.Feed(frame)
	out.mu.Lock()
	defer out.mu.Unlock()
	if len(out.points) != 1 {
		t.Fatalf("points=%d", len(out.points))
	}
	if out.points[0].Channel != "PACK_State.Pack_Voltage" || math.Abs(out.points[0].Value-10) > 1e-9 {
		t.Fatalf("point %+v", out.points[0])
	}
	if out.points[0].TimeNs != 1_700_000_000_000*int64(time.Millisecond) {
		t.Fatalf("time %d", out.points[0].TimeNs)
	}
}
