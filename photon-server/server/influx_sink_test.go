package main

import (
	"io"
	"net/http"
	"net/http/httptest"
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

func newTestSink(t *testing.T, f *fakeInflux, token string) *influxSink {
	t.Helper()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	s, err := newInfluxSink(srv.URL+"/", "photon_dev", token)
	if err != nil {
		t.Fatal(err)
	}
	s.backoff = func(int) time.Duration { return time.Millisecond }
	return s
}

func TestInfluxSinkWritesLineProtocol(t *testing.T) {
	f := &fakeInflux{}
	s := newTestSink(t, f, "tok")
	s.Send([]string{"a v=1i 1", "b v=2i 2"})
	s.Close(time.Second)

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
	if s.written.Load() != 2 || s.dropped.Load() != 0 {
		t.Fatalf("written=%d dropped=%d", s.written.Load(), s.dropped.Load())
	}
}

func TestInfluxSinkRetriesServerErrors(t *testing.T) {
	f := &fakeInflux{status: []int{503, 500}}
	s := newTestSink(t, f, "")
	s.Send([]string{"a v=1i 1"})
	s.Close(time.Second)

	if len(f.reqs) != 3 || s.written.Load() != 1 {
		t.Fatalf("requests=%d written=%d, want 3 and 1", len(f.reqs), s.written.Load())
	}
	if f.reqs[0].Header.Get("Authorization") != "" {
		t.Fatal("sent Authorization with no token")
	}
}

func TestInfluxSinkDropsRejectedWrites(t *testing.T) {
	f := &fakeInflux{status: []int{401}}
	s := newTestSink(t, f, "bad")
	s.Send([]string{"a v=1i 1"})
	s.Close(time.Second)

	if len(f.reqs) != 1 || s.dropped.Load() != 1 {
		t.Fatalf("requests=%d dropped=%d, want 1 and 1 (no retry on 401)", len(f.reqs), s.dropped.Load())
	}
}

func TestInfluxSinkGivesUpAfterMaxAttempts(t *testing.T) {
	f := &fakeInflux{status: []int{500, 500, 500, 500, 500, 500}}
	s := newTestSink(t, f, "")
	s.Send([]string{"a v=1i 1"})
	s.Close(time.Second)

	if len(f.reqs) != influxMaxAttempts || s.dropped.Load() != 1 {
		t.Fatalf("requests=%d dropped=%d, want %d and 1", len(f.reqs), s.dropped.Load(), influxMaxAttempts)
	}
}

func TestInfluxSinkSendNeverBlocks(t *testing.T) {
	block := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		<-block
		w.WriteHeader(http.StatusNoContent)
	}))
	defer srv.Close()
	defer close(block)
	s, err := newInfluxSink(srv.URL, "photon_dev", "")
	if err != nil {
		t.Fatal(err)
	}

	done := make(chan struct{})
	go func() {
		for i := 0; i < influxQueueBatches+10; i++ {
			s.Send([]string{"a v=1i 1"})
		}
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Send blocked while InfluxDB was stuck")
	}
	if s.dropped.Load() == 0 {
		t.Fatal("full queue dropped nothing")
	}
}

func TestInfluxSinkRejectsBadURL(t *testing.T) {
	if _, err := newInfluxSink("127.0.0.1:8181", "db", ""); err == nil {
		t.Fatal("URL without scheme accepted")
	}
}
