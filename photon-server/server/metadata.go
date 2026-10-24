package main

import (
	"context"
	"encoding/json"
	"net"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

var ingressStats struct {
	listening      atomic.Bool
	active         atomic.Int64
	total          atomic.Uint64
	bytes          atomic.Uint64
	reads          atomic.Uint64
	lastReceiveMS  atomic.Int64
	readFailures   atomic.Uint64
	acceptFailures atomic.Uint64
}

var metadataNowMS atomic.Int64

var queueStats struct {
	peakDepth      atomic.Uint64
	blockedNS      atomic.Int64
	blockStartedNS atomic.Int64
}

func observeQueueDepth(depth int) {
	value := uint64(depth)
	for peak := queueStats.peakDepth.Load(); value > peak; peak = queueStats.peakDepth.Load() {
		if queueStats.peakDepth.CompareAndSwap(peak, value) {
			return
		}
	}
}

type IngressMetadata struct {
	Listening      bool   `json:"listening"`
	Active         int64  `json:"active"`
	Total          uint64 `json:"total"`
	Bytes          uint64 `json:"bytes"`
	Reads          uint64 `json:"reads"`
	LastReceiveMS  int64  `json:"last_receive_ms"`
	ReadFailures   uint64 `json:"read_failures"`
	AcceptFailures uint64 `json:"accept_failures"`
}

type ServerMetadata struct {
	StartTime   time.Time       `json:"start_time"`
	UptimeMS    int64           `json:"uptime_ms"`
	CurrentTime time.Time       `json:"current_time"`
	PID         int             `json:"pid"`
	Goroutines  int             `json:"goroutines"`
	MemoryBytes uint64          `json:"memory_bytes"`
	Ingress     IngressMetadata `json:"ingress"`
	Clients     ClientsMetadata `json:"clients"`
	Queue       QueueMetadata   `json:"queue"`
	Storage     StorageMetadata `json:"storage"`
	Video       VideoMetadata   `json:"video"`
}

type QueueMetadata struct {
	Depth     int    `json:"depth"`
	Capacity  int    `json:"capacity"`
	PeakDepth uint64 `json:"peak_depth"`
	BlockedMS int64  `json:"blocked_ms"`
}

type StoredChunkMetadata struct {
	ID       MessageID `json:"id"`
	Bytes    int       `json:"bytes"`
	Captured int       `json:"captured"`
	Data     []byte    `json:"data"`
}

type StorageMetadata struct {
	OldestID          MessageID             `json:"oldest_id"`
	LatestID          MessageID             `json:"latest_id"`
	RetainedMessages  uint64                `json:"retained_messages"`
	Segments          int                   `json:"segments"`
	ActiveSegmentID   MessageID             `json:"active_segment_id"`
	ActiveSegmentSize int64                 `json:"active_segment_size"`
	DiskBytes         int64                 `json:"disk_bytes"`
	HotBytes          int64                 `json:"hot_bytes"`
	PressureHigh      bool                  `json:"pressure_high"`
	Chunks            []StoredChunkMetadata `json:"chunks"`
}

type ClientMetadata struct {
	ID            string    `json:"id"`
	Remote        string    `json:"remote"`
	ConnectedMS   int64     `json:"connected_ms"`
	NextID        MessageID `json:"next_id"`
	Lag           uint64    `json:"lag"`
	Bytes         uint64    `json:"bytes"`
	Messages      uint64    `json:"messages"`
	LastSendMS    int64     `json:"last_send_ms"`
	WriteFailures uint64    `json:"write_failures"`
}

type VideoMetadata struct {
	Listening     bool   `json:"listening"`
	BytesStored   uint64 `json:"bytes_stored"`
	SegmentCount  int    `json:"segment_count"`
	LiveClients   int    `json:"live_clients"`
	LastReceiveMS int64  `json:"last_receive_ms"`
}

type ClientsMetadata struct {
	Listening      bool             `json:"listening"`
	Active         int              `json:"active"`
	Total          uint64           `json:"total"`
	Bytes          uint64           `json:"bytes"`
	Messages       uint64           `json:"messages"`
	MaxLag         uint64           `json:"max_lag"`
	LastSendMS     int64            `json:"last_send_ms"`
	WriteFailures  uint64           `json:"write_failures"`
	AcceptFailures uint64           `json:"accept_failures"`
	Clients        []ClientMetadata `json:"clients"`
}

func listenForObservers(
	ctx context.Context,
	listener net.Listener,
	startedAt time.Time,
	store *StreamStore,
	clients *ClientTracker,
	ingress <-chan ingressPayload,
	video *VideoStore,
	gate *ClientGate,
	connWg *sync.WaitGroup,
) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				return
			default:
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
			writeServerMetadata(ctx, c, startedAt, store, clients, ingress, video)
		}(conn)
	}
}

