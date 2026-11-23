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
	// dbFlushRows: the InfluxDB 3 Core docs give 10,000 lines (or 10 MB) as
	// the optimal write batch. At peak ingest, 250 ms of rows is about that
	// many, so the row and timer limits trigger at about the same time.
	dbFlushRows = 10_000
	// dbBufferBytes is what each buffer starts with: about one 10,000-row
	// flush. A buffer grows past it only while the other one is still sending.
	dbBufferBytes = 1 << 20
	// dbMaxBatchBytes caps the filling buffer while the other one is still
	// sending: the 10 MB upper end of the docs' optimal batch. Rows past it
	// are dropped; raw segments keep every frame for backfill.
	dbMaxBatchBytes = 10_000_000

	influxMaxAttempts = 5
	influxTimeout     = 10 * time.Second
)

// dbWriterOptions tunes how the writer talks to InfluxDB.
type dbWriterOptions struct {
	// Flush sends the filling buffer this long after its first row. Zero
	// sends each Write as soon as the other buffer is free.
	Flush time.Duration
	// NoSync asks InfluxDB to acknowledge before its WAL is saved. The ack
	// comes sooner, but a crash can lose the last WAL interval and some write
	// errors (bad lines, type conflicts) are no longer reported.
	NoSync bool
	// Block makes Write wait for room instead of dropping rows, for a writer
	// whose caller can wait (backfill). The live server leaves it off: its
	// decoder must never wait on InfluxDB.
	Block bool
}

// rowBuf is one of the writer's two buffers: line protocol, one row per line.
type rowBuf struct {
	body    []byte
	rows    int
	started time.Time // when the first row arrived
}

// dbWriter is the one InfluxDB writer. It batches rows and posts them to
// InfluxDB 3 Core (/api/v3/write_lp), one request per buffer. Rows fill
// buffer A; at dbFlushRows or Flush after A's first row, A goes to the sender
// and B becomes the one filling.
//
// Write never blocks the decoder. While B is still sending (a slow or down
// InfluxDB, or retries), A keeps filling up to dbMaxBatchBytes and then drops
// rows. Raw segments keep every frame, so dropped rows can be backfilled.
type dbWriter struct {
	writeURL   string
	token      string
	client     *http.Client
	backoff    func(attempt int) time.Duration
	flushRows  int
	flushAfter time.Duration
	maxBytes   int
	block      bool // Write waits for room rather than dropping rows

	mu      sync.Mutex
	idle    *sync.Cond  // signalled when the sender hands its buffer back
	fill    *rowBuf     // A: rows collect here
	spare   *rowBuf     // B, or nil while it is sending
	timer   *time.Timer // sends fill flushAfter after its first row
	closed  bool
	sending chan *rowBuf // holds at most the one buffer being sent
	done    chan struct{}

	written  atomic.Int64 // rows InfluxDB accepted
	dropped  atomic.Int64 // rows given up on (buffer full, retries spent, rejected, or after Close)
	failing  atomic.Bool
	dropping atomic.Bool // logged that rows are being dropped for lack of room
}

func newDBWriter(baseURL, db, token string, opts dbWriterOptions) (*dbWriter, error) {
	u, err := url.Parse(strings.TrimRight(baseURL, "/"))
	if err != nil || u.Scheme == "" || u.Host == "" {
		return nil, fmt.Errorf("bad InfluxDB URL %q", baseURL)
	}
	u.Path += "/api/v3/write_lp"
	q := url.Values{"db": {db}, "precision": {"nanosecond"}}
	if opts.NoSync {
		q.Set("no_sync", "true")
	}
	u.RawQuery = q.Encode()

	w := &dbWriter{
		writeURL:   u.String(),
		token:      token,
		client:     &http.Client{Timeout: influxTimeout},
		flushRows:  dbFlushRows,
		flushAfter: opts.Flush,
		maxBytes:   dbMaxBatchBytes,
		block:      opts.Block,
		fill:       &rowBuf{body: make([]byte, 0, dbBufferBytes)},
		spare:      &rowBuf{body: make([]byte, 0, dbBufferBytes)},
		sending:    make(chan *rowBuf, 1),
		done:       make(chan struct{}),
		backoff: func(attempt int) time.Duration {
			return time.Duration(1<<attempt) * time.Second // 2, 4, 8, 16 s
		},
	}
	w.idle = sync.NewCond(&w.mu)
	w.timer = time.AfterFunc(time.Hour, w.flushTimer)
	w.timer.Stop()
	go w.run()
	return w, nil
}

