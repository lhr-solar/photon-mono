package recording

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"server/cache"
	"sort"
	"strings"
	"sync"
)

const DecoderVersion = "go-can-1"

type DBC struct {
	Hash        string              `json:"hash"`
	Label       string              `json:"label"`
	Alias       string              `json:"sha1"`
	Signals     int                 `json:"signals"`
	Diagnostics []string            `json:"diagnostics"`
	Defs        *cache.SessionCache `json:"-"`
	Supported   []DBCSignal         `json:"supported_signals"`
}
type DBCSignal struct {
	ID        uint32 `json:"id"`
	Message   string `json:"message"`
	Name      string `json:"name"`
	Unit      string `json:"unit"`
	ValueType int    `json:"value_type"`
}
type Registry struct {
	mu    sync.RWMutex
	dir   string
	files map[string]DBC
}

func OpenRegistry(root string) (*Registry, error) {
	r := &Registry{dir: filepath.Join(root, "dbcs"), files: make(map[string]DBC)}
	if err := os.MkdirAll(r.dir, 0755); err != nil {
		return nil, err
	}
	paths, _ := filepath.Glob(filepath.Join(r.dir, "*.dbc"))
	for _, p := range paths {
		b, err := os.ReadFile(p)
		if err != nil {
			return nil, err
		}
		d, err := r.register(filepath.Base(p), b, false)
		if err != nil {
			return nil, err
		}
		var meta DBC
		if b, err := os.ReadFile(strings.TrimSuffix(p, ".dbc") + ".json"); err == nil && json.Unmarshal(b, &meta) == nil {
			d.Label = meta.Label
		}
		r.files[d.Hash] = d
	}
	return r, nil
}
func (r *Registry) Register(label string, b []byte) (DBC, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.register(label, b, true)
}
func (r *Registry) register(label string, b []byte, persist bool) (DBC, error) {
	if len(b) > 4*1024*1024 {
		return DBC{}, fmt.Errorf("DBC exceeds 4 MiB")
	}
	defs, diagnostics, err := cache.ValidateDBC(string(b))
	if err != nil {
		return DBC{Diagnostics: diagnostics}, err
	}
	hash := sha256.Sum256(b)
	id := hex.EncodeToString(hash[:])
	if d, ok := r.files[id]; ok {
		return d, nil
	}
	d := DBC{Hash: id, Label: filepath.Base(label), Alias: cache.SHA1Hex(b), Signals: len(defs.Definitions), Defs: defs}
	for _, signal := range defs.SortedDefinitions() {
		d.Supported = append(d.Supported, DBCSignal{signal.Number, signal.Message, signal.ID.Name, signal.Unit, signal.ValueType})
	}
	defs.DBCSHA = id
	if persist {
		if err := atomicWrite(filepath.Join(r.dir, id+".dbc"), b); err != nil {
			return DBC{}, err
		}
		if err := writeJSON(filepath.Join(r.dir, id+".json"), d); err != nil {
			return DBC{}, err
		}
	}
	r.files[id] = d
	return d, nil
}
func (r *Registry) Get(id string) (DBC, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if d, ok := r.files[id]; ok {
		return d, true
	}
	for _, d := range r.files {
		if id == d.Alias {
			return d, true
		}
	}
	var candidate DBC
	found := false
	for _, d := range r.files {
		if id == d.Label || strings.TrimSuffix(d.Label, ".dbc") == id {
			if found {
				return DBC{}, false
			}
			candidate = d
			found = true
		}
	}
	return candidate, found
}
func (r *Registry) List() []DBC {
	r.mu.RLock()
	defer r.mu.RUnlock()
	var out []DBC
	for _, d := range r.files {
		out = append(out, d)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Label == out[j].Label {
			return out[i].Hash < out[j].Hash
		}
		return out[i].Label < out[j].Label
	})
	return out
}
func atomicWrite(path string, b []byte) error {
	f, err := os.CreateTemp(filepath.Dir(path), ".pending-*")
	if err != nil {
		return err
	}
	name := f.Name()
	defer os.Remove(name)
	if _, err = f.Write(b); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err == nil {
		err = closeErr
	}
	if err != nil {
		return err
	}
	return os.Rename(name, path)
}
func writeJSON(path string, v any) error {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	return atomicWrite(path, b)
}
