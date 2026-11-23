package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// fakeInflux answers write_lp with the queued status codes, then 204.
type fakeInflux struct {
	mu     sync.Mutex
	status []int
	bodies []string
	reqs   []*http.Request
}

func (f *fakeInflux) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(r.Body)
	f.mu.Lock()
	defer f.mu.Unlock()
	f.reqs = append(f.reqs, r)
	f.bodies = append(f.bodies, string(body))
	code := http.StatusNoContent
	if len(f.status) > 0 {
		code, f.status = f.status[0], f.status[1:]
	}
	w.WriteHeader(code)
}

func (f *fakeInflux) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.reqs)
}

// send writes rows to w in one Write, as the frame writer does.
func send(w *dbWriter, rows ...string) {
	w.Write([]byte(strings.Join(rows, "\n")+"\n"), len(rows))
}

func newTestWriter(t *testing.T, f *fakeInflux, token string) *dbWriter {
	t.Helper()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	w, err := newDBWriter(srv.URL+"/", "photon_dev", token, dbWriterOptions{Flush: DefaultInfluxFlush})
	if err != nil {
		t.Fatal(err)
	}
	w.backoff = func(int) time.Duration { return time.Millisecond }
	return w
}

func TestDBWriterWritesLineProtocol(t *testing.T) {
	f := &fakeInflux{}
	w := newTestWriter(t, f, "tok")
	send(w, "a v=1i 1", "b v=2i 2")
	w.Close(time.Second)

	if len(f.reqs) != 1 {
		t.Fatalf("%d requests, want 1", len(f.reqs))
	}
	r := f.reqs[0]
	if r.Method != http.MethodPost || r.URL.Path != "/api/v3/write_lp" {
		t.Fatalf("got %s %s", r.Method, r.URL.Path)
	}
	if q := r.URL.Query(); q.Get("db") != "photon_dev" || q.Get("precision") != "nanosecond" {
		t.Fatalf("query %q", r.URL.RawQuery)
	}
	if got := r.Header.Get("Authorization"); got != "Bearer tok" {
		t.Fatalf("Authorization %q", got)
	}
	if f.bodies[0] != "a v=1i 1\nb v=2i 2\n" {
		t.Fatalf("body %q", f.bodies[0])
	}
	if w.written.Load() != 2 || w.dropped.Load() != 0 {
		t.Fatalf("written=%d dropped=%d", w.written.Load(), w.dropped.Load())
	}
}

func TestDBWriterFlushesAtRowCount(t *testing.T) {
	f := &fakeInflux{}
	w := newTestWriter(t, f, "")
	w.flushRows = 2
	// Wait for each send so B is free at every swap; while B is busy, A
	// keeps filling instead (TestDBWriterNeverBlocks).
	waitFor := func(n int) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for f.count() < n && time.Now().Before(deadline) {
			time.Sleep(time.Millisecond)
		}
		w.mu.Lock()
		for w.spare == nil {
			w.idle.Wait()
		}
		w.mu.Unlock()
	}
	send(w, "a v=1i 1")
	send(w, "a v=2i 2")
	waitFor(1)
	send(w, "a v=3i 3")
	send(w, "a v=4i 4")
	waitFor(2)
	send(w, "a v=5i 5")
	// A Write is never split: three rows in one Write go out together.
	send(w, "b v=1i 1", "b v=2i 2", "b v=3i 3")
	w.Close(time.Second)

	want := []string{"a v=1i 1\na v=2i 2\n", "a v=3i 3\na v=4i 4\n", "a v=5i 5\nb v=1i 1\nb v=2i 2\nb v=3i 3\n"}
	if len(f.bodies) != len(want) {
		t.Fatalf("bodies %q, want %q", f.bodies, want)
	}
	for i := range want {
		if f.bodies[i] != want[i] {
			t.Fatalf("bodies %q, want %q", f.bodies, want)
		}
	}
}

func TestDBWriterFlushesAfterTimer(t *testing.T) {
	f := &fakeInflux{}
	w := newTestWriter(t, f, "")
	w.flushAfter = 20 * time.Millisecond
	defer w.Close(time.Second)
	send(w, "a v=1i 1")

	deadline := time.Now().Add(2 * time.Second)
	for f.count() == 0 {
		if time.Now().After(deadline) {
			t.Fatal("one row never flushed without Close")
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// With B stuck in InfluxDB, Write never waits: A keeps filling up to its cap,
// rows past the cap are dropped, and A goes out once B comes back.
func TestDBWriterNeverBlocks(t *testing.T) {
	release := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		<-release
		_, _ = io.Copy(io.Discard, r.Body)
		rw.WriteHeader(http.StatusNoContent)
	}))
	defer srv.Close()
	w, err := newDBWriter(srv.URL, "photon_dev", "", dbWriterOptions{Flush: time.Hour})
	if err != nil {
		t.Fatal(err)
	}
	w.flushRows = 1
	row := "a v=1i 1\n"
	w.maxBytes = 2 * len(row) // A holds two rows while B is busy

	sent := make(chan struct{})
	go func() {
		send(w, "a v=1i 1") // A -> sending, stuck in InfluxDB
		send(w, "a v=1i 2") // B fills
		send(w, "a v=1i 3") // still room
		send(w, "a v=1i 4") // over the cap: dropped
		close(sent)
	}()
	select {
	case <-sent:
	case <-time.After(2 * time.Second):
		t.Fatal("Write blocked while InfluxDB was busy")
	}
	if got := w.dropped.Load(); got != 1 {
		t.Fatalf("dropped=%d while busy, want 1", got)
	}
	close(release)
	w.Close(2 * time.Second)
	if w.written.Load() != 3 || w.dropped.Load() != 1 {
		t.Fatalf("written=%d dropped=%d, want 3 and 1", w.written.Load(), w.dropped.Load())
	}
}