func writeServerMetadata(
	ctx context.Context,
	conn net.Conn,
	startedAt time.Time,
	store *StreamStore,
	clients *ClientTracker,
	ingress <-chan ingressPayload,
	video *VideoStore,
) {
	defer conn.Close()

	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}()

	encoder := json.NewEncoder(conn)
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()

	for {
		now := time.Now()
		nowMS := now.UnixMilli()
		nowNS := now.UnixNano()
		metadataNowMS.Store(nowMS)
		blockedNS := queueStats.blockedNS.Load()
		if started := queueStats.blockStartedNS.Load(); started != 0 {
			blockedNS += nowNS - started
		}
		var memory runtime.MemStats
		runtime.ReadMemStats(&memory)
		storage := store.Snapshot(8)
		metadata := ServerMetadata{
			StartTime:   startedAt,
			UptimeMS:    time.Since(startedAt).Milliseconds(),
			CurrentTime: now,
			PID:         os.Getpid(),
			Goroutines:  runtime.NumGoroutine(),
			MemoryBytes: memory.Alloc,
			Ingress: IngressMetadata{
				Listening:      ingressStats.listening.Load(),
				Active:         ingressStats.active.Load(),
				Total:          ingressStats.total.Load(),
				Bytes:          ingressStats.bytes.Load(),
				Reads:          ingressStats.reads.Load(),
				LastReceiveMS:  ingressStats.lastReceiveMS.Load(),
				ReadFailures:   ingressStats.readFailures.Load(),
				AcceptFailures: ingressStats.acceptFailures.Load(),
			},
			Clients: clients.Snapshot(storage.Stats.LatestID),
			Queue: QueueMetadata{
				Depth:     len(ingress),
				Capacity:  cap(ingress),
				PeakDepth: queueStats.peakDepth.Load(),
				BlockedMS: blockedNS / int64(time.Millisecond),
			},
			Storage: storageMetadata(storage),
			Video:   videoMetadata(video),
		}
		if err := encoder.Encode(metadata); err != nil {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func videoMetadata(store *VideoStore) VideoMetadata {
	meta := VideoMetadata{
		Listening:     videoRuntime.ingressListening.Load(),
		LiveClients:   int(videoRuntime.liveClients.Load()) + int(videoUdpRuntime.subs.Load()),
		LastReceiveMS: videoRuntime.lastReceiveMS.Load(),
	}
	if store != nil {
		stats := store.Stats()
		meta.BytesStored = stats.BytesStored
		meta.SegmentCount = stats.SegmentCount
	}
	return meta
}

func storageMetadata(snapshot StoreSnapshot) StorageMetadata {
	stats := snapshot.Stats
	metadata := StorageMetadata{
		OldestID:          stats.OldestID,
		LatestID:          stats.LatestID,
		RetainedMessages:  stats.RetainedMessages,
		Segments:          stats.Segments,
		ActiveSegmentID:   stats.ActiveSegmentID,
		ActiveSegmentSize: stats.ActiveSegmentSize,
		DiskBytes:         stats.DiskBytes,
		HotBytes:          stats.HotBytes,
		PressureHigh:      stats.PressureHigh,
		Chunks:            make([]StoredChunkMetadata, 0, len(snapshot.Recent)),
	}
	for _, record := range snapshot.Recent {
		data := make([]byte, record.Captured)
		copy(data, record.Data[:record.Captured])
		metadata.Chunks = append(metadata.Chunks, StoredChunkMetadata{
			ID:       record.ID,
			Bytes:    record.Bytes,
			Captured: record.Captured,
			Data:     data,
		})
	}
	return metadata
}
