package main

import (
	"log"
	"path/filepath"
	"server/cache"
	"strings"
	"time"
)

const (
	// frameMaxFutureSec rejects frames this far ahead of the server clock.
	frameMaxFutureSec = 60
	// frameMinValidSec (2020-01-01) drops frames sent before the car's clock
	// was set, which would otherwise be written near 1970.
	frameMinValidSec = 1577836800
	// sessionRowEvery is how often the session row's end is rewritten.
	sessionRowEvery = time.Second
)

// lineSink takes finished line-protocol rows. dbWriter is the real one.
type lineSink interface {
	// Write adds n rows; rows holds them back to back, each ending in '\n'.
	// Write must not keep rows after it returns; the caller reuses it.
	Write(rows []byte, n int)
	Close(wait time.Duration)
}

// frameWriter turns decoded frames into rows, one per frame, for the DB
// writer. Only appendLoop (the decoder) touches it.
type frameWriter struct {
	cache   *cache.SessionCache // DBC definitions
	session string
	// rowPrefix is "frame,session=<escaped session>", built once.
	rowPrefix string
	dbcLabel  string
	sink      lineSink
	now       func() time.Time

	// startMs and endMs bound the session on the frame clock, as the session
	// row records them. sentEndMs is the end the last session row carried.
	startMs     int64
	endMs       int64
	sentEndMs   int64
	lastSession time.Time

	// buf collects one Fold's rows, built in place; it is reused, so a row
	// costs no allocation. n counts the rows in it.
	buf      []byte
	n        int
	badClock int64 // frames before frameMinValidSec or too far in the future
}

// startFrameWriter loads the DBC and starts the DB writer. It returns nil,
// with a log line, when InfluxDB writes are off or anything fails.
func startFrameWriter(cfg *Config, startedAt time.Time) *frameWriter {
	if strings.EqualFold(cfg.InfluxURL, "off") {
		log.Printf("[photon-server] InfluxDB writes off (-influx-url off)")
		return nil
	}
	dbc := resolveDbc(cfg)
	if dbc == "" {
		log.Printf("[photon-server] No DBC set (-dbc / PHOTON_DBC); InfluxDB writes off")
		return nil
	}
	c, err := cache.LoadDBC(dbc)
	if err != nil {
		log.Printf("[photon-server] InfluxDB writes off: %v", err)
		return nil
	}
	c.SessionID = startedAt.UTC().Format("20060102T150405Z")
	log.Printf("[photon-server] DBC loaded from %s: %d signals", dbc, len(c.Definitions))

	sink, err := newDBWriter(cfg.InfluxURL, cfg.InfluxDB, cfg.InfluxToken)
	if err != nil {
		log.Printf("[photon-server] InfluxDB writes off: %v", err)
		return nil
	}
	if cfg.InfluxToken == "" {
		log.Printf("[photon-server] InfluxDB token not set (PHOTON_INFLUX_TOKEN); writes need a server started --without-auth")
	}
	log.Printf("[photon-server] Frames -> InfluxDB %s database %q, session %s", cfg.InfluxURL, cfg.InfluxDB, c.SessionID)
	label := strings.TrimSuffix(filepath.Base(dbc), filepath.Ext(dbc))
	return newFrameWriter(c, c.SessionID, label, sink)
}

// newFrameWriter accepts frames. The session and signal_def rows wait for
// the first good frame, because the session starts on the frame clock.
func newFrameWriter(c *cache.SessionCache, session, dbcLabel string, sink lineSink) *frameWriter {
	return &frameWriter{cache: c, session: session, rowPrefix: cache.FramePrefix(session),
		dbcLabel: dbcLabel, sink: sink, now: time.Now}
}

// Fold sends one row per frame with a good clock and a known CAN ID.
func (w *frameWriter) Fold(frames []cache.Frame) {
	now := w.now()
	maxSec := now.Unix() + frameMaxFutureSec
	for _, fr := range frames {
		sec := int64(fr.TimeMs / 1000)
		if sec < frameMinValidSec || sec > maxSec {
			if w.badClock == 0 {
				log.Printf("[photon-server] Skipping frames with a bad car clock (first at %d s)", sec)
			}
			w.badClock++
			continue
		}
		w.markSession(int64(fr.TimeMs))
		var ok bool
		if w.buf, ok = w.cache.AppendFrameLine(w.buf, w.rowPrefix, fr); ok {
			w.n++
		}
	}
	if now.Sub(w.lastSession) >= sessionRowEvery {
		w.lastSession = now
		w.sendSession()
	}
	w.flush()
}

// flush hands the collected rows to the sink in one call.
func (w *frameWriter) flush() {
	w.sink.Write(w.buf, w.n)
	w.buf, w.n = w.buf[:0], 0
}

// addLine queues one preformatted row (signal_def or session).
func (w *frameWriter) addLine(line string) {
	w.buf = append(w.buf, line...)
	w.buf = append(w.buf, '\n')
	w.n++
}

// markSession widens the session to ms. The first frame queues the
// signal_def and session rows ahead of it, so scale factors are stored
// before any frame row.
func (w *frameWriter) markSession(ms int64) {
	if w.startMs == 0 {
		w.startMs, w.endMs = ms, ms
		for _, line := range cache.SignalDefLines(w.cache, w.cache.DBCSHA, ms) {
			w.addLine(line)
		}
		w.sendSession()
		return
	}
	if ms > w.endMs {
		w.endMs = ms
	}
}

// sendSession queues a rewrite of the session row when its end has moved.
func (w *frameWriter) sendSession() {
	if w.startMs == 0 || w.endMs == w.sentEndMs {
		return
	}
	w.addLine(cache.SessionLine(w.session, w.dbcLabel, w.cache.DBCSHA, w.startMs, w.endMs))
	w.sentEndMs = w.endMs
}

// Close writes the final session row and gives the sink up to wait to deliver.
func (w *frameWriter) Close(wait time.Duration) {
	if w.badClock > 0 {
		log.Printf("[photon-server] Dropped %d frames with a bad car clock", w.badClock)
	}
	w.sendSession()
	w.flush()
	w.sink.Close(wait)
}
