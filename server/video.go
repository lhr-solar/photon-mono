package main

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"log"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

var videoRuntime struct {
	ingressListening atomic.Bool
	clientListening  atomic.Bool
	bytesIn          atomic.Uint64
	packetsIn        atomic.Uint64
	lastReceiveMS    atomic.Int64
	liveClients      atomic.Int64
	acceptFailures   atomic.Uint64
}

func listenVideoIngress(
	ctx context.Context,
	conn net.PacketConn,
	store *VideoStore,
	clock *RTPClock,
) {
	videoRuntime.ingressListening.Store(true)
	defer videoRuntime.ingressListening.Store(false)

	buf := make([]byte, 65535)
	depay := &RTPDepacketizer{}
	for {
		_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, _, err := conn.ReadFrom(buf)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}
			select {
			case <-ctx.Done():
				return
			default:
				continue
			}
		}
		if n == 0 {
			continue
		}
		ingestRTPDatagram(ctx, store, clock, depay, buf[:n])
	}
}

// listenVideoIngressTCP accepts the Oracle-pipe path: a plain TCP client sends
// length-prefixed RTP datagrams (u32be length + packet). Same payload as UDP
// :6200, so SSH -R 6200 can carry video the way it already carries CANP :6100.
// Senders do not SSH; they connect to the public IP.
func listenVideoIngressTCP(
	ctx context.Context,
	listener net.Listener,
	store *VideoStore,
	clock *RTPClock,
	connWg *sync.WaitGroup,
) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				return
			default:
				videoRuntime.acceptFailures.Add(1)
				continue
			}
		}
		connWg.Add(1)
		go func(c net.Conn) {
			defer connWg.Done()
			serveVideoIngressTCP(ctx, c, store, clock)
		}(conn)
	}
}

func serveVideoIngressTCP(ctx context.Context, conn net.Conn, store *VideoStore, clock *RTPClock) {
	defer conn.Close()
	depay := &RTPDepacketizer{}
	hdr := make([]byte, 4)
	for {
		if ctx.Err() != nil {
			return
		}
		_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, err := io.ReadFull(conn, hdr); err != nil {
			if ctx.Err() != nil {
				return
			}
			if isTimeout(err) {
				continue
			}
			return
		}
		n := binary.BigEndian.Uint32(hdr)
		if n == 0 || n > 65535 {
			return
		}
		pkt := make([]byte, n)
		_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, err := io.ReadFull(conn, pkt); err != nil {
			return
		}
		ingestRTPDatagram(ctx, store, clock, depay, pkt)
	}
}

func ingestRTPDatagram(
	ctx context.Context,
	store *VideoStore,
	clock *RTPClock,
	depay *RTPDepacketizer,
	datagram []byte,
) {
	videoRuntime.bytesIn.Add(uint64(len(datagram)))
	videoRuntime.packetsIn.Add(1)
	nowMS := videoNowMS()
	videoRuntime.lastReceiveMS.Store(int64(nowMS))

	nals := depay.Push(datagram)
	for _, nal := range nals {
		// The RTP timestamp is the sender's clock; RTCP sender reports are
		// what make it comparable to telemetry time. Receive time survives
		// as a labelled fallback rather than as the silent default it used
		// to be (§8.5).
		wallMS, prov := nowMS, clockProvReceive
		if clock != nil {
			wallMS, prov = clock.WallMs(nal.RTPTimestamp, nowMS)
		}
		if _, err := store.AppendStamped(nal.Payload, wallMS, nal.RTPTimestamp, nowMS, prov); err != nil {
			if ctx.Err() != nil {
				return
			}
		}
	}
}

func listenVideoClients(
	ctx context.Context,
	listener net.Listener,
	store *VideoStore,
	gate *ClientGate,
	connWg *sync.WaitGroup,
) {
	videoRuntime.clientListening.Store(true)
	defer videoRuntime.clientListening.Store(false)

	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				return
			default:
				videoRuntime.acceptFailures.Add(1)
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
			serveVideoClient(ctx, c, store)
		}(conn)
	}
}

