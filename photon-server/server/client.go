package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

type Client struct {
	ID          string
	Remote      string
	ConnectedMS int64
	nextID      atomic.Uint64
	bytes       atomic.Uint64
	messages    atomic.Uint64
	lastSendMS  atomic.Int64
	failures    atomic.Uint64
}

type ClientTracker struct {
	mu              sync.RWMutex
	nextSeq         uint64
	clients         map[string]*Client
	listening       atomic.Bool
	acceptFailures  atomic.Uint64
	retiredBytes    uint64
	retiredMessages uint64
	retiredFailures uint64
	retiredLastSend int64
}

func NewClientTracker() *ClientTracker {
	return &ClientTracker{clients: make(map[string]*Client)}
}

func (t *ClientTracker) Register(remoteAddr string, nextID MessageID) *Client {
	t.mu.Lock()
	defer t.mu.Unlock()

	t.nextSeq++
	client := &Client{
		ID:          fmt.Sprintf("%s#%d", remoteAddr, t.nextSeq),
		Remote:      remoteAddr,
		ConnectedMS: time.Now().UnixMilli(),
	}
	client.nextID.Store(uint64(nextID))
	t.clients[client.ID] = client
	return client
}

func (t *ClientTracker) Unregister(client *Client) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.retiredBytes += client.bytes.Load()
	t.retiredMessages += client.messages.Load()
	t.retiredFailures += client.failures.Load()
	if last := client.lastSendMS.Load(); last > t.retiredLastSend {
		t.retiredLastSend = last
	}
	delete(t.clients, client.ID)
}

func listenForClients(
	ctx context.Context,
	listener net.Listener,
	store *StreamStore,
	tracker *ClientTracker,
	gate *ClientGate,
	connWg *sync.WaitGroup,
) {
	tracker.listening.Store(true)
	defer tracker.listening.Store(false)

	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				return
			default:
				tracker.acceptFailures.Add(1)
				continue
			}
		}
		connWg.Add(1)
		go func(c net.Conn) {
			defer connWg.Done()
			if !admitAndAuth(gate, c) {
				_ = c.Close()
				return
			}
			defer gate.Release(c)
			client := tracker.Register(c.RemoteAddr().String(), store.StartIDForNewClient())
			writeClient(ctx, c, client, store, tracker)
		}(conn)
	}
}

func writeClient(
	ctx context.Context,
	conn net.Conn,
	client *Client,
	store *StreamStore,
	tracker *ClientTracker,
) {
	defer conn.Close()
	defer tracker.Unregister(client)

	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}()

	nextID := MessageID(client.nextID.Load())
	for {
		msg, err := store.ReadOrWait(nextID)
		if err != nil {
			return
		}
		n, err := writeFull(conn, msg.Payload)
		client.bytes.Add(uint64(n))
		if err != nil {
			client.failures.Add(1)
			return
		}
		nextID++
		client.messages.Add(1)
		client.lastSendMS.Store(metadataNowMS.Load())
		client.nextID.Store(uint64(nextID))
	}
}

func writeFull(conn net.Conn, payload []byte) (int, error) {
	written := 0
	for len(payload) > 0 {
		n, err := conn.Write(payload)
		written += n
		if err != nil {
			return written, err
		}
		if n == 0 {
			return written, io.ErrNoProgress
		}
		payload = payload[n:]
	}
	return written, nil
}

func (t *ClientTracker) Snapshot(latestID MessageID) ClientsMetadata {
	t.mu.RLock()
	snapshot := ClientsMetadata{
		Listening:      t.listening.Load(),
		Active:         len(t.clients),
		Total:          t.nextSeq,
		Bytes:          t.retiredBytes,
		Messages:       t.retiredMessages,
		LastSendMS:     t.retiredLastSend,
		WriteFailures:  t.retiredFailures,
		AcceptFailures: t.acceptFailures.Load(),
		Clients:        make([]ClientMetadata, 0, len(t.clients)),
	}
	for _, client := range t.clients {
		nextID := MessageID(client.nextID.Load())
		lag := uint64(0)
		if latestID >= nextID {
			lag = uint64(latestID - nextID + 1)
		}
		item := ClientMetadata{
			ID:            client.ID,
			Remote:        client.Remote,
			ConnectedMS:   client.ConnectedMS,
			NextID:        nextID,
			Lag:           lag,
			Bytes:         client.bytes.Load(),
			Messages:      client.messages.Load(),
			LastSendMS:    client.lastSendMS.Load(),
			WriteFailures: client.failures.Load(),
		}
		snapshot.Bytes += item.Bytes
		snapshot.Messages += item.Messages
		snapshot.WriteFailures += item.WriteFailures
		if item.Lag > snapshot.MaxLag {
			snapshot.MaxLag = item.Lag
		}
		if item.LastSendMS > snapshot.LastSendMS {
			snapshot.LastSendMS = item.LastSendMS
		}
		snapshot.Clients = append(snapshot.Clients, item)
	}
	t.mu.RUnlock()

	sort.Slice(snapshot.Clients, func(i, j int) bool {
		return snapshot.Clients[i].ID < snapshot.Clients[j].ID
	})
	return snapshot
}

