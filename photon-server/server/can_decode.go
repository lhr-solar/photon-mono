package main

import (
	"encoding/binary"
	"fmt"
	"log"
	"strings"
	"sync"
	"sync/atomic"

	"server/dbc"
)

// canDecoder decodes the frames in ingested batches. appendLoop owns it, so
// only that goroutine touches the Decoder.
type canDecoder struct {
	dec       *dbc.Decoder
	published dbc.Stats // what decode has already added to canStats
}

// newCANDecoder loads the DBCs in cfg. Rows have nowhere to go until the DB
// writer exists; the Decoder's Stats count them.
func newCANDecoder(cfg CANConfig) (*canDecoder, error) {
	catalog, err := dbc.Load(cfg.DBCDir, cfg.SampleRates)
	if err != nil {
		return nil, err
	}
	log.Printf("[photon-server] CAN decoding %d messages from %s", len(catalog.Messages), cfg.DBCDir)
	return &canDecoder{dec: dbc.NewDecoder(catalog, discardRows{})}, nil
}

// startCANDecoder loads the DBCs when cfg.Decode is on. It returns nil, with a
// log line, when decoding is off or the DBCs do not load; ingest carries on
// either way.
func startCANDecoder(cfg CANConfig) *canDecoder {
	if !cfg.Decode {
		return nil
	}
	can, err := newCANDecoder(cfg)
	if err != nil {
		log.Printf("[photon-server] CAN decoding off: %v", err)
		return nil
	}
	canStats.enabled.Store(true)
	return can
}

// decode handles one pool buffer of whole batches, then adds the new counts to
// canStats. They add up across decoders, so turning decoding off and on again
// keeps the totals.
func (c *canDecoder) decode(buf []byte) {
	walkCANP3(buf, c.dec.Decode)
	s, p := c.dec.Stats, c.published
	canStats.frames.Add(s.Frames - p.Frames)
	canStats.rows.Add(s.Rows - p.Rows)
	canStats.unknownID.Add(s.UnknownID - p.UnknownID)
	canStats.short.Add(s.Short - p.Short)
	canStats.unknownMux.Add(s.UnknownMux - p.UnknownMux)
	c.published = s
}

// canSettings is the CAN configuration the running server uses. The observer
// changes it; appendLoop picks up each new decoder between buffers.
type canSettings struct {
	mu   sync.Mutex
	cfg  CANConfig
	swap chan *canDecoder // the next decoder for appendLoop; nil turns decoding off
}

// newCANSettings starts from cfg and the decoder startCANDecoder made. When the
// DBCs failed to load, decoding is reported as off.
func newCANSettings(cfg CANConfig, running *canDecoder) *canSettings {
	cfg.Decode = running != nil
	return &canSettings{cfg: cfg, swap: make(chan *canDecoder, 1)}
}

// canChange is a request to change some CAN settings; nil fields stay as they are.
type canChange struct {
	Decode *bool   `json:"decode,omitempty"`
	DBCDir *string `json:"dbc_dir,omitempty"`
}

// current is a copy of the settings in use.
func (s *canSettings) current() CANConfig {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.cfg
}

// apply makes a change, or returns why it cannot without changing anything. A
// change that turns decoding on, or keeps it on, loads the DBCs first, so a bad
// folder never reaches appendLoop.
func (s *canSettings) apply(change canChange) (CANConfig, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	next := s.cfg
	if change.Decode != nil {
		next.Decode = *change.Decode
	}
	if change.DBCDir != nil {
		next.DBCDir = strings.TrimSpace(*change.DBCDir)
	}
	if next.Decode && next.DBCDir == "" {
		return s.cfg, fmt.Errorf("no DBC directory set")
	}

	var can *canDecoder
	if next.Decode {
		var err error
		if can, err = newCANDecoder(next); err != nil {
			return s.cfg, err
		}
	}
	// Replace a decoder appendLoop has not picked up yet; only the latest counts.
	select {
	case <-s.swap:
	default:
	}
	s.swap <- can
	s.cfg = next
	canStats.enabled.Store(next.Decode)
	log.Printf("[photon-server] CAN settings changed: decode=%v dbc_dir=%q", next.Decode, next.DBCDir)
	return next, nil
}

// canStats is what the observer shows of decoding. appendLoop writes it after
// each buffer; the metadata writer reads it.
var canStats struct {
	enabled    atomic.Bool
	frames     atomic.Uint64
	rows       atomic.Uint64
	unknownID  atomic.Uint64
	short      atomic.Uint64
	unknownMux atomic.Uint64
}

const (
	canpRecordSize = 29     // can_id u32, dlc u8, data[8], dt u16[8]
	canpBus0Bit    = 0x1000 // set on standard IDs from can0 (and GPS); clear on can1
	canpExtended   = 0x80000000
)

// walkCANP3 calls emit for every frame in buf, a run of whole CANP v3 batches as
// canpReader submits them. This is the one part that knows the v3 record
// layout; the sender's next format replaces it.
//
// The v3 sender marks can0 by setting bit 12 of a standard ID, and sets bit 31
// on extended IDs, which say nothing of their bus. Each record's dt[0] is its
// age in milliseconds before the batch timestamp.
func walkCANP3(buf []byte, emit func(id uint32, bus string, data []byte, tNS int64)) {
	for len(buf) >= canpHeaderSize {
		n, err := batchLength(buf)
		if err != nil || n > len(buf) {
			// canpReader only submits whole batches; anything else is a bug upstream.
			log.Printf("[photon-server] CAN decode: malformed batch in a submitted buffer (%d bytes left)", len(buf))
			return
		}
		tsMS := binary.BigEndian.Uint64(buf[12:20])
		for rec := buf[canpHeaderSize:n]; len(rec) >= canpRecordSize; rec = rec[canpRecordSize:] {
			id := binary.BigEndian.Uint32(rec[0:4])
			dlc := min(int(rec[4]), 8)
			age := uint64(binary.BigEndian.Uint16(rec[13:15]))

			bus := "can1"
			switch {
			case id&canpExtended != 0:
				bus = "can"
			case id&canpBus0Bit != 0:
				bus = "can0"
				id &^= canpBus0Bit
			}
			t := tsMS
			if age <= t {
				t -= age
			}
			emit(id, bus, rec[5:5+dlc], int64(t)*1_000_000)
		}
		buf = buf[n:]
	}
}

type discardRows struct{}

func (discardRows) Row(string, string, int64, []string, []float64) {}
func (discardRows) Raw(string, uint32, []byte, int64, string)      {}
