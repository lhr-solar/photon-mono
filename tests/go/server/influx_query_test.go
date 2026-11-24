package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// fakeQueries is an InfluxDB query endpoint for the tests. It records each query
// and answers with reply, which gets the request's context, so it can block. The
// span queries of runs (the session table) are answered with span, a JSON line or
// nothing, and are not recorded: most tests do not ask for them.
type fakeQueries struct {
	mu        sync.Mutex
	reqs      []fakeQuery
	reply     func(ctx context.Context, q fakeQuery) (status int, body string)
	span      string
	spanReads atomic.Int32 // span queries answered
}

// isSpanQuery reports whether sql is a run's span query, which the fake answers apart.
func isSpanQuery(sql string) bool {
	return strings.Contains(sql, "FROM session WHERE")
}

// fakeQuery is one query the fake received, with the headers the tests check.
type fakeQuery struct {
	Path   string
	Auth   string            // the Authorization header
	DB     string            `json:"db"`
	SQL    string            `json:"q"`
	Params map[string]string `json:"params"`
	Format string            `json:"format"`
}

func (f *fakeQueries) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	var q fakeQuery
	body, _ := io.ReadAll(r.Body)
	_ = json.Unmarshal(body, &q)
	q.Path, q.Auth = r.URL.Path, r.Header.Get("Authorization")
	if isSpanQuery(q.SQL) {
		f.spanReads.Add(1)
		_, _ = io.WriteString(w, f.span)
		return
	}
	f.mu.Lock()
	f.reqs = append(f.reqs, q)
	reply := f.reply
	f.mu.Unlock()
	status, text := http.StatusOK, ""
	if reply != nil {
		status, text = reply(r.Context(), q)
	}
	w.WriteHeader(status)
	_, _ = io.WriteString(w, text)
}

// queries returns the queries received so far.
func (f *fakeQueries) queries() []fakeQuery {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]fakeQuery(nil), f.reqs...)
}

// newFakeQueryServer serves f and returns its base URL. The server closes when the
// test ends, after f's replies have returned.
func newFakeQueryServer(t *testing.T, f *fakeQueries) string {
	t.Helper()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	return srv.URL
}

// once returns a function that runs f the first time it is called only.
func once(f func()) func() {
	var o sync.Once
	return func() { o.Do(f) }
}

func TestInfluxQueryBearerTokenAndRows(t *testing.T) {
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, `{"t_ns":1795408093135000000,"lo":0.0,"hi":0.75,"mean":0.375,"n":4}` + "\n" +
			`{"t_ns":1795408093136000000,"lo":1,"hi":2,"mean":1.5}` + "\n"
	}}
	q := newInfluxQuerier(newFakeQueryServer(t, f)+"/", "photon_dev", "tok", 2)
	rows, err := q.query(context.Background(), "SELECT 1", map[string]string{"run": "r1"})
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 2 {
		t.Fatalf("%d rows, want 2", len(rows))
	}
	// Nanosecond times are above 2^53, so they must not pass through a float.
	if ns, err := rowInt(rows[0], "t_ns"); err != nil || ns != 1795408093135000000 {
		t.Fatalf("t_ns = %d, %v; want 1795408093135000000 exactly", ns, err)
	}
	if mean, err := rowFloat(rows[0], "mean"); err != nil || mean != 0.375 {
		t.Fatalf("mean = %v, %v; want 0.375", mean, err)
	}
	got := f.queries()[0]
	if got.Path != "/api/v3/query_sql" || got.Auth != "Bearer tok" || got.DB != "photon_dev" ||
		got.SQL != "SELECT 1" || got.Params["run"] != "r1" || got.Format != "jsonl" {
		t.Fatalf("query %+v", got)
	}
}

func TestInfluxQueryEmptyBodyHasNoRows(t *testing.T) {
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, "" }}
	q := newInfluxQuerier(newFakeQueryServer(t, f), "photon_dev", "", 2)
	rows, err := q.query(context.Background(), "SELECT 1", nil)
	if err != nil || len(rows) != 0 {
		t.Fatalf("rows %v, err %v; want no rows and no error", rows, err)
	}
	if q := f.queries()[0]; q.Auth != "" || q.Params == nil {
		t.Fatalf("no token is set, so no Authorization header; params must be an object: %+v", q)
	}
}