// Flush 0 sends each Write on its own while the sender is free.
func TestDBWriterFlushZeroSendsEachWrite(t *testing.T) {
	f := &fakeInflux{}
	srv := httptest.NewServer(f)
	defer srv.Close()
	w, err := newDBWriter(srv.URL, "photon_dev", "", dbWriterOptions{})
	if err != nil {
		t.Fatal(err)
	}
	send(w, "a v=1i 1")
	deadline := time.Now().Add(2 * time.Second)
	for f.count() < 1 && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if f.count() != 1 {
		t.Fatalf("requests=%d, want 1 without waiting for a timer", f.count())
	}
	w.Close(time.Second)
}

func TestDBWriterNoSyncQuery(t *testing.T) {
	for _, noSync := range []bool{false, true} {
		w, err := newDBWriter("http://127.0.0.1:8181", "photon_dev", "", dbWriterOptions{NoSync: noSync})
		if err != nil {
			t.Fatal(err)
		}
		if has := strings.Contains(w.writeURL, "no_sync=true"); has != noSync {
			t.Fatalf("NoSync=%v: writeURL %q", noSync, w.writeURL)
		}
		w.Close(time.Second)
	}
}

func TestDBWriterRetriesServerErrors(t *testing.T) {
	f := &fakeInflux{status: []int{503, 500}}
	w := newTestWriter(t, f, "")
	send(w, "a v=1i 1")
	w.Close(time.Second)

	if len(f.reqs) != 3 || w.written.Load() != 1 {
		t.Fatalf("requests=%d written=%d, want 3 and 1", len(f.reqs), w.written.Load())
	}
	if f.reqs[0].Header.Get("Authorization") != "" {
		t.Fatal("sent Authorization with no token")
	}
}

func TestDBWriterDropsRejectedWrites(t *testing.T) {
	f := &fakeInflux{status: []int{401}}
	w := newTestWriter(t, f, "bad")
	send(w, "a v=1i 1")
	w.Close(time.Second)

	if len(f.reqs) != 1 || w.dropped.Load() != 1 {
		t.Fatalf("requests=%d dropped=%d, want 1 and 1 (no retry on 401)", len(f.reqs), w.dropped.Load())
	}
}

func TestDBWriterGivesUpAfterMaxAttempts(t *testing.T) {
	f := &fakeInflux{status: []int{500, 500, 500, 500, 500, 500}}
	w := newTestWriter(t, f, "")
	send(w, "a v=1i 1")
	w.Close(time.Second)

	if len(f.reqs) != influxMaxAttempts || w.dropped.Load() != 1 {
		t.Fatalf("requests=%d dropped=%d, want %d and 1", len(f.reqs), w.dropped.Load(), influxMaxAttempts)
	}
}

func TestDBWriterRejectsBadURL(t *testing.T) {
	if _, err := newDBWriter("127.0.0.1:8181", "db", "", dbWriterOptions{}); err == nil {
		t.Fatal("URL without scheme accepted")
	}
}

// With Block set, a Write that finds buffer A full waits for InfluxDB to hand
// buffer B back, instead of dropping its rows. Here InfluxDB holds the first
// request until the test lets go, and every row arrives in order.
func TestDBWriterBlockWaitsForRoom(t *testing.T) {
	release := make(chan struct{})
	var calls atomic.Int32
	var mu sync.Mutex
	var bodies []string
	srv := httptest.NewServer(http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		if calls.Add(1) == 1 {
			<-release // a slow InfluxDB: the first buffer stays in flight
		}
		mu.Lock()
		bodies = append(bodies, string(body))
		mu.Unlock()
		rw.WriteHeader(http.StatusNoContent)
	}))
	defer srv.Close()
	w, err := newDBWriter(srv.URL, "photon_dev", "", dbWriterOptions{Flush: time.Hour, Block: true})
	if err != nil {
		t.Fatal(err)
	}
	w.flushRows = 1
	w.maxBytes = 2 * len("a v=1i 1\n") // A holds two rows while B is busy

	send(w, "a v=1i 1") // A goes out and stays in flight
	send(w, "a v=1i 2") // B fills
	send(w, "a v=1i 3") // B is at its cap
	blocked := make(chan struct{})
	go func() {
		send(w, "a v=1i 4") // over the cap: waits for B
		close(blocked)
	}()
	select {
	case <-blocked:
		t.Fatal("Write did not wait while InfluxDB held the buffer")
	case <-time.After(100 * time.Millisecond):
	}
	close(release)
	select {
	case <-blocked:
	case <-time.After(5 * time.Second):
		t.Fatal("Write still waiting after InfluxDB let go")
	}
	w.Close(5 * time.Second)

	if w.Written() != 4 || w.Dropped() != 0 {
		t.Fatalf("written=%d dropped=%d, want 4 and 0", w.Written(), w.Dropped())
	}
	mu.Lock()
	defer mu.Unlock()
	if got, want := strings.Join(bodies, ""), "a v=1i 1\na v=1i 2\na v=1i 3\na v=1i 4\n"; got != want {
		t.Fatalf("bodies %q, want %q", got, want)
	}
}

// A block writer takes a single Write bigger than its cap once buffer A is
// empty, rather than waiting for room that will never come.
func TestDBWriterBlockAcceptsOversizedWrite(t *testing.T) {
	f := &fakeInflux{}
	w := newTestWriter(t, f, "")
	w.block = true
	w.maxBytes = 4
	send(w, "a v=1i 1") // 10 bytes, more than the cap
	w.Close(time.Second)

	if w.Written() != 1 || w.Dropped() != 0 {
		t.Fatalf("written=%d dropped=%d, want 1 and 0", w.Written(), w.Dropped())
	}
}
