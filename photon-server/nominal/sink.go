package nominal

import (
	"context"
	"fmt"
	"log"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	nstream "github.com/nominal-io/nominal-streaming-go"
)

// Options configures a live stream from decoded CAN into a Nominal dataset.
// The stream is off unless APIKey and DatasetRID are both set.
type Options struct {
	APIKey     string
	DatasetRID string
	BaseURL    string
	DBC        *Database
}

// Sink copies ingress bytes off the server append loop and writes decoded
// signals to Nominal. Feed never blocks the caller.
type Sink struct {
	dec     *Decoder
	out     pointWriter
	in      chan []byte
	done    chan struct{}
	mu      sync.Mutex
	closed  bool
	dropped atomic.Uint64
}

type pointWriter interface {
	write(channel, tap string, timeNs int64, value float64) error
	close() error
}

// Open starts a Nominal dataset stream. The caller must Close it.
func Open(ctx context.Context, opt Options) (*Sink, error) {
	key := strings.TrimSpace(opt.APIKey)
	rid := strings.TrimSpace(opt.DatasetRID)
	if key == "" || rid == "" {
		return nil, fmt.Errorf("nominal api key and dataset rid are required")
	}
	if opt.DBC == nil {
		return nil, fmt.Errorf("nominal dbc is required")
	}
	var clientOpts []nstream.Option
	if base := strings.TrimSpace(opt.BaseURL); base != "" {
		clientOpts = append(clientOpts, nstream.WithBaseURL(base))
	}
	client, err := nstream.NewClient(key, clientOpts...)
	if err != nil {
		return nil, err
	}
	ds, errCh, err := client.NewDatasetStream(ctx, rid,
		nstream.WithBatchSize(8192),
		nstream.WithFlushInterval(500*time.Millisecond),
	)
	if err != nil {
		return nil, err
	}
	go logStreamErrors(errCh)
	return startSink(NewDecoder(opt.DBC), &datasetWriter{ds: ds}), nil
}

func startSink(dec *Decoder, out pointWriter) *Sink {
	s := &Sink{
		dec:  dec,
		out:  out,
		in:   make(chan []byte, 256),
		done: make(chan struct{}),
	}
	go s.run()
	return s
}

func (s *Sink) run() {
	defer close(s.done)
	for payload := range s.in {
		for _, pt := range s.dec.Push(payload, time.Now()) {
			if err := s.out.write(pt.Channel, pt.Tap, pt.TimeNs, pt.Value); err != nil {
				log.Printf("[photon-server] nominal enqueue %s: %v", pt.Channel, err)
			}
		}
	}
}

// Feed queues one ingress chunk. A full queue drops the chunk so CAN storage
// is not stalled by the Nominal client.
func (s *Sink) Feed(payload []byte) {
	if s == nil || len(payload) == 0 {
		return
	}
	buf := make([]byte, len(payload))
	copy(buf, payload)
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return
	}
	select {
	case s.in <- buf:
	default:
		n := s.dropped.Add(1)
		if n == 1 || n%1000 == 0 {
			log.Printf("[photon-server] nominal ingress queue full, dropped %d chunks", n)
		}
	}
}

// Close drains queued chunks and flushes the Nominal batch.
func (s *Sink) Close() error {
	if s == nil {
		return nil
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	close(s.in)
	s.mu.Unlock()
	<-s.done
	return s.out.close()
}

type datasetWriter struct {
	ds    *nstream.DatasetStream
	mu    sync.Mutex
	chans map[string]*nstream.ChannelStream[float64]
}

func (w *datasetWriter) write(channel, tap string, timeNs int64, value float64) error {
	w.mu.Lock()
	ch := w.channel(channel, tap)
	w.mu.Unlock()
	ch.Enqueue(timeNs, value)
	return nil
}

func (w *datasetWriter) channel(name, tap string) *nstream.ChannelStream[float64] {
	if w.chans == nil {
		w.chans = map[string]*nstream.ChannelStream[float64]{}
	}
	key := name + "\x00" + tap
	if ch := w.chans[key]; ch != nil {
		return ch
	}
	tags := nstream.Tags{"source": "photon-server"}
	if tap != "" {
		tags["tap"] = tap
	}
	ch := w.ds.FloatStream(name, nstream.WithTags(tags))
	w.chans[key] = ch
	return ch
}

func (w *datasetWriter) close() error {
	if w == nil || w.ds == nil {
		return nil
	}
	return w.ds.Close()
}

func logStreamErrors(errs <-chan error) {
	for err := range errs {
		if err != nil {
			log.Printf("[photon-server] nominal stream: %v", err)
		}
	}
}