// Replies that mean "nothing written yet" give errNoData; others are errors.
func TestInfluxQueryNoDataReplies(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status int
		body   string
		noData bool
	}{
		{"missing column", http.StatusInternalServerError, `Schema error: No field named "tap". Valid fields are frame.v.`, true},
		{"missing database", http.StatusNotFound, `{"error":"query error: database not found: photon_dev"}`, true},
		{"missing table", http.StatusBadRequest, `Error during planning: table 'public.iox.session' not found`, true},
		{"server error", http.StatusInternalServerError, "boom", false},
		{"other 404", http.StatusNotFound, "no such endpoint", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return tc.status, tc.body }}
			q := newInfluxQuerier(newFakeQueryServer(t, f), "photon_dev", "", 2)
			rows, err := q.query(context.Background(), "SELECT 1", nil)
			if rows != nil {
				t.Fatalf("rows %v, want none", rows)
			}
			if got := errors.Is(err, errNoData); got != tc.noData {
				t.Fatalf("error %v: no-data %v, want %v", err, got, tc.noData)
			}
			if err != nil && !tc.noData && !strings.Contains(err.Error(), tc.body) {
				t.Fatalf("error %q does not carry the reply %q", err, tc.body)
			}
			want := uint64(1)
			if tc.noData {
				want = 0
			}
			if n := q.failures.Load(); n != want {
				t.Fatalf("failures = %d, want %d", n, want)
			}
		})
	}
}

// At most maxConcurrent queries reach InfluxDB at once; the rest wait for a slot.
func TestInfluxQuerySemaphoreLimitsConcurrency(t *testing.T) {
	release := make(chan struct{})
	unblock := once(func() { close(release) })
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		<-release
		return http.StatusOK, ""
	}}
	url := newFakeQueryServer(t, f)
	t.Cleanup(unblock) // runs before the server closes, so blocked handlers return
	q := newInfluxQuerier(url, "photon_dev", "", 2)

	var wg sync.WaitGroup
	var failed atomic.Int32
	for range 3 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if _, err := q.query(context.Background(), "SELECT 1", nil); err != nil {
				failed.Add(1)
			}
		}()
	}
	waitUntil(t, "two queries at InfluxDB", func() bool { return len(f.queries()) == 2 })
	time.Sleep(100 * time.Millisecond)
	if n := len(f.queries()); n != 2 {
		t.Fatalf("%d queries reached InfluxDB at once, want 2", n)
	}
	if n := len(q.sem); n != 2 {
		t.Fatalf("%d slots in use, want 2", n)
	}
	unblock()
	wg.Wait()
	if failed.Load() != 0 || len(f.queries()) != 3 {
		t.Fatalf("%d failed, %d reached InfluxDB; want 0 failed and 3 reached", failed.Load(), len(f.queries()))
	}
}

// A query waiting for a slot gives up when its context ends, and never reaches
// InfluxDB.
func TestInfluxQueryCancelWhileWaiting(t *testing.T) {
	release := make(chan struct{})
	unblock := once(func() { close(release) })
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		<-release
		return http.StatusOK, ""
	}}
	url := newFakeQueryServer(t, f)
	t.Cleanup(unblock)
	q := newInfluxQuerier(url, "photon_dev", "", 1)

	go func() { _, _ = q.query(context.Background(), "first", nil) }()
	waitUntil(t, "the first query at InfluxDB", func() bool { return len(f.queries()) == 1 })

	ctx, cancel := context.WithCancel(context.Background())
	errc := make(chan error, 1)
	go func() {
		_, err := q.query(ctx, "second", nil)
		errc <- err
	}()
	time.Sleep(50 * time.Millisecond)
	cancel()
	select {
	case err := <-errc:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("waiting query returned %v, want context canceled", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("waiting query still blocked after its context ended")
	}
	if n := len(f.queries()); n != 1 {
		t.Fatalf("%d queries reached InfluxDB, want 1", n)
	}
	unblock()
}

