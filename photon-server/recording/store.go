package recording

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"server/wire"
	"sort"
	"sync"
	"time"
)

type Decode struct {
	ID          string `json:"id"`
	DBCHash     string `json:"dbc_hash"`
	Version     string `json:"version"`
	State       string `json:"state"`
	StartRecord uint64 `json:"start_record"`
	EndRecord   uint64 `json:"end_record,omitempty"`
	Checkpoint  uint64 `json:"checkpoint"`
	Error       string `json:"error,omitempty"`
	Scope       string `json:"scope"`
	MinMS       uint64 `json:"min_ms"`
	MaxMS       uint64 `json:"max_ms"`
}
type Manifest struct {
	ID            string   `json:"id"`
	Source        string   `json:"source"`
	Bus           string   `json:"bus"`
	MinMS         uint64   `json:"min_ms"`
	MaxMS         uint64   `json:"max_ms"`
	Records       uint64   `json:"records"`
	Raw           bool     `json:"has_raw"`
	DefaultDecode string   `json:"default_decode"`
	Decodes       []Decode `json:"decodes"`
	ImportedFrom  string   `json:"imported_from,omitempty"`
	Coarse        bool     `json:"coarse_history"`
}
type Entry struct {
	ID, TimeMS uint64
	Segment    uint32
	Offset     uint64
	Size       uint32
}
type Store struct {
	mu          sync.RWMutex
	root        string
	Meta        Manifest
	entries     []Entry
	timeEntries []Entry
	file        *os.File
	index       *os.File
	segment     uint32
	size        int64
}

const rotationBytes = 256 * 1024 * 1024