func serveVideoClient(ctx context.Context, conn net.Conn, store *VideoStore) {
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

	// Locate and range reads share one authenticated connection. Opening a TCP
	// connection for every GOP exhausted the shared per-IP admission limit.
	for {
		_ = conn.SetReadDeadline(time.Now().Add(10 * time.Second))
		req, err := DecodeVideoRequestExt(conn)
		if err != nil {
			return
		}
		_ = conn.SetReadDeadline(time.Time{})

		switch req.Mode {
		case VideoModeReplay:
			writeVideoReplay(ctx, conn, store, req.StartTimeMs, 0)
			return
		case VideoModeReplayRange:
			// endTimeMs stops replay at B instead of running to end-of-data,
			// which is what lets a bounded window be fetched (§8.2).
			writeVideoReplay(ctx, conn, store, req.StartTimeMs, req.EndTimeMs)
			return
		case VideoModeLocate:
			res := store.Locate(req.StreamID, req.StartTimeMs, int(req.Prefetch))
			_ = conn.SetWriteDeadline(time.Now().Add(time.Duration(videoWriteTimeoutMillis) * time.Millisecond))
			if err := EncodeVideoLocateReply(conn, res); err != nil {
				return
			}
			_ = conn.SetWriteDeadline(time.Time{})
		case VideoModeRange:
			payload, err := store.ReadRange(req.SegmentID, req.Offset, req.Length)
			if err != nil {
				log.Printf("[photon-server] video range read seg=%d off=%d len=%d: %v",
					req.SegmentID, req.Offset, req.Length, err)
				return
			}
			_ = conn.SetWriteDeadline(time.Now().Add(time.Duration(videoWriteTimeoutMillis) * time.Millisecond))
			if err := EncodeVideoRangeReply(conn, payload); err != nil {
				return
			}
			_ = conn.SetWriteDeadline(time.Time{})
		case VideoModeClock:
			offset := int64(req.StartTimeMs)
			if err := store.SetManualOffset(offset); err != nil {
				log.Printf("[photon-server] video clock nudge %+d: %v", offset, err)
				return
			}
			_ = conn.SetWriteDeadline(time.Now().Add(time.Duration(videoWriteTimeoutMillis) * time.Millisecond))
			if err := EncodeVideoClockReply(conn, store.ManualOffset(), store.ClockProvenance()); err != nil {
				return
			}
			_ = conn.SetWriteDeadline(time.Time{})
		default:
			writeVideoLive(ctx, conn, store)
			return
		}
	}
}

// waitPeerClose returns a channel that closes when the peer goes away.
// Live waiters used to sit in ReadOrWait after the client EOF and hold a
// ClientGate slot until the next picture (or forever, if ingest is quiet).
func waitPeerClose(conn net.Conn, stop <-chan struct{}) <-chan struct{} {
	dead := make(chan struct{})
	go func() {
		defer close(dead)
		buf := make([]byte, 1)
		for {
			select {
			case <-stop:
				return
			default:
			}
			_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
			_, err := conn.Read(buf)
			if err != nil {
				var ne net.Error
				if errors.As(err, &ne) && ne.Timeout() {
					continue
				}
				return
			}
		}
	}()
	return dead
}

func writeVideoLive(ctx context.Context, conn net.Conn, store *VideoStore) {
	videoRuntime.liveClients.Add(1)
	defer videoRuntime.liveClients.Add(-1)

	stopWatch := make(chan struct{})
	defer close(stopWatch)
	peerDead := waitPeerClose(conn, stopWatch)

	if err := sendVideoConfig(conn, store, videoNowMS()); err != nil {
		return
	}

	needKey := true
	var next uint64
	var lastKeySeq uint64
	now := videoNowMS()
	if kf, seq, ok := store.LatestKeyframe(); ok {
		age := int64(now) - int64(kf.TimestampMs)
		if age >= 0 && age <= int64(videoLiveKeyframeFreshMs) {
			if err := writeVideoFrameDeadline(conn, kf); err != nil {
				return
			}
			next = seq + 1
			lastKeySeq = seq
			needKey = false
		}
	}
	if next == 0 {
		next = store.NextSeq()
		if next == 0 {
			next = 1
		}
	}

	for {
		if ctx.Err() != nil {
			return
		}
		select {
		case <-peerDead:
			return
		default:
		}
		type waited struct {
			frame VideoFrame
			seq   uint64
			err   error
		}
		ch := make(chan waited, 1)
		go func(want uint64) {
			frame, seq, err := store.ReadOrWait(want)
			ch <- waited{frame, seq, err}
		}(next)
		var frame VideoFrame
		var seq uint64
		var err error
		select {
		case <-ctx.Done():
			return
		case <-peerDead:
			return
		case w := <-ch:
			frame, seq, err = w.frame, w.seq, w.err
		}
		if err != nil {
			return
		}
		if seq < next {
			next = store.NextSeq()
			needKey = true
			continue
		}
		if discardableNALType(nalTypeOfAnnexB(frame.Payload)) {
			next = seq + 1
			continue
		}
		if frame.IsKeyframe() && seq == lastKeySeq {
			next = store.NextSeq()
			if next <= seq {
				next = seq + 1
			}
			needKey = true
			continue
		}
		if needKey && !frame.IsKeyframe() && !frame.IsConfig() {
			next = seq + 1
			continue
		}
		if err := writeVideoFrameDeadline(conn, frame); err != nil {
			if isTimeout(err) {
				if err := resyncLive(conn, store); err != nil {
					return
				}
				if _, seq, ok := store.LatestKeyframe(); ok {
					next = seq + 1
					lastKeySeq = seq
					needKey = false
					continue
				}
			}
			return
		}
		if frame.IsKeyframe() {
			lastKeySeq = seq
			needKey = false
		}
		next = seq + 1
	}
}