// The rows of a stream arrive in order, with the fields in the columns' order.
func TestInfluxStreamRowsInOrder(t *testing.T) {
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, "t_ns,v\n1,2\n3,4.5\n"
	}}
	q := newInfluxQuerier(newFakeQueryServer(t, f)+"/", "photon_dev", "tok", 2)
	var got [][]string
	err := q.stream(context.Background(), "SELECT 1", map[string]string{"run": "r1"}, []string{"t_ns", "v"}, func(row []string) error {
		got = append(got, slices.Clone(row)) // the row is reused, so a copy is kept
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if want := [][]string{{"1", "2"}, {"3", "4.5"}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("rows %q, want %q", got, want)
	}
	sent := f.queries()[0]
	if sent.Path != "/api/v3/query_sql" || sent.Auth != "Bearer tok" || sent.DB != "photon_dev" ||
		sent.SQL != "SELECT 1" || sent.Params["run"] != "r1" || sent.Format != "csv" {
		t.Fatalf("query %+v", sent)
	}
}

// A NULL field arrives as an empty string.
func TestInfluxStreamNULLIsEmpty(t *testing.T) {
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, "t_ns,v,w\n1795408093136000000,2,\n"
	}}
	q := newInfluxQuerier(newFakeQueryServer(t, f), "photon_dev", "", 2)
	var got [][]string
	err := q.stream(context.Background(), "SELECT 1", nil, []string{"t_ns", "v", "w"}, func(row []string) error {
		got = append(got, slices.Clone(row))
		return nil
	})
	if want := [][]string{{"1795408093136000000", "2", ""}}; err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("rows %q, err %v; want %q", got, err, want)
	}
}

// An empty body is a query with no rows, and so is a reply with only its header.
func TestInfluxStreamWithNoRows(t *testing.T) {
	for _, body := range []string{"", "t_ns,v\n"} {
		f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, body }}
		q := newInfluxQuerier(newFakeQueryServer(t, f), "photon_dev", "", 2)
		err := q.stream(context.Background(), "SELECT 1", nil, []string{"t_ns", "v"}, func([]string) error {
			t.Fatalf("a row was delivered for reply %q", body)
			return nil
		})
		if err != nil {
			t.Fatalf("reply %q: %v", body, err)
		}
	}
}

// Replies that mean nothing is written yet give errNoData in a stream too; others are errors.
func TestInfluxStreamNoDataReplies(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status int
		body   string
		noData bool
	}{
		{"missing column", http.StatusInternalServerError, `Schema error: No field named "tap". Valid fields are frame.v.`, true},
		{"missing database", http.StatusNotFound, `{"error":"query error: database not found: photon_dev"}`, true},
		{"missing table", http.StatusBadRequest, `Error during planning: table 'public.iox.frame' not found`, true},
		{"server error", http.StatusInternalServerError, "boom", false},
		{"other 404", http.StatusNotFound, "no such endpoint", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return tc.status, tc.body }}
			q := newInfluxQuerier(newFakeQueryServer(t, f), "photon_dev", "", 2)
			err := q.stream(context.Background(), "SELECT 1", nil, []string{"t_ns", "v"}, func([]string) error {
				t.Fatal("a row was delivered for an error reply")
				return nil
			})
			if got := errors.Is(err, errNoData); got != tc.noData {
				t.Fatalf("error %v: no-data %v, want %v", err, got, tc.noData)
			}
			if err != nil && !tc.noData && !strings.Contains(err.Error(), tc.body) {
				t.Fatalf("error %q does not carry the reply %q", err, tc.body)
			}
			want := uint64(1)
			if tc.noData {
				want = 0
			}
			if n := q.failures.Load(); n != want {
				t.Fatalf("failures = %d, want %d", n, want)
			}
		})
	}
}

// The header must name the columns asked for, and every row must have as many fields.
func TestInfluxStreamChecksColumnsAndFieldCount(t *testing.T) {
	for _, body := range []string{"t_ns,value\n1,2\n", "t_ns,v\n1,2,3\n"} {
		f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, body }}
		q := newInfluxQuerier(newFakeQueryServer(t, f), "photon_dev", "", 2)
		err := q.stream(context.Background(), "SELECT 1", nil, []string{"t_ns", "v"}, func([]string) error {
			t.Fatalf("a row was delivered for reply %q", body)
			return nil
		})
		if err == nil {
			t.Fatalf("reply %q was accepted", body)
		}
	}
}

