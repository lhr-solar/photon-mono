package main

import (
	"context"
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

func (t *ClientTracker) MinNextID(defaultID MessageID) MessageID {
	t.mu.RLock()
	defer t.mu.RUnlock()

	if len(t.clients) == 0 {
		return defaultID
	}

	minID := MessageID(^uint64(0))
	for _, client := range t.clients {
		nextID := MessageID(client.nextID.Load())
		if nextID < minID {
			minID = nextID
		}
	}
	return minID
}

func (t *ClientTracker) MaxLag(latestID MessageID) uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()
	var max uint64
	for _, client := range t.clients {
		nextID := MessageID(client.nextID.Load())
		if latestID >= nextID {
			lag := uint64(latestID - nextID + 1)
			if lag > max {
				max = lag
			}
		}
	}
	return max
}

func (t *ClientTracker) Count() int {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return len(t.clients)
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
