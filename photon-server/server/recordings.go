package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"server/cache"
	"server/influx"
	"server/recording"
	"server/wire"
	"strings"
	"sync"
	"time"
)

type liveBatch struct {
	Session string
	Batch   wire.Batch
	Defs    *cache.SessionCache
}
type recordingService struct {
	mu             sync.RWMutex
	root           string
	registry       *recording.Registry
	influx         *influx.Client
	stores         map[string]*recording.Store
	active         map[string]string
	defaultDBC     string
	subs           map[chan liveBatch]bool
	jobs           map[string]context.CancelFunc
	workers        sync.WaitGroup
	ctx            context.Context
	cancel         context.CancelFunc
	thresholdMu    sync.Mutex
	thresholds     map[string]thresholdRule
	thresholdState map[string]bool
	rootLock       *os.File
}

func openRecordingService(cfg *Config) (*recordingService, error) {
	rootLock, err := recording.LockRoot(cfg.DataDir)
	if err != nil {
		return nil, err
	}
	success := false
	defer func() {
		if !success {
			rootLock.Close()
		}
	}()
	r, err := recording.OpenRegistry(cfg.DataDir) // dbc
	if err != nil {
		return nil, err
	}
	if cfg.DefaultDBCHash != "" {
		d, ok := r.Get(cfg.DefaultDBCHash)
		if !ok || d.Hash != cfg.DefaultDBCHash {
			return nil, fmt.Errorf("default DBC hash %q is not registered; upload the DBC first", cfg.DefaultDBCHash)
		}
	}
	stores, err := recording.Discover(cfg.DataDir) // logs
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithCancel(context.Background())
	m := &recordingService{root: cfg.DataDir, registry: r, stores: stores, active: make(map[string]string), subs: make(map[chan liveBatch]bool), jobs: make(map[string]context.CancelFunc), ctx: ctx, cancel: cancel}
	m.rootLock = rootLock
	m.defaultDBC = cfg.DefaultDBCHash
	m.thresholds = make(map[string]thresholdRule)
	m.thresholdState = make(map[string]bool)
	if data, err := os.ReadFile(filepath.Join(cfg.DataDir, "thresholds.json")); err == nil { // TODO
		if err = json.Unmarshal(data, &m.thresholds); err != nil {
			return nil, err
		}
	}
	if cfg.InfluxURL != "" && !strings.EqualFold(cfg.InfluxURL, "off") {
		token := os.Getenv("PHOTON_INFLUX_TOKEN")
		if cfg.InfluxTokenFile != "" {
			b, err := os.ReadFile(cfg.InfluxTokenFile)
			if err != nil {
				return nil, err
			}
			token = string(bytes.TrimSpace(b))
		}
		m.influx = influx.New(cfg.InfluxURL, cfg.InfluxDatabase, token)
	}
	for _, s := range stores {
		_ = s.Update(func(meta *recording.Manifest) {
			for i := range meta.Decodes {
				if meta.Decodes[i].State == "live" {
					meta.Decodes[i].Scope = "epoch"
					meta.Decodes[i].EndRecord = meta.Records
					meta.Decodes[i].State = "pending"
				}
			}
		})
		meta := s.Snapshot()
		if !meta.Raw {
			continue
		}
		for _, d := range meta.Decodes {
			if d.State == "live" || d.State == "pending" || d.State == "running" {
				m.startDecode(s, d, d.Scope != "epoch")
			}
		}
	}
	success = true
	return m, nil
}
func (m *recordingService) Close() {
	defer m.rootLock.Close()
	m.cancel()
	m.workers.Wait()
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, s := range m.stores {
		if err := s.Close(); err != nil {
			log.Printf("recording close: %v", err)
		}
	}
}
func (m *recordingService) Subscribe() chan liveBatch {
	ch := make(chan liveBatch, 64)
	m.mu.Lock()
	m.subs[ch] = true
	m.mu.Unlock()
	return ch
}
func (m *recordingService) Unsubscribe(ch chan liveBatch) {
	m.mu.Lock()
	delete(m.subs, ch)
	m.mu.Unlock()
}