func Create(root, source, bus string) (*Store, error) {
	id := make([]byte, 16)
	if _, err := rand.Read(id); err != nil {
		return nil, err
	}
	m := Manifest{ID: hex.EncodeToString(id), Source: source, Bus: bus, Raw: true}
	dir := filepath.Join(root, "recordings", m.ID)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, err
	}
	if err := writeJSON(filepath.Join(dir, "manifest.json"), m); err != nil {
		return nil, err
	}
	return Open(dir)
}
func Open(dir string) (*Store, error) {
	s := &Store{root: dir}
	b, err := os.ReadFile(filepath.Join(dir, "manifest.json"))
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(b, &s.Meta); err != nil {
		return nil, err
	}
	paths, _ := filepath.Glob(filepath.Join(dir, "*.log"))
	sort.Strings(paths)
	for _, p := range paths {
		var segment uint32
		if _, err = fmt.Sscanf(filepath.Base(p), "%08d.log", &segment); err != nil {
			continue
		}
		if err = s.recover(p, segment); err != nil {
			return nil, err
		}
		if segment > s.segment {
			s.segment = segment
		}
	}
	if len(s.entries) > 0 {
		s.Meta.Records = uint64(len(s.entries))
		s.Meta.MinMS = s.entries[0].TimeMS
		s.Meta.MaxMS = s.Meta.MinMS
		for _, e := range s.entries {
			if e.TimeMS < s.Meta.MinMS {
				s.Meta.MinMS = e.TimeMS
			}
			if e.TimeMS > s.Meta.MaxMS {
				s.Meta.MaxMS = e.TimeMS
			}
		}
	}
	for i := range s.Meta.Decodes {
		d := &s.Meta.Decodes[i]
		if d.Scope != "epoch" {
			continue
		}
		d.MinMS = 0
		d.MaxMS = 0
		for _, e := range s.entries {
			if e.ID < d.StartRecord || d.EndRecord != 0 && e.ID > d.EndRecord {
				continue
			}
			if e.ID == d.StartRecord || e.TimeMS < d.MinMS {
				d.MinMS = e.TimeMS
			}
			if e.TimeMS > d.MaxMS {
				d.MaxMS = e.TimeMS
			}
		}
	}
	// The raw log is authoritative. Rebuild the timestamp index before opening append.
	var idx []byte
	for _, e := range s.entries {
		idx = append(idx, encodeEntry(e)...)
	}
	if err = atomicWrite(filepath.Join(dir, "time.idx"), idx); err != nil {
		return nil, err
	}
	s.index, err = os.OpenFile(filepath.Join(dir, "time.idx"), os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		return nil, err
	}
	s.segment++
	if err = s.openSegment(); err != nil {
		s.index.Close()
		return nil, err
	}
	if err = s.save(); err != nil {
		s.Close()
		return nil, err
	}
	return s, nil
}
func (s *Store) recover(path string, segment uint32) error {
	f, err := os.OpenFile(path, os.O_RDWR, 0644)
	if err != nil {
		return err
	}
	defer f.Close()
	var off int64
	for {
		h := make([]byte, 24)
		n, err := f.ReadAt(h, off)
		if n == 0 && errors.Is(err, io.EOF) {
			break
		}
		if n < 24 {
			break
		}
		size := binary.BigEndian.Uint32(h[4:])
		if binary.BigEndian.Uint32(h) != 0x56534431 || size > wire.MaxBatchBytes {
			return fmt.Errorf("corrupt raw header at %s:%d", path, off)
		}
		p := make([]byte, size)
		if _, err = f.ReadAt(p, off+24); err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			return err
		}
		if crc32.ChecksumIEEE(p) != binary.BigEndian.Uint32(h[20:]) {
			return fmt.Errorf("raw CRC mismatch at %s:%d", path, off)
		}
		if len(p) < 20 {
			return fmt.Errorf("short CANP payload")
		}
		id := binary.BigEndian.Uint64(h[8:])
		if id != uint64(len(s.entries))+1 {
			return fmt.Errorf("non-contiguous raw record IDs")
		}
		e := Entry{ID: id, TimeMS: binary.BigEndian.Uint64(p[12:]), Segment: segment, Offset: uint64(off), Size: 24 + size}
		s.entries = append(s.entries, e)
		off += int64(e.Size)
	}
	return f.Truncate(off)
}
func encodeEntry(e Entry) []byte {
	b := make([]byte, 32)
	binary.LittleEndian.PutUint64(b, e.ID)
	binary.LittleEndian.PutUint64(b[8:], e.TimeMS)
	binary.LittleEndian.PutUint32(b[16:], e.Segment)
	binary.LittleEndian.PutUint64(b[20:], e.Offset)
	binary.LittleEndian.PutUint32(b[28:], e.Size)
	return b
}
func (s *Store) openSegment() error {
	p := filepath.Join(s.root, fmt.Sprintf("%08d.log", s.segment))
	f, err := os.OpenFile(p, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	s.file = f
	s.size = 0
	return nil
}
func (s *Store) save() error { return writeJSON(filepath.Join(s.root, "manifest.json"), s.Meta) }
func (s *Store) Append(b wire.Batch) (Entry, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var e Entry
	if s.file == nil {
		return e, errors.New("recording closed")
	}
	if s.size+int64(24+len(b.Raw)) > rotationBytes {
		if err := s.file.Sync(); err != nil {
			return e, err
		}
		s.file.Close()
		s.segment++
		if err := s.openSegment(); err != nil {
			return e, err
		}
	}
	e = Entry{ID: uint64(len(s.entries)) + 1, TimeMS: b.TimeMS, Segment: s.segment, Offset: uint64(s.size), Size: uint32(24 + len(b.Raw))}
	h := make([]byte, 24)
	binary.BigEndian.PutUint32(h, 0x56534431)
	binary.BigEndian.PutUint32(h[4:], uint32(len(b.Raw)))
	binary.BigEndian.PutUint64(h[8:], e.ID)
	binary.BigEndian.PutUint32(h[16:], uint32(time.Now().Unix()))
	binary.BigEndian.PutUint32(h[20:], crc32.ChecksumIEEE(b.Raw))
	packet := append(h, b.Raw...)
	if n, err := s.file.Write(packet); err != nil || n != len(packet) {
		if err == nil {
			err = io.ErrShortWrite
		}
		return Entry{}, err
	}
	if err := s.file.Sync(); err != nil {
		return Entry{}, err
	}
	if _, err := s.index.Write(encodeEntry(e)); err != nil {
		return Entry{}, err
	}
	s.size += int64(e.Size)
	s.entries = append(s.entries, e)
	s.Meta.Records = e.ID
	if e.ID == 1 || b.TimeMS < s.Meta.MinMS {
		s.Meta.MinMS = b.TimeMS
	}
	if b.TimeMS > s.Meta.MaxMS {
		s.Meta.MaxMS = b.TimeMS
	}
	for i := range s.Meta.Decodes {
		d := &s.Meta.Decodes[i]
		if d.Scope == "epoch" && e.ID >= d.StartRecord && (d.EndRecord == 0 || e.ID <= d.EndRecord) {
			if e.ID == d.StartRecord || b.TimeMS < d.MinMS {
				d.MinMS = b.TimeMS
			}
			if b.TimeMS > d.MaxMS {
				d.MaxMS = b.TimeMS
			}
		}
	}
	return e, nil
}
func (s *Store) Read(id uint64) ([]byte, error) {
	s.mu.RLock()
	if id == 0 || id > uint64(len(s.entries)) {
		s.mu.RUnlock()
		return nil, io.EOF
	}
	e := s.entries[id-1]
	root := s.root
	s.mu.RUnlock()
	f, err := os.Open(filepath.Join(root, fmt.Sprintf("%08d.log", e.Segment)))
	if err != nil {
		return nil, err
	}
	defer f.Close()
	p := make([]byte, e.Size-24)
	_, err = f.ReadAt(p, int64(e.Offset)+24)
	return p, err
}
func (s *Store) Seek(at uint64) uint64 {
	ids := s.TimePage(at, 0, 1, 0, 1)
	if len(ids) > 0 {
		return ids[0]
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	return uint64(len(s.entries)) + 1
}

// TimePage seeks in the shared timestamp index. Each replay client retains only
// a bounded page; equal timestamps continue after the previous arrival ID.
func (s *Store) TimePage(at, afterID, startID, endID uint64, limit int) []uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.timeEntries) != len(s.entries) {
		s.timeEntries = append(s.timeEntries[:0], s.entries...)
		sort.Slice(s.timeEntries, func(i, j int) bool {
			a, b := s.timeEntries[i], s.timeEntries[j]
			if a.TimeMS == b.TimeMS {
				return a.ID < b.ID
			}
			return a.TimeMS < b.TimeMS
		})
	}
	if limit <= 0 || limit > 512 {
		limit = 512
	}
	i := sort.Search(len(s.timeEntries), func(i int) bool { e := s.timeEntries[i]; return e.TimeMS > at || e.TimeMS == at && e.ID > afterID })
	ids := make([]uint64, 0, limit)
	for ; i < len(s.timeEntries) && len(ids) < limit; i++ {
		e := s.timeEntries[i]
		if e.ID >= startID && (endID == 0 || e.ID <= endID) {
			ids = append(ids, e.ID)
		}
	}
	return ids
}

