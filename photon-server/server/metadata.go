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
	bytes          atomic.Uint64 // bytes of whole batches submitted to the parser
	reads          atomic.Uint64 // reads off ingress connections that returned data
	batches        atomic.Uint64 // batches submitted to the parser
	lastReceiveMS  atomic.Int64
	readFailures   atomic.Uint64
	acceptFailures atomic.Uint64
	skippedBytes   atomic.Uint64 
	cutBatches     atomic.Uint64 // batches cut short by a new header or a disconnect; their whole frames are kept
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
	Batches        uint64 `json:"batches"`
	LastReceiveMS  int64  `json:"last_receive_ms"`
	ReadFailures   uint64 `json:"read_failures"`
	AcceptFailures uint64 `json:"accept_failures"`
	SkippedBytes   uint64 `json:"skipped_bytes"`
	CutBatches     uint64 `json:"cut_batches"`
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
	CAN         CANMetadata     `json:"can"`
	Config      ConfigMetadata  `json:"config"`
}

// CANMetadata counts what CAN decoding has done since startup.
type CANMetadata struct {
	Enabled    bool   `json:"enabled"`
	Frames     uint64 `json:"frames"`
	Rows       uint64 `json:"rows"`
	UnknownID  uint64 `json:"unknown_id"`  // frames no DBC defines, saved raw
	Short      uint64 `json:"short"`       // frames too short for some signals
	UnknownMux uint64 `json:"unknown_mux"` // frames naming an undefined mux page
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
	pool *batchPool,
	video *VideoStore,
	settings *canSettings,
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
			writeServerMetadata(ctx, c, startedAt, store, clients, pool, video, settings)
		}(conn)
	}
}

// writeServerMetadata sends a metadata line every second. The observer may
// also send commands on the same connection; each gets a reply line, and the
// next metadata line follows straight away so the change shows at once.
func writeServerMetadata(
	ctx context.Context,
	conn net.Conn,
	startedAt time.Time,
	store *StreamStore,
	clients *ClientTracker,
	pool *batchPool,
	video *VideoStore,
	settings *canSettings,
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

	// Metadata and replies come from two goroutines; one line at a time.
	var writeMu sync.Mutex
	encoder := json.NewEncoder(conn)
	write := func(v any) error {
		writeMu.Lock()
		defer writeMu.Unlock()
		return encoder.Encode(v)
	}
	refresh := make(chan struct{}, 1)
	go readObserverCommands(conn, write, settings, refresh)

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
				Batches:        ingressStats.batches.Load(),
				LastReceiveMS:  ingressStats.lastReceiveMS.Load(),
				ReadFailures:   ingressStats.readFailures.Load(),
				AcceptFailures: ingressStats.acceptFailures.Load(),
				SkippedBytes:   ingressStats.skippedBytes.Load(),
				CutBatches:     ingressStats.cutBatches.Load(),
			},
			Clients: clients.Snapshot(storage.Stats.LatestID),
			Queue: QueueMetadata{
				Depth:     len(pool.ready),
				Capacity:  cap(pool.ready),
				PeakDepth: queueStats.peakDepth.Load(),
				BlockedMS: blockedNS / int64(time.Millisecond),
			},
			Storage: storageMetadata(storage),
			Video:   videoMetadata(video),
			CAN: CANMetadata{
				Enabled:    canStats.enabled.Load(),
				Frames:     canStats.frames.Load(),
				Rows:       canStats.rows.Load(),
				UnknownID:  canStats.unknownID.Load(),
				Short:      canStats.short.Load(),
				UnknownMux: canStats.unknownMux.Load(),
			},
			Config: configMetadata(settings.current()),
		}
		if err := write(metadata); err != nil {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		case <-refresh:
		}
	}
}

// observerCommand is a line an observer sends to change settings.
type observerCommand struct {
	Set struct {
		CAN *canChange `json:"can"`
	} `json:"set"`
}

// observerReply answers one command. It is a line of its own between metadata
// lines, so an observer tells it apart by its "reply" key.
type observerReply struct {
	Reply replyBody `json:"reply"`
}

type replyBody struct {
	OK    bool   `json:"ok"`
	Error string `json:"error,omitempty"`
}

// readObserverCommands applies each command the observer sends and replies to
// it. It returns when the connection closes or sends something unreadable; the
// metadata lines carry on either way.
func readObserverCommands(conn net.Conn, write func(any) error, settings *canSettings, refresh chan<- struct{}) {
	decoder := json.NewDecoder(conn)
	for {
		var cmd observerCommand
		if err := decoder.Decode(&cmd); err != nil {
			return
		}
		var reply observerReply
		if cmd.Set.CAN == nil {
			reply.Reply.Error = "nothing to change"
		} else if _, err := settings.apply(*cmd.Set.CAN); err != nil {
			reply.Reply.Error = err.Error()
		} else {
			reply.Reply.OK = true
		}
		if write(reply) != nil {
			return
		}
		select {
		case refresh <- struct{}{}:
		default:
		}
	}
}

// ConfigMetadata is the configuration the server is running with.
type ConfigMetadata struct {
	CAN CANConfigMetadata `json:"can"`
}

// CANConfigMetadata is the CAN configuration in use. An observer can change
// Decode and DBCDir; SampleRates are shown for reference.
type CANConfigMetadata struct {
	Decode      bool                 `json:"decode"`
	DBCDir      string               `json:"dbc_dir"`
	SampleRates []SampleRateMetadata `json:"sample_rates"`
}

type SampleRateMetadata struct {
	Messages string  `json:"messages"`
	Hz       float64 `json:"hz"`
}

func configMetadata(can CANConfig) ConfigMetadata {
	rates := make([]SampleRateMetadata, len(can.SampleRates))
	for i, r := range can.SampleRates {
		rates[i] = SampleRateMetadata{Messages: r.Messages, Hz: r.Hz}
	}
	return ConfigMetadata{CAN: CANConfigMetadata{Decode: can.Decode, DBCDir: can.DBCDir, SampleRates: rates}}
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
