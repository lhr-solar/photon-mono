package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"server/cache"
	"server/recording"
	"server/wire"
	"strconv"
)

// Event marks retain frame time and raw value, independently of plot resolution.
type eventMark = recording.Event

func (c *browserClient) eventControl(q browserControl) error {
	id := q.SessionID
	if id == "" {
		id = c.session
	}
	if c.service.recordings.Store(id) == nil {
		return fmt.Errorf("unknown recording")
	}
	c.service.eventMu.Lock()
	defer c.service.eventMu.Unlock()
	path := filepath.Join(c.service.cfg.DataDir, "recordings", id, "events.jsonl")
	if q.Type == "event_list" {
		data, err := os.ReadFile(path)
		if os.IsNotExist(err) {
			data = nil
			err = nil
		}
		if err != nil {
			return err
		}
		var rows []eventMark
		for _, line := range splitJSONLines(data) {
			var e eventMark
			if json.Unmarshal(line, &e) == nil && e.Time >= q.From && (q.To == 0 || e.Time < q.To) {
				rows = append(rows, e)
			}
		}
		return c.sendJSON(map[string]any{"type": "event_result", "events": rows})
	}
	if q.Type == "threshold_set" {
		var rule thresholdRule
		if err := json.Unmarshal(q.Raw, &rule); err != nil {
			return err
		}
		if err := c.service.recordings.setThreshold(rule); err != nil {
			return err
		}
		return c.sendJSON(map[string]any{"type": "threshold_status", "threshold": rule})
	}
	var e eventMark
	if err := json.Unmarshal(q.Raw, &e); err != nil {
		return err
	}
	e.Session = id
	e.Source = "manual"
	if e.Decode == "" {
		e.Decode = c.decode
	}
	if e.Time == 0 {
		e.Time = c.cursor
	}
	if len(e.Label) > 1024 || len(e.Color) > 64 {
		return fmt.Errorf("event label too long")
	}
	if err := c.service.recordings.Store(id).AddEvent(e); err != nil {
		return err
	}
	return c.sendJSON(map[string]any{"type": "event_created", "event": e})
}
func splitJSONLines(b []byte) [][]byte {
	var out [][]byte
	start := 0
	for i, c := range b {
		if c == '\n' {
			out = append(out, b[start:i])
			start = i + 1
		}
	}
	return out
}

type thresholdRule struct {
	ID       string  `json:"id"`
	SourceID string  `json:"source_id"`
	Signal   string  `json:"signal"`
	Tap      *int32  `json:"tap,omitempty"`
	Operator string  `json:"operator"`
	Value    float64 `json:"value"`
	Label    string  `json:"label"`
	Color    string  `json:"color"`
	Enabled  bool    `json:"enabled"`
}

func (m *recordingService) setThreshold(rule thresholdRule) error {
	if rule.ID == "" || len(rule.ID) > 128 || len(rule.Label) > 1024 || len(rule.Color) > 64 {
		return fmt.Errorf("invalid threshold identity or label")
	}
	if math.IsNaN(rule.Value) || math.IsInf(rule.Value, 0) {
		return fmt.Errorf("non-finite threshold")
	}
	switch rule.Operator {
	case ">", ">=", "<", "<=", "==":
	default:
		return fmt.Errorf("unsupported threshold operator")
	}
	m.thresholdMu.Lock()
	defer m.thresholdMu.Unlock()
	if len(m.thresholds) >= 256 {
		if _, ok := m.thresholds[rule.ID]; !ok {
			return fmt.Errorf("threshold limit reached")
		}
	}
	old, existed := m.thresholds[rule.ID]
	m.thresholds[rule.ID] = rule
	if err := recording.SaveJSON(filepath.Join(m.root, "thresholds.json"), m.thresholds); err != nil {
		if existed {
			m.thresholds[rule.ID] = old
		} else {
			delete(m.thresholds, rule.ID)
		}
		return err
	}
	return nil
}
func (m *recordingService) evaluateThresholds(s *recording.Store, meta recording.Manifest, defs *cache.SessionCache, b wire.Batch) {
	if defs == nil {
		return
	}
	m.thresholdMu.Lock()
	defer m.thresholdMu.Unlock()
	for _, f := range b.Frames {
		for _, v := range defs.Decode(f) {
			for _, rule := range m.thresholds {
				if !rule.Enabled || rule.SourceID != "" && rule.SourceID != meta.Source || rule.Signal != v.Def.Message+"."+v.Def.ID.Name || rule.Tap != nil && *rule.Tap != v.Tap {
					continue
				}
				trigger := false
				switch rule.Operator {
				case ">":
					trigger = v.Physical > rule.Value
				case ">=":
					trigger = v.Physical >= rule.Value
				case "<":
					trigger = v.Physical < rule.Value
				case "<=":
					trigger = v.Physical <= rule.Value
				case "==":
					trigger = v.Physical == rule.Value
				}
				key := fmt.Sprintf("%s:%s:%d", meta.ID, rule.ID, v.Tap)
				if trigger && !m.thresholdState[key] {
					value := strconv.FormatInt(v.Raw, 10)
					if v.Def.ValueType != 0 {
						value = strconv.FormatFloat(v.Numeric, 'g', -1, 64)
					}
					_ = s.AddEvent(recording.Event{Decode: meta.DefaultDecode, Time: f.TimeMs, Signal: rule.Signal, Tap: v.Tap, Source: "threshold", Label: rule.Label, Color: rule.Color, Value: value, Condition: fmt.Sprintf("%s %g", rule.Operator, rule.Value)})
				}
				m.thresholdState[key] = trigger
			}
		}
	}
}