func resyncLive(conn net.Conn, store *VideoStore) error {
	if err := sendVideoConfig(conn, store, videoNowMS()); err != nil {
		return err
	}
	kf, _, ok := store.LatestKeyframe()
	if !ok {
		return nil
	}
	return writeVideoFrameDeadline(conn, kf)
}

// writeVideoReplay streams from the keyframe at or before startMs. A non-zero
// endMs stops at the first frame past it.
func writeVideoReplay(ctx context.Context, conn net.Conn, store *VideoStore, startMs uint64, endMs uint64) {
	seq, sps, pps, ok := store.ReplayStart(startMs)
	if !ok {
		return
	}
	ts := startMs
	if sps != nil {
		if err := EncodeVideoFrame(conn, VideoFrame{TimestampMs: ts, Flags: VideoFlagConfig, Payload: sps}); err != nil {
			return
		}
	}
	if pps != nil {
		if err := EncodeVideoFrame(conn, VideoFrame{TimestampMs: ts, Flags: VideoFlagConfig, Payload: pps}); err != nil {
			return
		}
	}

	for {
		if ctx.Err() != nil {
			return
		}
		frame, found, err := store.ReadSeq(seq)
		if err != nil {
			return
		}
		if !found {
			return
		}
		if endMs != 0 && frame.TimestampMs > endMs && frame.Flags&VideoFlagConfig == 0 {
			return
		}
		if discardableNALType(nalTypeOfAnnexB(frame.Payload)) {
			seq++
			continue
		}
		if err := EncodeVideoFrame(conn, frame); err != nil {
			return
		}
		seq++
	}
}

func sendVideoConfig(conn net.Conn, store *VideoStore, ts uint64) error {
	sps, pps := store.ConfigNALs()
	if len(sps) > 0 {
		if err := writeVideoFrameDeadline(conn, VideoFrame{TimestampMs: ts, Flags: VideoFlagConfig, Payload: sps}); err != nil {
			return err
		}
	}
	if len(pps) > 0 {
		if err := writeVideoFrameDeadline(conn, VideoFrame{TimestampMs: ts, Flags: VideoFlagConfig, Payload: pps}); err != nil {
			return err
		}
	}
	return nil
}

func writeVideoFrameDeadline(conn net.Conn, frame VideoFrame) error {
	_ = conn.SetWriteDeadline(time.Now().Add(time.Duration(videoWriteTimeoutMillis) * time.Millisecond))
	err := EncodeVideoFrame(conn, frame)
	_ = conn.SetWriteDeadline(time.Time{})
	return err
}

func isTimeout(err error) bool {
	var netErr net.Error
	return errors.As(err, &netErr) && netErr.Timeout()
}

func openVideoIngress(addr string) (net.PacketConn, error) {
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return nil, err
	}
	if err := conn.SetReadBuffer(4 * 1024 * 1024); err != nil {
		log.Printf("[photon-server] video ingress: could not raise UDP read buffer: %v", err)
	}
	return conn, nil
}
