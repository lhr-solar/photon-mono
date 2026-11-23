package main

import (
	"bytes"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	// dbFlushRows and dbFlushAfter: buffer A goes to InfluxDB at 10K rows or
	// 250 ms after its first row (about 250 ms of rows at 1 MiB/s).
	dbFlushRows  = 10_000
	dbFlushAfter = 250 * time.Millisecond
	// dbBufferBytes preallocates each of A and B for a full flush.
	dbBufferBytes = 1 << 20

	influxMaxAttempts = 5
	influxTimeout     = 10 * time.Second
)

// rowBuf is one of the writer's two buffers: line protocol, one row per line.
type rowBuf struct {
	body    []byte
	rows    int
	started time.Time // when the first row arrived
}

// dbWriter aggregates rows and posts them to InfluxDB 3 Core
// (/api/v3/write_lp), one request per buffer. Rows fill buffer A; at
// dbFlushRows or dbFlushAfter, A goes to the sender and B becomes the one
// filling. If A fills again while B is still sending, Send waits. That pushes
// back through the decoder into the buffer pool.
type dbWriter struct {
	writeURL   string
	token      string
	client     *http.Client
	backoff    func(attempt int) time.Duration
	flushRows  int
	flushAfter time.Duration

	mu     sync.Mutex
	fill   *rowBuf     // A: rows collect here
	timer  *time.Timer // flushes fill dbFlushAfter after its first row
	closed bool

	spare   chan *rowBuf // B while it is not sending
	sending chan *rowBuf // the full buffer, for the sender
	done    chan struct{}

	written atomic.Int64 // rows InfluxDB accepted
	dropped atomic.Int64 // rows given up on (retries spent, rejected, or sent after Close)
	failing atomic.Bool
}

func newDBWriter(baseURL, db, token string) (*dbWriter, error) {
	u, err := url.Parse(strings.TrimRight(baseURL, "/"))
	if err != nil || u.Scheme == "" || u.Host == "" {
		return nil, fmt.Errorf("bad InfluxDB URL %q", baseURL)
	}
	u.Path += "/api/v3/write_lp"
	u.RawQuery = url.Values{"db": {db}, "precision": {"nanosecond"}}.Encode()

	w := &dbWriter{
		writeURL:   u.String(),
		token:      token,
		client:     &http.Client{Timeout: influxTimeout},
		flushRows:  dbFlushRows,
		flushAfter: dbFlushAfter,
		fill:       &rowBuf{body: make([]byte, 0, dbBufferBytes)},
		spare:      make(chan *rowBuf, 1),
		sending:    make(chan *rowBuf, 1),
		done:       make(chan struct{}),
		backoff: func(attempt int) time.Duration {
			return time.Duration(1<<attempt) * time.Second // 2, 4, 8, 16 s
		},
	}
	w.spare <- &rowBuf{body: make([]byte, 0, dbBufferBytes)}
	w.timer = time.AfterFunc(time.Hour, w.flushTimer)
	w.timer.Stop()
	go w.run()
	return w, nil
}

// Write adds n rows, already formatted, to buffer A in one copy. It does not
// keep rows. It blocks only while A is full and B is still being sent.
//
// A Write is one pool buffer's worth of rows (at most ~570), and it is never
// split, so A can pass dbFlushRows by that much before it is sent.
func (w *dbWriter) Write(rows []byte, n int) {
	if n == 0 {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed {
		w.dropped.Add(int64(n))
		return
	}
	if w.fill.rows == 0 {
		w.fill.started = time.Now()
		w.timer.Reset(w.flushAfter)
	}
	w.fill.body = append(w.fill.body, rows...)
	w.fill.rows += n
	if w.fill.rows >= w.flushRows {
		w.swapLocked()
	}
}

// flushTimer sends A once its first row is dbFlushAfter old. A swap by row
// count may have started a newer A; then it waits out that one's time.
func (w *dbWriter) flushTimer() {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed || w.fill.rows == 0 {
		return
	}
	if left := w.flushAfter - time.Since(w.fill.started); left > 0 {
		w.timer.Reset(left)
		return
	}
	w.swapLocked()
}

// swapLocked hands A to the sender and makes B the buffer that fills. It waits
// for B while B is still sending.
func (w *dbWriter) swapLocked() {
	if w.fill.rows == 0 {
		return
	}
	next := <-w.spare
	w.sending <- w.fill
	w.fill = next
}

// Close sends what is buffered, waiting at most wait.
func (w *dbWriter) Close(wait time.Duration) {
	go func() {
		w.mu.Lock()
		defer w.mu.Unlock()
		w.closed = true
		w.timer.Stop()
		w.swapLocked()
		close(w.sending)
	}()
	select {
	case <-w.done:
	case <-time.After(wait):
		log.Printf("[photon-server] InfluxDB: shutdown before buffers drained")
	}
	if n := w.dropped.Load(); n > 0 {
		log.Printf("[photon-server] InfluxDB: %d rows dropped this run", n)
	}
}

func (w *dbWriter) run() {
	defer close(w.done)
	for b := range w.sending {
		w.deliver(b)
		b.body, b.rows = b.body[:0], 0
		w.spare <- b
	}
}

// deliver posts one buffer, retrying temporary failures. Retries hold B, so
// a long outage backs up ingest; after influxMaxAttempts the rows are dropped.
func (w *dbWriter) deliver(b *rowBuf) {
	for attempt := 1; ; attempt++ {
		retry, err := w.post(b.body)
		if err == nil {
			w.written.Add(int64(b.rows))
			if w.failing.Swap(false) {
				log.Printf("[photon-server] InfluxDB: writes recovered")
			}
			return
		}
		if !w.failing.Swap(true) {
			log.Printf("[photon-server] InfluxDB: write failed: %v", err)
		}
		if !retry || attempt >= influxMaxAttempts {
			w.dropped.Add(int64(b.rows))
			return
		}
		time.Sleep(w.backoff(attempt))
	}
}

// post sends one body. retry reports whether the failure may be temporary.
func (w *dbWriter) post(body []byte) (retry bool, err error) {
	req, err := http.NewRequest(http.MethodPost, w.writeURL, bytes.NewReader(body))
	if err != nil {
		return false, err
	}
	req.Header.Set("Content-Type", "text/plain; charset=utf-8")
	if w.token != "" {
		req.Header.Set("Authorization", "Bearer "+w.token)
	}
	resp, err := w.client.Do(req)
	if err != nil {
		return true, err
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 == 2 {
		_, _ = io.Copy(io.Discard, resp.Body)
		return false, nil
	}
	msg, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
	err = fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(msg)))
	// 429 and 5xx are worth retrying; other 4xx (bad token, bad line) are not.
	return resp.StatusCode == http.StatusTooManyRequests || resp.StatusCode >= 500, err
}
