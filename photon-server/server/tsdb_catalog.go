package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"server/cache"
	"sort"
	"strings"
	"sync"
	"time"
)

type tsdbSession struct {
	ID, DBC, Label, Dir string
	Min, Max            uint64
	Samples             uint64
}

var tsdbCatalogCache = struct {
	sync.Mutex
	key  string
	mod  time.Time
	rows []tsdbSession
}{}

func loadTsdbCatalog(dataDir string) []tsdbSession {
	root := filepath.Join(dataDir, "tsdb")
	path := filepath.Join(root, "catalog.json")
	st, statErr := os.Stat(path)
	var mod time.Time
	if statErr == nil {
		mod = st.ModTime()
	}
	tsdbCatalogCache.Lock()
	if tsdbCatalogCache.key == path && tsdbCatalogCache.mod.Equal(mod) && tsdbCatalogCache.rows != nil {
		rows := tsdbCatalogCache.rows
		tsdbCatalogCache.Unlock()
		return rows
	}
	tsdbCatalogCache.Unlock()
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var file struct {
		Sessions []struct {
			ID   string `json:"id"`
			DBC  string `json:"dbc_sha"`
			Min  uint64 `json:"min_ms"`
			Max  uint64 `json:"max_ms"`
			Path string `json:"path"`
		} `json:"sessions"`
	}
	if json.Unmarshal(data, &file) != nil {
		return nil
	}
	var out []tsdbSession
	for _, s := range file.Sessions {
		if s.ID == "" || s.Max < s.Min {
			continue
		}
		dir, err := tsdbSessionDir(root, s.ID, s.Path)
		if err != nil {
			continue
		}
		samples := uint64(0)
		label := ""
		dbc := s.DBC
		if meta, err := os.ReadFile(filepath.Join(dir, "session.json")); err == nil {
			var m struct {
				Samples uint64 `json:"samples"`
				DBC     string `json:"dbc_sha"`
				Label   string `json:"dbc_label"`
			}
			if json.Unmarshal(meta, &m) == nil {
				samples = m.Samples
				label = m.Label
				if dbc == "" {
					dbc = m.DBC
				}
			}
		}
		out = append(out, tsdbSession{ID: s.ID, DBC: dbc, Label: label, Dir: dir, Min: s.Min, Max: s.Max, Samples: samples})
	}
	if out == nil {
		out = []tsdbSession{}
	}
	tsdbCatalogCache.Lock()
	tsdbCatalogCache.key = path
	tsdbCatalogCache.mod = mod
	tsdbCatalogCache.rows = out
	tsdbCatalogCache.Unlock()
	return out
}

// videoStreamGroups splits a pointer table into per-stream runs. snapshot()
// already sorts by (stream, start).
func videoStreamGroups(rows []GopRow) [][]GopRow {
	if len(rows) == 0 {
		return nil
	}
	var groups [][]GopRow
	for i := 0; i < len(rows); {
		j := i + 1
		for j < len(rows) && rows[j].StreamID == rows[i].StreamID {
			j++
		}
		groups = append(groups, rows[i:j])
		i = j
	}
	return groups
}

func groupsOverlap(groups [][]GopRow, offset, lo, hi int64) bool {
	if hi < lo {
		return false
	}
	for _, group := range groups {
		k := sort.Search(len(group), func(n int) bool {
			return int64(group[n].StartMs)+offset > hi
		})
		if k == 0 {
			continue
		}
		row := group[k-1]
		start := int64(row.StartMs) + offset
		end := int64(row.EndMs) + offset
		if start <= hi && end >= lo {
			return true
		}
	}
	return false
}

