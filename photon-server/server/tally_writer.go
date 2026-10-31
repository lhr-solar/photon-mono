package main

import (
	"log"
	"os"
	"server/cache"
	"strings"
	"time"
)

const (
	// tallyCloseLagSec is how far behind the newest frame second a bucket must
	// be before it is written. Frames aged by δt can land a few seconds back.
	tallyCloseLagSec = 5
	// tallyMaxFutureSec rejects frames this far ahead of the server clock, so a
	// fast car clock cannot pin newest in the future.
	tallyMaxFutureSec = 60
	// tallyClockResetSec: a frame this far behind newest means the car clock
	// stepped back (SNTP). The car clamps δt age at ~65 s, so aged frames
	// never fall this far behind their own stream.
	tallyClockResetSec = 70
	// tallyMinValidSec (2020-01-01) drops frames sent before the car's clock
	// was set, which would otherwise be written near 1970.
	tallyMinValidSec = 1577836800
)

// lineSink takes finished line-protocol rows. influxSink is the real one.
type lineSink interface {
	Send(lines []string)
	Close(wait time.Duration)
}

// tallyWriter sends closed one-second tallies to a lineSink. Only appendLoop
// touches it and the cache.
type tallyWriter struct {
	cache   *cache.SessionCache
	session string
	sink    lineSink
	now     func() time.Time

	newest int64 // newest frame second seen
	// closedBefore: seconds below this are sent. A later frame for one of
	// them would add a second partial row, so it is dropped and counted.
	closedBefore int64
	late         int64
	badClock     int64 // frames before tallyMinValidSec or too far in the future
}

// startTally loads the DBC and starts the InfluxDB sink. It returns nil, with
// a log line, when the tally is off or anything fails.
func startTally(cfg *Config, startedAt time.Time) *tallyWriter {
	if strings.EqualFold(cfg.InfluxURL, "off") {
		log.Printf("[photon-server] Live tally off (-influx-url off)")
		return nil
	}
	dbc := resolveDbc(cfg)
	if dbc == "" {
		log.Printf("[photon-server] No DBC set (-dbc / PHOTON_DBC); live tally off")
		return nil
	}
	c, err := cache.LoadDBC(dbc)
	if err != nil {
		log.Printf("[photon-server] Live tally off: %v", err)
		return nil
	}
	c.SessionID = startedAt.UTC().Format("20060102T150405Z")
	log.Printf("[photon-server] DBC loaded from %s: %d signals", dbc, len(c.Definitions))

	token := influxToken(cfg)
	sink, err := newInfluxSink(cfg.InfluxURL, cfg.InfluxDatabase, token)
	if err != nil {
		log.Printf("[photon-server] Live tally off: %v", err)
		return nil
	}
	if token == "" {
		log.Printf("[photon-server] InfluxDB token not set (PHOTON_INFLUX_TOKEN); writes need a server started --without-auth")
	}
	log.Printf("[photon-server] Live tally -> InfluxDB %s database %q, session %s", cfg.InfluxURL, cfg.InfluxDatabase, c.SessionID)
	return newTallyWriter(c, c.SessionID, startedAt.Unix(), sink)
}

// influxToken prefers the env token. A token file is the local fallback.
func influxToken(cfg *Config) string {
	if cfg.InfluxToken != "" {
		return cfg.InfluxToken
	}
	if cfg.InfluxTokenFile == "" {
		return ""
	}
	b, err := os.ReadFile(cfg.InfluxTokenFile)
	if err != nil {
		log.Printf("[photon-server] InfluxDB token file %s: %v", cfg.InfluxTokenFile, err)
		return ""
	}
	return strings.TrimSpace(string(b))
}

// newTallyWriter sends one signal_def row per DBC signal, so stored raw
// integers can be scaled later, then accepts frames.
func newTallyWriter(c *cache.SessionCache, session string, start int64, sink lineSink) *tallyWriter {
	sink.Send(cache.SignalDefLines(c, c.DBCSHA, start))
	return &tallyWriter{cache: c, session: session, sink: sink, now: time.Now}
}

// Fold adds frames and remembers the newest second seen.
func (t *tallyWriter) Fold(frames []cache.Frame) {
	maxSec := t.now().Unix() + tallyMaxFutureSec
	for _, fr := range frames {
		sec := int64(fr.TimeMs / 1000)
		if sec < tallyMinValidSec || sec > maxSec {
			if t.badClock == 0 {
				log.Printf("[photon-server] Tally skipping frames with a bad car clock (first at %d s)", sec)
			}
			t.badClock++
			continue
		}
		if t.newest != 0 && sec < t.newest-tallyClockResetSec {
			t.resetClock(sec)
		}
		if sec < t.closedBefore {
			t.late++
			continue
		}
		t.cache.Fold(fr)
		if sec > t.newest {
			t.newest = sec
		}
	}
}

// resetClock follows a backward step of the car clock: write every open
// bucket, then accept frames from the new timeline.
func (t *tallyWriter) resetClock(sec int64) {
	log.Printf("[photon-server] Tally: car clock stepped back %d s; flushing open seconds", t.newest-sec)
	t.sink.Send(t.cache.TakeClosed(t.session, 1<<62))
	t.newest = sec
	t.closedBefore = 0
}

// FlushClosed sends buckets older than the close lag and drops them.
func (t *tallyWriter) FlushClosed() {
	if t.newest == 0 {
		return
	}
	cutoff := t.newest - tallyCloseLagSec
	if cutoff > t.closedBefore {
		t.closedBefore = cutoff
	}
	t.sink.Send(t.cache.TakeClosed(t.session, cutoff))
}

// Close sends every remaining bucket and gives the sink up to wait to deliver.
func (t *tallyWriter) Close(wait time.Duration) {
	if t.late > 0 {
		log.Printf("[photon-server] Tally dropped %d late frames (older than %ds behind newest)", t.late, tallyCloseLagSec)
	}
	if t.badClock > 0 {
		log.Printf("[photon-server] Tally dropped %d frames with a bad car clock", t.badClock)
	}
	t.sink.Send(t.cache.TakeClosed(t.session, 1<<62))
	t.sink.Close(wait)
}
