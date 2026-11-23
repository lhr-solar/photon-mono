package main

import (
	"context"
	"errors"
	"flag"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// influxWriter is the InfluxDB writer, or nil when InfluxDB writes are off. The
// metadata reads it.
var influxWriter atomic.Pointer[dbWriter]

func main() {
	log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds)

	if len(os.Args) > 1 && os.Args[1] == "backfill" {
		os.Exit(backfillMain(os.Args[2:], os.Getenv, os.Stdout))
	}

	cfg, err := ParseConfig(os.Args[1:])
	if err != nil {
		if errors.Is(err, flag.ErrHelp) {
			os.Exit(0)
		}
		log.Fatalf("[photon-server] Configuration error: %v", err)
	}

	startedAt := time.Now()
	log.Printf("[photon-server] Initializing storage at %s...", cfg.SegmentsDir())

	// Old-format segment files move into run folders before the store opens. If
	// that fails they stay where they are, and new messages are numbered above
	// them so that no ID is used twice.
	var idFloor MessageID
	migrated, err := migrateLegacySegments(cfg.SegmentsDir())
	if err != nil {
		log.Printf("[photon-server] Could not migrate the old segment files in %s; they stay where they are and migration is tried again next start: %v",
			cfg.SegmentsDir(), err)
		floor, ferr := legacyNextID(cfg.SegmentsDir())
		if ferr != nil {
			log.Fatalf("[photon-server] Cannot read the old segment files to number new messages above them: %v", ferr)
		}
		idFloor = floor
	} else if migrated.Moved > 0 {
		log.Printf("[photon-server] Migrated %d records (%.1f MiB) in %d runs from the old segment files to %s in %s; the old files are in %s",
			migrated.Records, float64(migrated.Bytes)/(1<<20), migrated.Runs, cfg.SegmentsDir(),
			migrated.Elapsed.Round(time.Millisecond), filepath.Join(cfg.SegmentsDir(), legacyFolder))
	}

	// The run's folder is named by its session ID, so backfill knows which run
	// wrote each record.
	store, err := OpenStreamStore(cfg.DataDir, sessionID(startedAt))
	if err != nil {
		log.Fatalf("[photon-server] Failed to open stream store: %v", err)
	}
	defer store.Close()
	store.raiseNextID(idFloor)
	store.SetMaxBytes(int64(cfg.StoreMaxGB) << 30)

	stats := store.Stats()
	log.Printf("[photon-server] Store initialized: %d segments recovered (oldest ID: %d, latest ID: %d, disk bytes: %d)",
		stats.Segments, stats.OldestID, stats.LatestID, stats.DiskBytes)

	log.Printf("[photon-server] Initializing video storage at %s...", cfg.VideoSegmentsDir())
	videoStore, err := OpenVideoStore(cfg.DataDir)
	if err != nil {
		log.Fatalf("[photon-server] Failed to open video store: %v", err)
	}
	defer videoStore.Close()

	vstats := videoStore.Stats()
	log.Printf("[photon-server] Video store initialized: %d segments recovered, %d bytes on disk, %d GOP rows",
		vstats.SegmentCount, vstats.BytesStored, len(videoStore.PointerRows()))

	// RTP/RTCP clock alignment. Until a sender report arrives, video NALs are
	// stamped with receive time and labelled as such, so a caller can tell an
	// aligned recording from an unaligned one instead of assuming (§8.5).
	videoClock := NewRTPClock()
	videoClock.Load(cfg.DataDir)
	videoStore.SetClock(videoClock)
	log.Printf("[photon-server] Video clock: provenance=%s manual_offset=%d ms",
		clockProvenanceName(videoClock.Provenance()), videoClock.ManualOffset())

	ingressListener, err := net.Listen("tcp", cfg.IngressAddr())
	if err != nil {
		log.Fatalf("[photon-server] Failed to bind ingress listener on %s: %v", cfg.IngressAddr(), err)
	}
	defer ingressListener.Close()

	clientListener, err := net.Listen("tcp", cfg.ClientAddr())
	if err != nil {
		log.Fatalf("[photon-server] Failed to bind client listener on %s: %v", cfg.ClientAddr(), err)
	}
	defer clientListener.Close()

	observerListener, err := net.Listen("tcp", cfg.ObserverAddr())
	if err != nil {
		log.Fatalf("[photon-server] Failed to bind observer listener on %s: %v", cfg.ObserverAddr(), err)
	}
	defer observerListener.Close()

	videoIngress, err := openVideoIngress(cfg.VideoIngressAddr())
	if err != nil {
		log.Fatalf("[photon-server] Failed to bind video ingress on %s: %v", cfg.VideoIngressAddr(), err)
	}
	defer videoIngress.Close()

	videoIngressTCP, err := net.Listen("tcp", cfg.VideoIngressAddr())
	if err != nil {
		log.Fatalf("[photon-server] Failed to bind TCP video ingress on %s: %v", cfg.VideoIngressAddr(), err)
	}
	defer videoIngressTCP.Close()

	videoClientListener, err := net.Listen("tcp", cfg.VideoClientAddr())
	if err != nil {
		log.Fatalf("[photon-server] Failed to bind video client listener on %s: %v", cfg.VideoClientAddr(), err)
	}
	defer videoClientListener.Close()

	// A missing RTCP socket degrades timestamping to receive time; it is not
	// worth refusing to record video over.
	videoRTCP, rtcpErr := openVideoIngress(cfg.VideoRTCPAddr())
	if rtcpErr != nil {
		log.Printf("[photon-server] Could not bind RTCP on %s: %v; video timestamps stay receive-stamped",
			cfg.VideoRTCPAddr(), rtcpErr)
	} else {
		defer videoRTCP.Close()
	}

	log.Printf("[photon-server] Ingress listening on %s (TCP CANP)", cfg.IngressAddr())
	log.Printf("[photon-server] Client fan-out listening on %s (TCP)", cfg.ClientAddr())
	log.Printf("[photon-server] Observer telemetry listening on %s (TCP JSON)", cfg.ObserverAddr())
	log.Printf("[photon-server] Video ingress listening on %s (UDP RTP/H.264 PT=96)", cfg.VideoIngressAddr())
	log.Printf("[photon-server] Video ingress listening on %s (TCP length-prefixed RTP, Oracle pipe)", cfg.VideoIngressAddr())
	if rtcpErr == nil {
		log.Printf("[photon-server] Video RTCP listening on %s (UDP sender reports)", cfg.VideoRTCPAddr())
	}
	log.Printf("[photon-server] Video clients listening on %s (TCP live/replay/locate/range)", cfg.VideoClientAddr())

	var videoUdp net.PacketConn
	var udpErr error
	if cfg.VideoUdpPort != 0 {
		videoUdp, udpErr = openVideoIngress(cfg.VideoUdpAddr())
		if udpErr != nil {
			log.Printf("[photon-server] Could not bind video UDP fan-out on %s: %v", cfg.VideoUdpAddr(), udpErr)
		} else {
			defer videoUdp.Close()
			log.Printf("[photon-server] Video fan-out listening on %s (UDP live NALs)", cfg.VideoUdpAddr())
		}
	} else {
		log.Printf("[photon-server] Video UDP fan-out off")
	}
	log.Printf("[photon-server] TSDB root %s", cfg.TsdbRoot())

	password, generated := EnsurePassword(cfg.Password)
	cfg.Password = password
	if generated {
		log.Printf("[photon-server] client password %s (set PHOTON_PASSWORD to pin it)", password)
	} else {
		log.Printf("[photon-server] client password is set")
	}
	gate := NewClientGate(password)

	log.Printf("[photon-server] Ready to process streams. Press Ctrl+C or send SIGTERM to shutdown.")

	liveWriter := startLiveWriter(cfg)
	defer liveWriter.Stop()

	localUI := startLocalUI(cfg)
	defer localUI.Stop()

	clients := NewClientTracker()
	
	pool := newBatchPool(cfg.Ingest.count(), cfg.Ingest.bufferSize())
	log.Printf("[photon-server] Ingest pool %s", cfg.Ingest)

	// The live ring keeps the recent frames for protocol v2 clients.
	ring := newLiveRing(liveRingFrames(cfg))
	ringFrames, _ := ring.stats()
	log.Printf("[photon-server] Live ring %d frames (%.1f MiB, %s at peak)",
		ringFrames, float64(ring.bytes())/(1<<20), cfg.LiveSeconds)

	// Frame rows come from the decoder. The writer runs whenever InfluxDB is not
	// off, so turning decoding on from the observer later also starts rows.
	var session *frameSession
	if !strings.EqualFold(cfg.InfluxURL, "off") {
		w, err := newDBWriter(cfg.InfluxURL, cfg.InfluxDB, cfg.InfluxToken,
			dbWriterOptions{Flush: cfg.InfluxFlush, NoSync: cfg.InfluxNoSync})
		if err != nil {
			log.Printf("[photon-server] InfluxDB writes off: %v", err)
		} else {
			influxWriter.Store(w)
			session = newFrameSession(startedAt, w)
			log.Printf("[photon-server] Frames -> InfluxDB %s database %q, session %s", cfg.InfluxURL, cfg.InfluxDB, session.sid)
			if cfg.InfluxToken == "" {
				log.Printf("[photon-server] InfluxDB token not set (PHOTON_INFLUX_TOKEN); writes need a server started --without-auth")
			}
		}
	}

	// Protocol v2 clients see the live run as the frame session ID.
	liveRun := func() string {
		if session == nil {
			return ""
		}
		return session.sid
	}
	// History queries go to the InfluxDB the rows are written to.
	var querier *influxQuerier
	if !strings.EqualFold(cfg.InfluxURL, "off") {
		querier = newInfluxQuerier(cfg.InfluxURL, cfg.InfluxDB, cfg.InfluxToken, cfg.InfluxQueries)
	}
	hub := newV2Hub(cfg, gate, liveRun, ring, querier)
	v2Current.Store(hub)

	decoder := startCANDecoder(cfg.CAN, session)
	// The observer can change these while the server runs.
	canConfig := newCANSettings(cfg.CAN, decoder, session)
	// Protocol v2 clients get each catalog the decoder uses, and the observer's
	// changes to it.
	canConfig.onCatalog = hub.SetCatalog
	if decoder != nil {
		hub.SetCatalog(decoder.catalog)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	stopV2 := startV2(ctx, cfg, hub)

	var wg sync.WaitGroup

	// Append loop
	wg.Add(1)
	go func() {
		defer wg.Done()
		appendLoop(store, pool, decoder, canConfig.swap, ring)
		// appendLoop has returned, so nothing else writes to the session now.
		session.Close(3 * time.Second)
	}()

	// Ingress listener & connection tracking
	var ingressConnWg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenForIngress(ctx, ingressListener, &ingressConnWg, pool, cfg.Ingest.Latency)
	}()

	// Client fan-out listener & connection tracking
	var clientConnWg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenForClients(ctx, clientListener, store, clients, gate, &clientConnWg)
	}()

	// Observer telemetry listener & connection tracking
	var observerConnWg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenForObservers(ctx, observerListener, startedAt, store, clients, pool, videoStore, canConfig, gate, &observerConnWg)
	}()

	// Video RTP/H.264 ingress (UDP local/WG + TCP Oracle pipe)
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenVideoIngress(ctx, videoIngress, videoStore, videoClock)
	}()
	var videoIngressTCPWg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenVideoIngressTCP(ctx, videoIngressTCP, videoStore, videoClock, &videoIngressTCPWg)
	}()

	// RTCP sender reports
	if rtcpErr == nil {
		wg.Add(1)
		go func() {
			defer wg.Done()
			listenRTCP(ctx, videoRTCP, videoClock)
		}()
	}

	// Video live/replay clients
	var videoClientWg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenVideoClients(ctx, videoClientListener, videoStore, gate, &videoClientWg)
	}()

	if videoUdp != nil {
		udpFan := NewVideoUdpFanout(videoUdp, videoStore, password)
		wg.Add(1)
		go func() {
			defer wg.Done()
			udpFan.Run(ctx)
		}()
	}

	// Periodic compaction loop
	wg.Add(1)
	go func() {
		defer wg.Done()
		compactionLoop(ctx, store, clients)
	}()

	// Wait for shutdown signal
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	sig := <-sigChan
	log.Printf("[photon-server] Received signal %v, initiating graceful shutdown...", sig)
	cancel()

	// Stop accepting new connections
	_ = ingressListener.Close()
	_ = clientListener.Close()
	_ = observerListener.Close()
	_ = videoIngress.Close()
	_ = videoIngressTCP.Close()
	if rtcpErr == nil {
		_ = videoRTCP.Close()
	}
	_ = videoClientListener.Close()
	stopV2()

	// Wait for existing ingress readers to finish reading and close ingress channel
	ingressConnWg.Wait()
	close(pool.ready)

	// Close stores so any waiting client readers unblock immediately and video indexes flush
	_ = store.Close()
	_ = videoStore.Close()

	// Wait with timeout for all goroutines to finish
	done := make(chan struct{})
	go func() {
		wg.Wait()
		clientConnWg.Wait()
		observerConnWg.Wait()
		videoClientWg.Wait()
		videoIngressTCPWg.Wait()
		close(done)
	}()

	select {
	case <-done:
		log.Printf("[photon-server] Graceful shutdown completed successfully.")
	case <-time.After(5 * time.Second):
		log.Printf("[photon-server] Shutdown timed out after 5s; exiting.")
	}
}