func tsdbSessionDir(root, id, path string) (string, error) {
	if id == "" || strings.ContainsAny(id, `/\`) {
		return "", fmt.Errorf("bad session id")
	}
	root = filepath.Clean(root)
	try := []string{filepath.Clean(path), filepath.Join(root, id)}
	for _, dir := range try {
		if dir == "" || dir == "." {
			continue
		}
		rel, err := filepath.Rel(root, dir)
		if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
			continue
		}
		if st, err := os.Stat(filepath.Join(dir, "session.json")); err == nil && !st.IsDir() {
			return dir, nil
		}
	}
	return "", fmt.Errorf("missing session.json")
}

func findTsdbSession(dataDir, id string) (tsdbSession, bool) {
	for _, s := range loadTsdbCatalog(dataDir) {
		if s.ID == id {
			return s, true
		}
	}
	return tsdbSession{}, false
}

func sessionDate(id string) string {
	if len(id) < 8 {
		return ""
	}
	for _, c := range id[:8] {
		if c < '0' || c > '9' {
			return ""
		}
	}
	return id[:4] + "-" + id[4:6] + "-" + id[6:8]
}

func (c *browserClient) appendTsdbSessions(rows []map[string]any, videoGroups [][]GopRow, videoOffset int64) []map[string]any {
	have := map[string]bool{}
	for _, row := range rows {
		id, _ := row["id"].(string)
		if id != "" {
			have[id] = true
		}
	}
	for _, s := range loadTsdbCatalog(c.service.cfg.DataDir) {
		if have[s.ID] {
			continue
		}
		hasVideo := groupsOverlap(videoGroups, videoOffset, int64(s.Min), int64(s.Max))
		rows = append(rows, map[string]any{
			"id": s.ID, "path": s.ID, "min_ms": s.Min, "max_ms": s.Max, "dbc_hash": s.DBC,
			"sample_count": s.Samples, "has_video": hasVideo, "decode_id": "", "decodes": []any{},
			"source": "tsdb", "has_raw": true,
		})
	}
	return rows
}

func driveDays(dataDir string) []map[string]any {
	type day struct {
		date     string
		sessions []tsdbSession
	}
	byDate := map[string]*day{}
	var order []string
	for _, s := range loadTsdbCatalog(dataDir) {
		date := sessionDate(s.ID)
		if date == "" {
			continue
		}
		d := byDate[date]
		if d == nil {
			d = &day{date: date}
			byDate[date] = d
			order = append(order, date)
		}
		d.sessions = append(d.sessions, s)
	}
	sort.Strings(order)
	out := make([]map[string]any, 0, len(order))
	for _, date := range order {
		d := byDate[date]
		sort.Slice(d.sessions, func(i, j int) bool { return d.sessions[i].Min < d.sessions[j].Min })
		var duration float64
		sessions := make([]map[string]any, 0, len(d.sessions))
		for _, s := range d.sessions {
			sec := float64(s.Max-s.Min) / 1000
			duration += sec
			sessions = append(sessions, map[string]any{
				"id": s.ID, "path": s.Dir, "start_ms": s.Min, "duration_s": sec, "speed_max_mps": 0,
			})
		}
		out = append(out, map[string]any{
			"date": date, "path": date, "session_count": len(sessions), "duration_s": duration, "sessions": sessions,
		})
	}
	return out
}

func loadTsdbSignals(dir string) ([]map[string]any, error) {
	data, err := os.ReadFile(filepath.Join(dir, "signals.json"))
	if err != nil {
		return nil, err
	}
	var rows []struct {
		ID      uint32  `json:"id"`
		Message string  `json:"message"`
		Name    string  `json:"name"`
		Unit    string  `json:"unit"`
		Min     float64 `json:"min"`
		Max     float64 `json:"max"`
	}
	if err = json.Unmarshal(data, &rows); err != nil {
		return nil, err
	}
	out := make([]map[string]any, 0, len(rows))
	for _, row := range rows {
		if row.ID == 0 || row.Name == "" {
			continue
		}
		out = append(out, map[string]any{
			"id": row.ID, "message": row.Message, "name": row.Name, "unit": row.Unit, "min": row.Min, "max": row.Max,
		})
	}
	return out, nil
}

// canIDsByMessage maps a DBC message name to its CAN id. signals.json stores
// the name and not the id, and the page groups the CAN screen by id. Without
// this, every saved-run signal lands in one 0x000 bucket named after whichever
// row sorts first.
func canIDsByMessage(defs *cache.SessionCache) map[string]uint32 {
	out := map[string]uint32{}
	if defs == nil {
		return out
	}
	for _, d := range defs.SortedDefinitions() {
		if _, ok := out[d.Message]; ok || d.Message == "" {
			continue
		}
		out[d.Message] = d.ID.CANID
	}
	return out
}

func attachMessageIDs(signals []map[string]any, byMessage map[string]uint32) {
	for _, row := range signals {
		name, _ := row["message"].(string)
		id, ok := byMessage[name]
		if !ok {
			continue
		}
		row["message_id"] = id
	}
}

func (c *browserClient) sessionDefs(sess tsdbSession) *cache.SessionCache {
	reg := c.service.recordings.registry
	if reg == nil {
		return nil
	}
	if sess.DBC != "" {
		if d, ok := reg.Get(sess.DBC); ok && d.Defs != nil {
			return d.Defs
		}
	}
	if sess.Label != "" {
		if d, ok := reg.Get(sess.Label); ok && d.Defs != nil {
			return d.Defs
		}
	}
	if d, ok := reg.Get(c.service.recordings.DefaultDBC()); ok && d.Defs != nil {
		return d.Defs
	}
	listed := reg.List()
	if len(listed) == 1 && listed[0].Defs != nil {
		return listed[0].Defs
	}
	return nil
}

func (c *browserClient) selectTsdb(sess tsdbSession) error {
	signals, err := loadTsdbSignals(sess.Dir)
	if err != nil {
		return err
	}
	attachMessageIDs(signals, canIDsByMessage(c.sessionDefs(sess)))
	c.session = sess.ID
	c.decode = ""
	c.defs = nil
	c.cursor = sess.Min
	c.tsdbMin = sess.Min
	c.tsdbMax = sess.Max
	c.tsdbDir = sess.Dir
	c.tsdbPlay = true
	c.mode = 1
	if err = c.sendJSON(map[string]any{"type": "signal_catalog", "signals": signals, "dbc_id": sess.DBC}); err != nil {
		return err
	}
	return c.sendJSON(map[string]any{
		"type": "recording_selected", "session_id": sess.ID, "decode_id": "", "dbc_hash": sess.DBC,
		"min_ms": sess.Min, "max_ms": sess.Max, "has_raw": true, "coarse_history": false,
	})
}
