package main

import (
	"encoding/json"
	"log"

	"server/dbc"
)

// v2CatalogMsg is the catalog control message of protocol v2. It is for display
// only: the client never decodes with it.
type v2CatalogMsg struct {
	Type     string             `json:"type"`
	SHA      string             `json:"sha"`
	Buses    []string           `json:"buses"`
	Messages []v2CatalogMessage `json:"messages"`
}

type v2CatalogMessage struct {
	ID      uint32            `json:"id"`
	Name    string            `json:"name"`
	Bus     int               `json:"bus"`
	Signals []v2CatalogSignal `json:"signals"`
}

type v2CatalogSignal struct {
	Index   int     `json:"index"`
	Name    string  `json:"name"`
	Unit    string  `json:"unit"`
	Min     float64 `json:"min"`
	Max     float64 `json:"max"`
	Integer bool    `json:"integer"`
	Mux     int16   `json:"mux"`
}

// catalogJSON encodes c as the catalog message. Messages and their signals keep
// the order of c, and a signal's index counts over all of them, so the index is
// the signal's position in the catalog. A message's bus is its index in buses. A
// signal's range is the DBC's; one that is not a finite number is sent as 0 to 0,
// with a log line. integer says the scaled value is always a whole number. It
// returns nil, after logging why, when c cannot be encoded.
func catalogJSON(c *dbc.Catalog) []byte {
	msg := v2CatalogMsg{
		Type:     "catalog",
		SHA:      c.SHA,
		Buses:    append([]string{}, c.Buses...),
		Messages: make([]v2CatalogMessage, 0, len(c.Messages)),
	}
	index := 0
	rangeLogged := false
	for _, m := range c.Messages {
		cm := v2CatalogMessage{
			ID:      m.ID,
			Name:    m.Name,
			Bus:     busIndex(c.Buses, m.Bus),
			Signals: make([]v2CatalogSignal, 0, len(m.Signals)),
		}
		for i := range m.Signals {
			sig := &m.Signals[i]
			lo, hi := sig.Min, sig.Max
			if !finite(lo) || !finite(hi) {
				if !rangeLogged {
					log.Printf("[photon-server] v2 catalog %s: %s.%s has a range that is not a number; sent as 0 to 0", c.SHA, m.Name, sig.Name)
					rangeLogged = true
				}
				lo, hi = 0, 0
			}
			cm.Signals = append(cm.Signals, v2CatalogSignal{
				Index:   index,
				Name:    sig.Name,
				Unit:    sig.Unit,
				Min:     lo,
				Max:     hi,
				Integer: sig.Kind == dbc.Integer && sig.Factor == 1 && sig.Offset == 0,
				Mux:     sig.Mux,
			})
			index++
		}
		msg.Messages = append(msg.Messages, cm)
	}
	out, err := json.Marshal(msg)
	if err != nil {
		log.Printf("[photon-server] v2 catalog %s not encoded: %v", c.SHA, err)
		return nil
	}
	return out
}

// catalogSignals lists the signals of c in catalog index order: the order
// catalogJSON gives them, so position i is the signal whose index is i.
func catalogSignals(c *dbc.Catalog) []tileSignal {
	var out []tileSignal
	for _, m := range c.Messages {
		for i := range m.Signals {
			out = append(out, tileSignal{msg: m, sig: &m.Signals[i]})
		}
	}
	return out
}

// busIndex is the position of bus in buses, or -1 when it is not there.
func busIndex(buses []string, bus string) int {
	for i, b := range buses {
		if b == bus {
			return i
		}
	}
	return -1
}
