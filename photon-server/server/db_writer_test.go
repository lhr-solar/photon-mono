package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
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
	w, err := newDBWriter(srv.URL+"/", "photon_dev", token)
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
	for _, row := range []string{"a v=1i 1", "a v=2i 2", "a v=3i 3", "a v=4i 4", "a v=5i 5"} {
		send(w, row)
	}
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

// With A full and B still sending, Write waits instead of dropping.
func TestDBWriterBlocksWhileBothBuffersBusy(t *testing.T) {
	release := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		<-release
		rw.WriteHeader(http.StatusNoContent)
	}))
	defer srv.Close()
	w, err := newDBWriter(srv.URL, "photon_dev", "")
	if err != nil {
		t.Fatal(err)
	}
	w.flushRows = 1

	sent := make(chan struct{})
	go func() {
		send(w, "a v=1i 1") // A -> sending, stuck in InfluxDB
		send(w, "a v=2i 2") // B fills, waits for A to come back
		send(w, "a v=3i 3")
		close(sent)
	}()
	select {
	case <-sent:
		t.Fatal("Write returned while both buffers were busy")
	case <-time.After(100 * time.Millisecond):
	}
	close(release)
	select {
	case <-sent:
	case <-time.After(2 * time.Second):
		t.Fatal("Write still blocked after InfluxDB answered")
	}
	w.Close(time.Second)
	if w.written.Load() != 3 || w.dropped.Load() != 0 {
		t.Fatalf("written=%d dropped=%d, want 3 and 0", w.written.Load(), w.dropped.Load())
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
	if _, err := newDBWriter("127.0.0.1:8181", "db", ""); err == nil {
		t.Fatal("URL without scheme accepted")
	}
}