// An error from fn stops the read and is returned as it is.
func TestInfluxStreamFnErrorStopsTheRead(t *testing.T) {
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		return http.StatusOK, "t_ns,v\n1,1\n2,2\n3,3\n"
	}}
	q := newInfluxQuerier(newFakeQueryServer(t, f), "photon_dev", "", 2)
	stop := errors.New("stop here")
	calls := 0
	err := q.stream(context.Background(), "SELECT 1", nil, []string{"t_ns", "v"}, func([]string) error {
		calls++
		return stop
	})
	if err != stop || calls != 1 {
		t.Fatalf("err %v after %d rows, want the stop error after 1 row", err, calls)
	}
}

// At most maxConcurrent streams reach InfluxDB at once; the rest wait for a slot.
func TestInfluxStreamSemaphoreLimitsConcurrency(t *testing.T) {
	release := make(chan struct{})
	unblock := once(func() { close(release) })
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) {
		<-release
		return http.StatusOK, ""
	}}
	url := newFakeQueryServer(t, f)
	t.Cleanup(unblock) // runs before the server closes, so blocked handlers return
	q := newInfluxQuerier(url, "photon_dev", "", 2)

	var wg sync.WaitGroup
	var failed atomic.Int32
	for range 3 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			err := q.stream(context.Background(), "SELECT 1", nil, []string{"t_ns"}, func([]string) error { return nil })
			if err != nil {
				failed.Add(1)
			}
		}()
	}
	waitUntil(t, "two streams at InfluxDB", func() bool { return len(f.queries()) == 2 })
	time.Sleep(100 * time.Millisecond)
	if n := len(f.queries()); n != 2 {
		t.Fatalf("%d streams reached InfluxDB at once, want 2", n)
	}
	if n := len(q.sem); n != 2 {
		t.Fatalf("%d slots in use, want 2", n)
	}
	unblock()
	wg.Wait()
	if failed.Load() != 0 || len(f.queries()) != 3 {
		t.Fatalf("%d failed, %d reached InfluxDB; want 0 failed and 3 reached", failed.Load(), len(f.queries()))
	}
}

// Cancelling the context while the reply is still arriving ends the read, and is not
// counted as a failure.
func TestInfluxStreamCancelEndsTheRead(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "t_ns,v\n1,2\n")
		w.(http.Flusher).Flush()
		<-r.Context().Done() // the reply never ends
	}))
	t.Cleanup(srv.Close)
	q := newInfluxQuerier(srv.URL, "photon_dev", "", 1)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	seen := make(chan struct{}, 1)
	errc := make(chan error, 1)
	go func() {
		errc <- q.stream(ctx, "SELECT 1", nil, []string{"t_ns", "v"}, func([]string) error {
			select {
			case seen <- struct{}{}:
			default:
			}
			return nil
		})
	}()
	select {
	case <-seen:
	case <-time.After(5 * time.Second):
		t.Fatal("the first row never arrived")
	}
	cancel()
	select {
	case err := <-errc:
		if err == nil {
			t.Fatal("a cancelled read returned no error")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the read still waits after its context ended")
	}
	if n := q.failures.Load(); n != 0 {
		t.Fatalf("%d failures, want 0 for a cancelled read", n)
	}
}

func TestInfluxQueriesConfig(t *testing.T) {
	none := func(string) string { return "" }
	cfg, err := ParseConfigWithEnv(none, nil, io.Discard)
	if err != nil || cfg.InfluxQueries != DefaultInfluxQueries {
		t.Fatalf("default queries %d, %v; want %d", cfg.InfluxQueries, err, DefaultInfluxQueries)
	}
	env := map[string]string{"PHOTON_INFLUX_QUERIES": "8"}
	cfg, err = ParseConfigWithEnv(func(k string) string { return env[k] }, nil, io.Discard)
	if err != nil || cfg.InfluxQueries != 8 {
		t.Fatalf("env queries %d, %v; want 8", cfg.InfluxQueries, err)
	}
	cfg, err = ParseConfigWithEnv(none, []string{"-influx-queries", "4"}, io.Discard)
	if err != nil || cfg.InfluxQueries != 4 {
		t.Fatalf("flag queries %d, %v; want 4", cfg.InfluxQueries, err)
	}
	for _, args := range [][]string{{"-influx-queries", "0"}, {"-influx-queries", "65"}} {
		if _, err := ParseConfigWithEnv(none, args, io.Discard); err == nil {
			t.Fatalf("%v accepted", args)
		}
	}
}