func (m *recordingService) EndSource(source string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.stores[m.active[source]]
	delete(m.active, source)
	if s == nil {
		return
	}
	_ = s.Update(func(meta *recording.Manifest) {
		for i := range meta.Decodes {
			if meta.Decodes[i].State == "live" {
				meta.Decodes[i].EndRecord = meta.Records
				meta.Decodes[i].State = "pending"
			}
		}
	})
}
func (m *recordingService) Store(id string) *recording.Store {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.stores[id]
}

func (m *recordingService) IsActive(id string) bool {
	m.mu.RLock()
	defer m.mu.RUnlock()
	for _, active := range m.active {
		if active == id {
			return true
		}
	}
	return false
}
func (m *recordingService) DefaultDBC() string {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.defaultDBC
}
func (m *recordingService) Catalog() []recording.Manifest {
	m.mu.RLock()
	defer m.mu.RUnlock()
	var out []recording.Manifest
	for _, s := range m.stores {
		out = append(out, s.Snapshot())
	}
	return out
}
func (m *recordingService) Ingest(source string, b wire.Batch) error {
	m.mu.Lock()
	s := m.stores[m.active[source]]
	if s == nil {
		var err error
		s, err = recording.Create(m.root, source, source)
		if err != nil {
			m.mu.Unlock()
			return err
		}
		m.stores[s.Meta.ID] = s
		m.active[source] = s.Meta.ID
		if m.defaultDBC != "" {
			d, err := s.NewEpoch(m.defaultDBC, 1)
			if err != nil {
				m.mu.Unlock()
				return err
			}
			_ = s.Update(func(meta *recording.Manifest) {
				meta.DefaultDecode = d.ID
				for i := range meta.Decodes {
					if meta.Decodes[i].ID == d.ID {
						meta.Decodes[i].State = "live"
					}
				}
			})
			m.startDecodeLocked(s, d, false)
		}
	}
	if _, err := s.Append(b); err != nil {
		m.mu.Unlock()
		return err
	}
	meta := s.Snapshot()
	var defs *cache.SessionCache
	for _, d := range meta.Decodes {
		if d.ID == meta.DefaultDecode {
			if dbc, ok := m.registry.Get(d.DBCHash); ok {
				defs = dbc.Defs
			}
		}
	}
	for i := range b.Frames {
		b.Frames[i].Bus = meta.Bus
	}
	item := liveBatch{meta.ID, b, defs}
	m.evaluateThresholds(s, meta, defs, b)
	for ch := range m.subs {
		select {
		case ch <- item:
		default: /* A slow display drops live samples; the raw recording retains them. */
		}
	}
	m.mu.Unlock()
	return nil
}
func (m *recordingService) Apply(source, hash string) (recording.Decode, error) {
	dbc, ok := m.registry.Get(hash)
	if !ok {
		return recording.Decode{}, fmt.Errorf("unknown DBC")
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.stores[m.active[source]]
	if s == nil {
		if source == "" {
			m.defaultDBC = dbc.Hash
			return recording.Decode{DBCHash: dbc.Hash, State: "configured"}, nil
		}
		return recording.Decode{}, fmt.Errorf("source not active")
	}
	meta := s.Snapshot()
	start := meta.Records + 1
	if err := s.Update(func(meta *recording.Manifest) {
		for i := range meta.Decodes {
			if meta.Decodes[i].State == "live" {
				meta.Decodes[i].EndRecord = start - 1
				meta.Decodes[i].State = "pending"
			}
		}
	}); err != nil {
		return recording.Decode{}, err
	}
	d, err := s.NewEpoch(dbc.Hash, start)
	if err != nil {
		return d, err
	}
	err = s.Update(func(meta *recording.Manifest) {
		meta.DefaultDecode = d.ID
		for i := range meta.Decodes {
			if meta.Decodes[i].ID == d.ID {
				meta.Decodes[i].State = "live"
			}
		}
	})
	if err == nil {
		m.startDecodeLocked(s, d, false)
	}
	return d, err
}
func (m *recordingService) Redecode(id, hash string) (recording.Decode, error) {
	m.mu.RLock()
	active := false
	for _, recordingID := range m.active {
		if recordingID == id {
			active = true
			break
		}
	}
	m.mu.RUnlock()
	if active {
		return recording.Decode{}, fmt.Errorf("finish the active recording before historical re-decoding; applying a DBC live starts a new epoch")
	}
	s := m.Store(id)
	dbc, ok := m.registry.Get(hash)
	if s == nil || !ok {
		return recording.Decode{}, fmt.Errorf("unknown recording or DBC")
	}
	if !s.Snapshot().Raw {
		return recording.Decode{}, fmt.Errorf("this legacy recording has no original CAN frames and cannot be re-decoded")
	}
	d, err := s.NewDecode(dbc.Hash, 1)
	if err != nil {
		return d, err
	}
	if d.State == "complete" {
		return d, nil
	}
	m.startDecode(s, d, true)
	return d, nil
}
func (m *recordingService) startDecode(s *recording.Store, d recording.Decode, historical bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.startDecodeLocked(s, d, historical)
}
func (m *recordingService) startDecodeLocked(s *recording.Store, d recording.Decode, historical bool) {
	if _, ok := m.jobs[d.ID]; ok {
		return
	}
	ctx, cancel := context.WithCancel(m.ctx)
	m.jobs[d.ID] = cancel
	m.workers.Add(1)
	go func() {
		defer m.workers.Done()
		defer func() { m.mu.Lock(); delete(m.jobs, d.ID); m.mu.Unlock() }()
		m.runDecode(ctx, s, d, historical)
	}()
}
func (m *recordingService) CancelDecode(id string) {
	m.mu.RLock()
	cancel := m.jobs[id]
	m.mu.RUnlock()
	if cancel != nil {
		cancel()
	}
}
func (m *recordingService) runDecode(ctx context.Context, s *recording.Store, d recording.Decode, historical bool) {
	dbc, ok := m.registry.Get(d.DBCHash)
	if !ok {
		return
	}
	if historical {
		if m.influx == nil {
			_ = s.Update(func(meta *recording.Manifest) {
				for i := range meta.Decodes {
					if meta.Decodes[i].ID == d.ID {
						meta.Decodes[i].State = "failed"
						meta.Decodes[i].Error = "Influx is not configured"
					}
				}
			})
			return
		}
		for {
			_, err := recording.ProjectRecording(ctx, s, dbc, m.influx)
			if err == nil {
				return
			}
			if !influx.Retryable(err) {
				log.Printf("redecode %s: %v", d.ID, err)
				return
			}
			select {
			case <-ctx.Done():
				return
			case <-time.After(time.Second):
			}
		}
	}
	acc := dbc.Defs.Clone()
	meta := s.Snapshot()
	acc.SessionID = meta.ID
	acc.DecodeID = d.ID
	// Rebuild from the raw boundary on restart. Repeated Influx writes replace complete tallies.
	next := d.StartRecord
	if next == 0 {
		next = 1
	}
	var lastMS uint64
	bound := uint64(0)
	if historical {
		bound = meta.Records
	}
	state := "live"
	if historical {
		state = "running"
	}
	update := func(checkpoint uint64, st string, err error) {
		_ = s.Update(func(meta *recording.Manifest) {
			for i := range meta.Decodes {
				if meta.Decodes[i].ID == d.ID {
					v := &meta.Decodes[i]
					v.Checkpoint = checkpoint
					v.State = st
					if err != nil {
						v.Error = err.Error()
					} else {
						v.Error = ""
					}
					if st == "complete" && historical {
						meta.DefaultDecode = d.ID
					}
				}
			}
		})
	}
	update(0, state, nil)
	tick := time.NewTicker(200 * time.Millisecond)
	defer tick.Stop()
	initialized := false
	for {
		select {
		case <-ctx.Done():
			if m.ctx.Err() == nil {
				update(next-1, "cancelled", nil)
			}
			return
		case <-tick.C:
		}
		if m.influx == nil {
			update(next-1, state, fmt.Errorf("Influx is not configured; raw recording retained"))
			continue
		}
		if !initialized {
			lines := cache.SignalDefLines(acc, dbc.Hash, 1)
			if err := recording.InitializeInflux(ctx, m.influx); err != nil {
				update(0, state, err)
				continue
			}
			if err := m.influx.Write(ctx, lines); err != nil {
				update(0, state, err)
				continue
			}
			initialized = true
		}
		meta = s.Snapshot()
		end := meta.Records
		if bound != 0 && bound < end {
			end = bound
		}
		for _, v := range meta.Decodes {
			if v.ID == d.ID && v.EndRecord != 0 {
				bound = v.EndRecord
				if bound < end {
					end = bound
				}
			}
		}
		// Bound memory and request sizes by writing each time window before advancing.
		processed := 0
		for next <= end && processed < 512 {
			raw, err := s.Read(next)
			if err != nil {
				update(next-1, "failed", err)
				return
			}
			b, err := wire.ReadBatch(bytes.NewReader(raw))
			if err != nil {
				update(next-1, "failed", err)
				return
			}
			if lastMS != 0 && b.TimeMS/1000 > lastMS/1000 && acc.Len() > 0 {
				break
			}
			if lastMS != 0 && b.TimeMS/1000 < lastMS/1000 {
				sec := int64(b.TimeMS / 1000)
				for k := range acc.Samples {
					if k.Second == sec {
						delete(acc.Samples, k)
					}
				}
				for k := range acc.Floats {
					if k.Second == sec {
						delete(acc.Floats, k)
					}
				}
				for _, id := range s.RangeIDs(uint64(sec)*1000, uint64(sec+1)*1000, next) {
					if id < d.StartRecord {
						continue
					}
					p, err := s.Read(id)
					if err != nil {
						update(next-1, "failed", err)
						return
					}
					late, err := wire.ReadBatch(bytes.NewReader(p))
					if err != nil {
						update(next-1, "failed", err)
						return
					}
					for _, f := range late.Frames {
						f.Bus = meta.Bus
						acc.Fold(f)
					}
				}
			} else {
				for _, f := range b.Frames {
					f.Bus = meta.Bus
					acc.Fold(f)
				}
			}
			lastMS = b.TimeMS
			next++
			processed++
		}
		lines := acc.OpenLines(meta.ID)
		if err := m.influx.Write(ctx, lines); err != nil {
			update(next-1, state, err)
			continue
		}
		update(next-1, state, nil)
		if next <= end {
			raw, err := s.Read(next)
			if err == nil {
				b, err := wire.ReadBatch(bytes.NewReader(raw))
				if err == nil && b.TimeMS/1000 > lastMS/1000 {
					acc.DropClosed(int64(b.TimeMS / 1000))
				}
			}
		}
		if lastMS != 0 {
			acc.DropClosed(int64(lastMS / 1000))
		}
		if bound != 0 && next > bound || historical && next > end {
			update(next-1, "complete", nil)
			return
		}
	}
}
func scaleRows(rows []influx.Row, defs *cache.SessionCache) {
	lookup := make(map[string]cache.SignalDef)
	if defs != nil {
		for _, d := range defs.Definitions {
			lookup[d.Message+"."+d.ID.Name] = d
		}
	}
	for i := range rows {
		if d, ok := lookup[rows[i].Signal]; ok {
			r := &rows[i]
			r.EngineeringMin = r.EngineeringMin*d.Factor + d.Offset
			r.EngineeringMax = r.EngineeringMax*d.Factor + d.Offset
			if d.Factor < 0 {
				r.EngineeringMin, r.EngineeringMax = r.EngineeringMax, r.EngineeringMin
			}
			r.EngineeringMean = r.EngineeringMean*d.Factor + d.Offset
		}
	}
}
func (m *recordingService) Query(ctx context.Context, q influx.Query) ([]influx.Row, error) {
	if m.influx == nil {
		return nil, fmt.Errorf("Influx is not configured")
	}
	s := m.Store(q.Session)
	if s == nil {
		return nil, fmt.Errorf("unknown recording")
	}
	meta := s.Snapshot()
	if q.DecodeID == "" {
		q.DecodeID = meta.DefaultDecode
	}
	var defs *cache.SessionCache
	for _, d := range meta.Decodes {
		if d.ID == q.DecodeID {
			if dbc, ok := m.registry.Get(d.DBCHash); ok {
				defs = dbc.Defs
			}
		}
	}
	if defs == nil {
		return nil, fmt.Errorf("unknown decoding")
	}
	rows, err := m.influx.Query(ctx, q)
	if err == nil {
		scaleRows(rows, defs)
	}
	return rows, err
}