type MessageID uint64

const storeInspectBytes = 20 + 64*29
const hotTailBytes = 32 * 1024 * 1024

type Message struct {
	ID      MessageID
	Payload []byte
}
type StoreStats struct {
	LatestID          MessageID
	OldestID          MessageID
	RetainedMessages  uint64
	Segments          int
	ActiveSegmentID   MessageID
	ActiveSegmentSize int64
	DiskBytes         int64
	HotBytes          int64
	PressureHigh      bool
}

type StoreRecord struct {
	ID       MessageID
	Bytes    int
	Captured int
	Data     [storeInspectBytes]byte
}

type StoreSnapshot struct {
	Stats  StoreStats
	Recent []StoreRecord
}

type StreamStore struct {
	mu               sync.Mutex
	cond             *sync.Cond
	closed           bool
	nextID, oldestID MessageID
	hotMessages      map[MessageID][]byte
	hotOrder         []MessageID
	hotBytes         int64
}

func OpenLiveStreamStore() *StreamStore {
	s := &StreamStore{nextID: 1, hotMessages: make(map[MessageID][]byte)}
	s.cond = sync.NewCond(&s.mu)
	return s
}
func (s *StreamStore) Append(payload []byte) (MessageID, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return 0, errors.New("live buffer is closed")
	}
	if len(payload) > hotTailBytes {
		return 0, errors.New("payload exceeds live buffer capacity")
	}
	id := s.nextID
	s.nextID++
	s.addHotLocked(id, payload)
	s.oldestID = s.hotOrder[0]
	s.cond.Broadcast()
	return id, nil
}
func (s *StreamStore) ReadOrWait(id MessageID) (Message, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for id >= s.nextID && !s.closed {
		s.cond.Wait()
	}
	if s.closed {
		return Message{}, errors.New("live buffer is closed")
	}
	if payload, ok := s.hotMessages[id]; ok {
		return Message{id, payload}, nil
	}
	return Message{}, fmt.Errorf("message %d is no longer available", id)
}
func (s *StreamStore) Read(id MessageID) (Message, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if payload, ok := s.hotMessages[id]; ok {
		return Message{id, payload}, true, nil
	}
	if id >= s.nextID {
		return Message{}, false, nil
	}
	return Message{}, false, fmt.Errorf("message %d is no longer available", id)
}
func (s *StreamStore) statsLocked() StoreStats {
	latest := s.nextID - 1
	return StoreStats{LatestID: latest, OldestID: s.oldestID, RetainedMessages: uint64(len(s.hotMessages)), HotBytes: s.hotBytes}
}
func (s *StreamStore) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	s.cond.Broadcast()
	return nil
}
func (s *StreamStore) StartIDForNewClient() MessageID {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.nextID
}

func (s *StreamStore) Stats() StoreStats {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.statsLocked()
}

func (s *StreamStore) Snapshot(recentLimit int) StoreSnapshot {
	s.mu.Lock()
	defer s.mu.Unlock()

	snapshot := StoreSnapshot{Stats: s.statsLocked()}
	if recentLimit <= 0 {
		return snapshot
	}
	snapshot.Recent = make([]StoreRecord, 0, min(recentLimit, len(s.hotOrder)))
	for i := len(s.hotOrder) - 1; i >= 0 && len(snapshot.Recent) < recentLimit; i-- {
		id := s.hotOrder[i]
		payload, ok := s.hotMessages[id]
		if !ok {
			continue
		}
		record := StoreRecord{ID: id, Bytes: len(payload)}
		record.Captured = copy(record.Data[:], payload)
		snapshot.Recent = append(snapshot.Recent, record)
	}
	return snapshot
}

func (s *StreamStore) addHotLocked(id MessageID, payload []byte) {
	copyPayload := make([]byte, len(payload))
	copy(copyPayload, payload)

	s.hotMessages[id] = copyPayload
	s.hotOrder = append(s.hotOrder, id)
	s.hotBytes += int64(len(copyPayload))

	for s.hotBytes > hotTailBytes && len(s.hotOrder) > 0 {
		oldest := s.hotOrder[0]
		s.hotOrder = s.hotOrder[1:]
		if payload, ok := s.hotMessages[oldest]; ok {
			s.hotBytes -= int64(len(payload))
			delete(s.hotMessages, oldest)
		}
	}
}
