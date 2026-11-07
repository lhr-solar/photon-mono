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
	// influxQueueBatches holds about ten minutes of one-second flushes while
	// InfluxDB is down. Raw segments keep every frame regardless.
	influxQueueBatches = 600
	influxMaxAttempts  = 5
	influxTimeout      = 10 * time.Second
)

// influxSink posts line protocol to InfluxDB 3 Core (/api/v3/write_lp) from
// its own goroutine, so a slow or down database never blocks ingest.
type influxSink struct {
	writeURL string
	token    string
	client   *http.Client
	queue    chan []string
	done     chan struct{}
	backoff  func(attempt int) time.Duration

	written atomic.Int64 // lines InfluxDB accepted
	dropped atomic.Int64 // lines given up on (queue full, retries spent, or rejected)
	failing atomic.Bool
}

func newInfluxSink(baseURL, db, token string) (*influxSink, error) {
	u, err := url.Parse(strings.TrimRight(baseURL, "/"))
	if err != nil || u.Scheme == "" || u.Host == "" {
		return nil, fmt.Errorf("bad InfluxDB URL %q", baseURL)
	}
	u.Path += "/api/v3/write_lp"
	u.RawQuery = url.Values{"db": {db}, "precision": {"nanosecond"}}.Encode()

	s := &influxSink{
		writeURL: u.String(),
		token:    token,
		client:   &http.Client{Timeout: influxTimeout},
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
	for lines := range s.queue {
		s.deliver(lines)
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
