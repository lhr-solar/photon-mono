package recording

import (
	"encoding/json"
	"os"
	"path/filepath"
)

type Event struct {
	Session   string `json:"session"`
	Decode    string `json:"decode_id"`
	Time      uint64 `json:"time_ms"`
	Signal    string `json:"signal"`
	Tap       int32  `json:"tap"`
	Source    string `json:"source"`
	Label     string `json:"label"`
	Color     string `json:"color"`
	Value     string `json:"raw_value"`
	End       uint64 `json:"end_ms,omitempty"`
	Condition string `json:"condition,omitempty"`
}

func (s *Store) AddEvent(e Event) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	f, err := os.OpenFile(filepath.Join(s.root, "events.jsonl"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	defer f.Close()
	e.Session = s.Meta.ID
	if err = json.NewEncoder(f).Encode(e); err != nil {
		return err
	}
	return f.Sync()
}
func SaveJSON(path string, v any) error { return writeJSON(path, v) }
