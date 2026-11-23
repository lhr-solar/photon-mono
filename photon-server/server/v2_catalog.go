package main

import (
	"encoding/json"
	"log"

	"server/dbc"
)

// v2CatalogMsg is the catalog control message of protocol v2.
type v2CatalogMsg struct {
	Type     string             `json:"type"`
	SHA      string             `json:"sha"`
	Buses    []string           `json:"buses"`
	Messages []v2CatalogMessage `json:"messages"`
}

type v2CatalogMessage struct {
	ID       uint32            `json:"id"`
	Name     string            `json:"name"`
	Bus      int               `json:"bus"`
	Length   uint8             `json:"length"`
	Selector int               `json:"selector"`
	Signals  []v2CatalogSignal `json:"signals"`
}

type v2CatalogSignal struct {
	Index        int     `json:"index"`
	Name         string  `json:"name"`
	Start        uint16  `json:"start"`
	Bits         uint8   `json:"bits"`
	LittleEndian bool    `json:"little_endian"`
	Signed       bool    `json:"signed"`
	Kind         string  `json:"kind"`
	Factor       float64 `json:"factor"`
	Offset       float64 `json:"offset"`
	Unit         string  `json:"unit"`
	Mux          int16   `json:"mux"`
}

// catalogJSON encodes c as the catalog message. Messages and their signals keep
// the order of c, and a signal's index counts over all of them, so the index is
// the signal's position in the catalog. A message's selector is the position of
// its mux selector in its own signals, as dbc.Message gives it. It returns nil,
// after logging why, when c cannot be encoded (a factor or offset that is not a
// finite number).
func catalogJSON(c *dbc.Catalog) []byte {
	msg := v2CatalogMsg{
		Type:     "catalog",
		SHA:      c.SHA,
		Buses:    append([]string{}, c.Buses...),
		Messages: make([]v2CatalogMessage, 0, len(c.Messages)),
	}
	index := 0
	for _, m := range c.Messages {
		cm := v2CatalogMessage{
			ID:       m.ID,
			Name:     m.Name,
			Bus:      busIndex(c.Buses, m.Bus),
			Length:   m.Length,
			Selector: m.Selector,
			Signals:  make([]v2CatalogSignal, 0, len(m.Signals)),
		}
		for i := range m.Signals {
			sig := &m.Signals[i]
			cm.Signals = append(cm.Signals, v2CatalogSignal{
				Index:        index,
				Name:         sig.Name,
				Start:        sig.Start,
				Bits:         sig.Bits,
				LittleEndian: sig.LittleEndian,
				Signed:       sig.Signed,
				Kind:         kindName(sig.Kind),
				Factor:       sig.Factor,
				Offset:       sig.Offset,
				Unit:         sig.Unit,
				Mux:          sig.Mux,
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

// kindName is the catalog's name for a signal kind.
func kindName(k dbc.Kind) string {
	switch k {
	case dbc.Integer:
		return "int"
	case dbc.Float32:
		return "f32"
	case dbc.Float64:
		return "f64"
	}
	return "unknown"
}