// RangeIDs orders records by event time, retaining the arrival ID as a tie breaker.
func (s *Store) RangeIDs(from, to, maxID uint64) []uint64 {
	s.mu.RLock()
	defer s.mu.RUnlock()
	var found []Entry
	for _, e := range s.entries {
		if e.TimeMS >= from && (to == 0 || e.TimeMS < to) && (maxID == 0 || e.ID <= maxID) {
			found = append(found, e)
		}
	}
	sort.SliceStable(found, func(i, j int) bool {
		if found[i].TimeMS == found[j].TimeMS {
			return found[i].ID < found[j].ID
		}
		return found[i].TimeMS < found[j].TimeMS
	})
	ids := make([]uint64, len(found))
	for i, e := range found {
		ids[i] = e.ID
	}
	return ids
}

func (s *Store) DecodeBounds(decodeID string) (uint64, uint64, uint64, uint64) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if decodeID == "" {
		decodeID = s.Meta.DefaultDecode
	}
	start, end := uint64(1), uint64(len(s.entries))
	minMS, maxMS := s.Meta.MinMS, s.Meta.MaxMS
	for _, d := range s.Meta.Decodes {
		if d.ID == decodeID && d.Scope == "epoch" {
			start = d.StartRecord
			if d.EndRecord != 0 {
				end = d.EndRecord
			}
			minMS, maxMS = d.MinMS, d.MaxMS
			break
		}
	}
	return start, end, minMS, maxMS
}
func (s *Store) Snapshot() Manifest {
	s.mu.RLock()
	defer s.mu.RUnlock()
	m := s.Meta
	m.Decodes = append([]Decode(nil), m.Decodes...)
	return m
}
func (s *Store) Update(fn func(*Manifest)) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	before := s.Meta
	before.Decodes = append([]Decode(nil), s.Meta.Decodes...)
	fn(&s.Meta)
	if err := s.save(); err != nil {
		s.Meta = before
		return err
	}
	return nil
}
func (s *Store) NewDecode(hash string, start uint64) (Decode, error) {
	return s.newDecode(hash, start, "recording")
}
func (s *Store) NewEpoch(hash string, start uint64) (Decode, error) {
	return s.newDecode(hash, start, "epoch")
}
func (s *Store) newDecode(hash string, start uint64, scope string) (Decode, error) {
	m := s.Snapshot()
	h := sha256.Sum256([]byte(fmt.Sprintf("%s:%s:%s:%s:%d", m.ID, hash, DecoderVersion, scope, start)))
	d := Decode{ID: hex.EncodeToString(h[:]), DBCHash: hash, Version: DecoderVersion, State: "pending", StartRecord: start, Scope: scope}
	err := s.Update(func(m *Manifest) {
		for _, old := range m.Decodes {
			if old.ID == d.ID {
				d = old
				return
			}
		}
		m.Decodes = append(m.Decodes, d)
	})
	return d, err
}
func (s *Store) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.file == nil {
		return nil
	}
	err := s.save()
	if e := s.file.Close(); err == nil {
		err = e
	}
	s.file = nil
	if e := s.index.Close(); err == nil {
		err = e
	}
	return err
}
func Discover(root string) (map[string]*Store, error) {
	stores := make(map[string]*Store)
	paths, _ := filepath.Glob(filepath.Join(root, "recordings", "*", "manifest.json"))
	for _, p := range paths {
		s, err := Open(filepath.Dir(p))
		if err != nil {
			for _, v := range stores {
				v.Close()
			}
			return nil, err
		}
		stores[s.Meta.ID] = s
	}
	return stores, nil
}