// appendLoop stores each submitted buffer, adds its frames to the live ring,
// decodes its CAN frames when decoder is not nil, and returns the buffer to the
// pool. A decoder sent on swap replaces the current one between buffers; nil
// turns decoding off.
func appendLoop(store *StreamStore, pool *batchPool, decoder *canDecoder, swap <-chan *canDecoder, ring *liveRing) {
	for {
		var batch []byte
		select {
		case decoder = <-swap:
			continue
		case b, ok := <-pool.ready:
			if !ok {
				return
			}
			batch = b
		}
		if store.StoragePressureHigh() {
			started := time.Now()
			queueStats.blockStartedNS.Store(started.UnixNano())
			for store.StoragePressureHigh() {
				time.Sleep(100 * time.Millisecond)
			}
			queueStats.blockStartedNS.Store(0)
			queueStats.blockedNS.Add(time.Since(started).Nanoseconds())
		}
		if _, err := store.Append(batch); err != nil {
			// Append error during store closure or write failure
		}
		ring.appendBuffer(batch)
		if decoder != nil {
			decoder.decode(batch)
		}
		pool.put(batch)
		
	}
}

func listenForIngress(
	ctx context.Context,
	listener net.Listener,
	connWg *sync.WaitGroup, 
	pool *batchPool,
	latency time.Duration,
) {
	ingressStats.listening.Store(true)
	defer ingressStats.listening.Store(false)

	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				return
			default:
				ingressStats.acceptFailures.Add(1)
				continue
			}
		}
		ingressStats.active.Add(1)
		ingressStats.total.Add(1)

		connWg.Add(1)
		go func(c net.Conn) {
			defer connWg.Done()
			readIngressConnection(ctx, c, pool, latency)
		}(conn)
	}
}

func readIngressConnection(ctx context.Context, conn net.Conn, pool *batchPool, latency time.Duration) {
	// This reader owns the connection and its active-connection count.
	defer conn.Close()
	defer ingressStats.active.Add(-1)

	// Closing the connection on shutdown unblocks a pending Read.
	// When the reader returns first, done stops this helper without closing it early.
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}()

	// The reader submits batches to the parser itself, and returns when the connection ends.
	err := newCanpReader(conn, pool, latency).run(ctx)
	if !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) && ctx.Err() == nil {
		ingressStats.readFailures.Add(1)
		log.Printf("[photon-server] ingress %s: %v; closing", conn.RemoteAddr(), err)
	}
}

func compactionLoop(ctx context.Context, store *StreamStore, tracker *ClientTracker) {
	// A finished segment is only removable after its frames are in Influx.
	// That upload is not running, so the files stay on disk.
	log.Printf("[photon-server] keeping segment files (latest=%d readers=%d)", store.LatestID(), tracker.Count())
	<-ctx.Done()
}
