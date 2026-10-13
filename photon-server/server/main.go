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
	"server/wire"
	"sync"
	"syscall"
	"time"
)

func main() {
	log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds)

	cfg, err := ParseConfig(os.Args[1:])
	if err != nil {
		if errors.Is(err, flag.ErrHelp) {
			os.Exit(0)
		}
		log.Fatalf("[photon-server] Configuration error: %v", err)
	}

	startedAt := time.Now()
	log.Printf("[photon-server] Initializing recordings at %s...", cfg.DataDir)

	store := OpenLiveStreamStore()
	defer store.Close()

	recordings, err := openRecordingService(cfg)
	if err != nil {
		log.Fatalf("recording service: %v", err)
	}
	defer recordings.Close()

	log.Printf("[photon-server] Initializing video storage at %s...", cfg.VideoSegmentsDir())
	videoStore, err := OpenVideoStore(cfg.VideoRoot())
	if err != nil {
		log.Fatalf("[photon-server] Failed to open video store: %v", err)
	}
	defer videoStore.Close()

	vstats := videoStore.Stats()
	log.Printf("[photon-server] Video store initialized: %d segments recovered, %d bytes on disk, %d GOP rows",
		vstats.SegmentCount, vstats.BytesStored, len(videoStore.PointerRows()))

	// RTP/RTCP clock alignment. Until a sender report arrives, video NALs are
	// stamped with receive time and labelled as such, so a caller can tell an aligned 
	// recording from an unaligned one instead of assuming 
	videoClock := NewRTPClock()
	videoClock.Load(cfg.VideoRoot())
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

	password, generated := EnsurePassword(cfg.Password)
	cfg.Password = password
	if generated {
		log.Printf("[photon-server] client password %s (set PHOTON_PASSWORD to pin it)", password)
	} else {
		log.Printf("[photon-server] client password is set")
	}
	gate := NewClientGate(password)

	log.Printf("[photon-server] Ready to process streams. Press Ctrl+C or send SIGTERM to shutdown.")

	browser, err := startBrowserService(cfg, recordings, videoStore)
	if err != nil {
		log.Fatalf("browser service: %v", err)
	}
	defer browser.Close()

	clients := NewClientTracker()
	ingress := make(chan []byte, 1024)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var wg sync.WaitGroup

	// Append loop
	wg.Add(1)
	go func() {
		defer wg.Done()
		appendLoop(store, ingress)
	}()

	// Ingress listener & connection tracking
	var ingressConnWg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenForIngress(ctx, ingressListener, ingress, &ingressConnWg, recordings)
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
		listenForObservers(ctx, observerListener, startedAt, store, clients, ingress, videoStore, gate, &observerConnWg)
	}()

	// Video RTP/H.264 ingress (UDP direct or Oracle/WireGuard relay)
	wg.Add(1)
	go func() {
		defer wg.Done()
		listenVideoIngress(ctx, videoIngress, videoStore, videoClock)
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
	if rtcpErr == nil {
		_ = videoRTCP.Close()
	}
	_ = videoClientListener.Close()

	// Wait for existing ingress readers to finish reading and close ingress channel
	ingressConnWg.Wait()
	close(ingress)

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
		close(done)
	}()

	select {
	case <-done:
		log.Printf("[photon-server] Graceful shutdown completed successfully.")
	case <-time.After(5 * time.Second):
		log.Printf("[photon-server] Shutdown timed out after 5s; exiting.")
	}
}

// Publish already persisted CANP batches to the compatibility TCP live buffer.
func appendLoop(store *StreamStore, ingress <-chan []byte) {
	for payload := range ingress {
		if _, err := store.Append(payload); err != nil {
			log.Printf("live fanout: %v", err)
		}

	}
}

func listenForIngress(
	ctx context.Context,
	listener net.Listener,
	ingress chan<- []byte,
	connWg *sync.WaitGroup,
	recordings *recordingService,
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
			readIngressConnection(ctx, c, ingress, recordings)
		}(conn)
	}
}

func readIngressConnection(ctx context.Context, conn net.Conn, ingress chan<- []byte, recordings *recordingService) {
	source := conn.RemoteAddr().String()
	defer recordings.EndSource(source)
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

	for {
		batch, err := wire.ReadBatch(conn)
		if err != nil {
			if err != io.EOF && !errors.Is(err, net.ErrClosed) {
				ingressStats.readFailures.Add(1)
			}
			return
		}
		ingressStats.bytes.Add(uint64(len(batch.Raw)))
		ingressStats.reads.Add(1)
		ingressStats.lastReceiveMS.Store(time.Now().UnixMilli())
		if err := recordings.Ingest(source, batch); err != nil {
			log.Printf("raw ingest: %v", err)
			return
		}
		select {
		case ingress <- batch.Raw:
		case <-ctx.Done():
			return
		}

		observeQueueDepth(len(ingress))
	}
}