// Write adds n rows, already formatted and each ending in '\n', to buffer A
// in one copy. It does not keep rows. When A is at dbMaxBatchBytes because B
// is still sending, the rows are dropped; a block writer waits for room
// instead.
func (w *dbWriter) Write(rows []byte, n int) {
	if n == 0 {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.block {
		// Once A is empty a single Write goes in, even one bigger than maxBytes.
		for len(w.fill.body)+len(rows) > w.maxBytes && w.fill.rows > 0 && !w.closed {
			if w.spare != nil {
				w.swapLocked()
			} else {
				w.idle.Wait() // the sender hands B back, then we look again
			}
		}
	}
	if w.closed {
		w.dropped.Add(int64(n))
		return
	}
	if !w.block && len(w.fill.body)+len(rows) > w.maxBytes {
		w.dropped.Add(int64(n))
		if !w.dropping.Swap(true) {
			log.Printf("[photon-server] InfluxDB is behind; dropping rows until it catches up (raw segments keep them)")
		}
		return
	}
	if w.fill.rows == 0 {
		w.fill.started = time.Now()
		if w.flushAfter > 0 {
			w.timer.Reset(w.flushAfter)
		}
	}
	w.fill.body = append(w.fill.body, rows...)
	w.fill.rows += n
	if w.dueLocked() {
		w.swapLocked()
	}
}

// Written is the number of rows InfluxDB has accepted.
func (w *dbWriter) Written() int64 { return w.written.Load() }

// Dropped is the number of rows given up on: buffer full, retries spent,
// rejected by InfluxDB, or written after Close.
func (w *dbWriter) Dropped() int64 { return w.dropped.Load() }

// dueLocked reports whether A should go to the sender now.
func (w *dbWriter) dueLocked() bool {
	return w.fill.rows > 0 &&
		(w.fill.rows >= w.flushRows || time.Since(w.fill.started) >= w.flushAfter)
}

// flushTimer sends A once its first row is flushAfter old. If B is still
// sending, the sender swaps A in when it is done.
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

// swapLocked hands A to the sender and makes B the buffer that fills. It does
// nothing while B is still sending, so it never waits.
func (w *dbWriter) swapLocked() {
	if w.fill.rows == 0 || w.spare == nil {
		return
	}
	w.sending <- w.fill // room for one, and the sender is idle
	w.fill, w.spare = w.spare, nil
	if w.dropping.Swap(false) {
		log.Printf("[photon-server] InfluxDB caught up; rows are written again")
	}
}

// Close sends what is buffered, waiting at most wait.
func (w *dbWriter) Close(wait time.Duration) {
	go func() {
		w.mu.Lock()
		defer w.mu.Unlock()
		w.closed = true
		w.timer.Stop()
		for w.spare == nil {
			w.idle.Wait()
		}
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

// run is the sender. After each buffer it takes B back and, if A is already
// due (it filled up while B was sending), sends A straight away.
func (w *dbWriter) run() {
	defer close(w.done)
	for b := range w.sending {
		w.deliver(b)
		w.mu.Lock()
		b.body, b.rows = b.body[:0], 0
		w.spare = b
		if !w.closed && w.dueLocked() {
			w.swapLocked()
		}
		w.idle.Broadcast()
		w.mu.Unlock()
	}
}

// deliver posts one buffer, retrying temporary failures. Retries hold B, so
// A fills meanwhile; after influxMaxAttempts the rows are dropped.
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
