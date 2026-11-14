package main

import (
	"bytes"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"strings"
	"sync/atomic"
	"time"
)

const (
	// influxQueueBatches is how many Send calls can wait while a post is in
	// flight or InfluxDB is down. Raw segments keep every frame regardless.
	influxQueueBatches = 600
	influxMaxAttempts  = 5
	influxTimeout      = 10 * time.Second
)

// influxOptions tunes how the sink writes.
type influxOptions struct {
	// Flush is how often queued lines are joined and posted. Zero posts each
	// Send on its own.
	Flush time.Duration
	// NoSync asks InfluxDB to acknowledge before its WAL is saved. The ack
	// comes sooner, but a crash can lose the last WAL interval and some write
	// errors (bad lines, type conflicts) are no longer reported.
	NoSync bool
}

// influxSink posts line protocol to InfluxDB 3 Core (/api/v3/write_lp) from
// its own goroutine, so a slow or down database never blocks ingest.
type influxSink struct {
	writeURL string
	token    string
	client   *http.Client
	flush    time.Duration
	queue    chan []string
	done     chan struct{}
	backoff  func(attempt int) time.Duration

	written atomic.Int64 // lines InfluxDB accepted
	dropped atomic.Int64 // lines given up on (queue full, retries spent, or rejected)
	failing atomic.Bool
}

func newInfluxSink(baseURL, db, token string, opts influxOptions) (*influxSink, error) {
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

	s := &influxSink{
		writeURL: u.String(),
		token:    token,
		client:   &http.Client{Timeout: influxTimeout},
		flush:    opts.Flush,
		queue:    make(chan []string, influxQueueBatches),
		done:     make(chan struct{}),
		backoff: func(attempt int) time.Duration {
			return time.Duration(1<<attempt) * time.Second // 2, 4, 8, 16 s
		},
	}
	go s.run()
	return s, nil
}

// Send queues lines without blocking. When the queue is full the lines are
// dropped and counted.
func (s *influxSink) Send(lines []string) {
	if len(lines) == 0 {
		return
	}
	select {
	case s.queue <- lines:
	default:
		s.dropped.Add(int64(len(lines)))
	}
}

// Close sends what is queued, waiting at most wait.
func (s *influxSink) Close(wait time.Duration) {
	close(s.queue)
	select {
	case <-s.done:
	case <-time.After(wait):
		log.Printf("[photon-server] InfluxDB: shutdown before queue drained")
	}
	if n := s.dropped.Load(); n > 0 {
		log.Printf("[photon-server] InfluxDB: %d tally lines dropped this run", n)
	}
}

func (s *influxSink) run() {
	defer close(s.done)
	if s.flush <= 0 {
		for lines := range s.queue {
			s.deliver(lines)
		}
		return
	}
	// Join everything that arrives within one interval into a single post.
	tick := time.NewTicker(s.flush)
	defer tick.Stop()
	var pending []string
	for {
		select {
		case lines, ok := <-s.queue:
			if !ok {
				if len(pending) > 0 {
					s.deliver(pending)
				}
				return
			}
			pending = append(pending, lines...)
		case <-tick.C:
			if len(pending) > 0 {
				s.deliver(pending)
				pending = nil
			}
		}
	}
}

func (s *influxSink) deliver(lines []string) {
	body := []byte(strings.Join(lines, "\n") + "\n")
	for attempt := 1; ; attempt++ {
		retry, err := s.post(body)
		if err == nil {
			s.written.Add(int64(len(lines)))
			if s.failing.Swap(false) {
				log.Printf("[photon-server] InfluxDB: writes recovered")
			}
			return
		}
		if !s.failing.Swap(true) {
			log.Printf("[photon-server] InfluxDB: write failed: %v", err)
		}
		if !retry || attempt >= influxMaxAttempts {
			s.dropped.Add(int64(len(lines)))
			return
		}
		time.Sleep(s.backoff(attempt))
	}
}

// post sends one body. retry reports whether the failure may be temporary.
func (s *influxSink) post(body []byte) (retry bool, err error) {
	req, err := http.NewRequest(http.MethodPost, s.writeURL, bytes.NewReader(body))
	if err != nil {
		return false, err
	}
	req.Header.Set("Content-Type", "text/plain; charset=utf-8")
	if s.token != "" {
		req.Header.Set("Authorization", "Bearer "+s.token)
	}
	resp, err := s.client.Do(req)
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
