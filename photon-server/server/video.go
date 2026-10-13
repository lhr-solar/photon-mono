package main

import (
	"bytes"
	"context"
	"crypto/subtle"
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

const (
	videoUdpMagic          = uint32(0x50565531) // "PVU1"
	videoUdpHeaderBytes    = 22
	videoUdpMaxPayload     = 1200
	videoUdpCmdSubscribe   = 1
	videoUdpCmdUnsubscribe = 0
	videoUdpSubTimeout     = 3 * time.Second
	videoUdpMaxSubs        = 8
)

var videoUdpRuntime struct {
	listening atomic.Bool
	subs      atomic.Int64
}

func EncodeVideoUdpPackets(seq uint32, frame VideoFrame) [][]byte {
	n := len(frame.Payload)
	if n == 0 {
		n = 1
	}
	count := (n + videoUdpMaxPayload - 1) / videoUdpMaxPayload
	if count < 1 {
		count = 1
	}
	out := make([][]byte, 0, count)
	off := 0
	for i := 0; i < count; i++ {
		end := off + videoUdpMaxPayload
		if end > len(frame.Payload) {
			end = len(frame.Payload)
		}
		pkt := make([]byte, videoUdpHeaderBytes+(end-off))
		binary.BigEndian.PutUint32(pkt[0:4], videoUdpMagic)
		binary.BigEndian.PutUint32(pkt[4:8], seq)
		binary.BigEndian.PutUint16(pkt[8:10], uint16(i))
		binary.BigEndian.PutUint16(pkt[10:12], uint16(count))
		binary.BigEndian.PutUint64(pkt[12:20], frame.TimestampMs)
		pkt[20] = frame.Flags
		pkt[21] = 0
		copy(pkt[videoUdpHeaderBytes:], frame.Payload[off:end])
		out = append(out, pkt)
		off = end
	}
	return out
}

type udpSub struct {
	addr    net.Addr
	seen    time.Time
	needKey bool
}

// VideoUdpFanout copies the live NAL tail to UDP subscribers. Clients punch
// NAT by sending a PHAU+cmd datagram first. This is not camera ingress :6200.
type VideoUdpFanout struct {
	conn  net.PacketConn
	store *VideoStore
	want  []byte

	mu   sync.Mutex
	subs map[string]*udpSub
	seq  uint32
}

func NewVideoUdpFanout(conn net.PacketConn, store *VideoStore, password string) *VideoUdpFanout {
	return &VideoUdpFanout{
		conn:  conn,
		store: store,
		want:  []byte(password),
		subs:  make(map[string]*udpSub),
	}
}

func (f *VideoUdpFanout) SubCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.subs)
}

func (f *VideoUdpFanout) Run(ctx context.Context) {
	if f == nil || f.conn == nil {
		return
	}
	videoUdpRuntime.listening.Store(true)
	defer videoUdpRuntime.listening.Store(false)

	go f.readLoop(ctx)
	f.liveLoop(ctx)
}

func (f *VideoUdpFanout) readLoop(ctx context.Context) {
	buf := make([]byte, 2048)
	for {
		_ = f.conn.SetReadDeadline(time.Now().Add(400 * time.Millisecond))
		n, addr, err := f.conn.ReadFrom(buf)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			continue
		}
		if n == 0 || addr == nil {
			continue
		}
		cmd, ok := f.authCmd(buf[:n])
		if !ok {
			continue
		}
		key := addr.String()
		f.mu.Lock()
		if cmd == videoUdpCmdUnsubscribe {
			delete(f.subs, key)
			videoUdpRuntime.subs.Store(int64(len(f.subs)))
			f.mu.Unlock()
			continue
		}
		sub, exists := f.subs[key]
		if !exists {
			if len(f.subs) >= videoUdpMaxSubs {
				f.mu.Unlock()
				continue
			}
			sub = &udpSub{addr: addr, needKey: true}
			f.subs[key] = sub
			videoUdpRuntime.subs.Store(int64(len(f.subs)))
			log.Printf("[photon-server] video udp subscribe %s n=%d", key, len(f.subs))
		}
		sub.addr = addr
		sub.seen = time.Now()
		seed := !exists
		f.mu.Unlock()
		if seed {
			f.seed(sub)
		}
	}
}

func (f *VideoUdpFanout) authCmd(b []byte) (uint8, bool) {
	r := bytes.NewReader(b)
	got, err := ReadClientAuth(r)
	if err != nil {
		return 0, false
	}
	if subtle.ConstantTimeCompare(f.want, []byte(got)) != 1 {
		return 0, false
	}
	cmd, err := r.ReadByte()
	if err != nil {
		return 0, false
	}
	return cmd, cmd == videoUdpCmdSubscribe || cmd == videoUdpCmdUnsubscribe
}

func (f *VideoUdpFanout) liveLoop(ctx context.Context) {
	var next uint64
	var lastKeySeq uint64
	needKey := true
	for {
		if ctx.Err() != nil {
			return
		}
		if f.SubCount() == 0 {
			select {
			case <-ctx.Done():
				return
			case <-time.After(200 * time.Millisecond):
			}
			needKey = true
			next = 0
			continue
		}
		if next == 0 {
			now := videoNowMS()
			if kf, seq, ok := f.store.LatestKeyframe(); ok {
				age := int64(now) - int64(kf.TimestampMs)
				if age >= 0 && age <= int64(videoLiveKeyframeFreshMs) {
					f.broadcast(kf)
					next = seq + 1
					lastKeySeq = seq
					needKey = false
				}
			}
			if next == 0 {
				next = f.store.NextSeq()
				if next == 0 {
					next = 1
				}
			}
		}
		type waited struct {
			frame VideoFrame
			seq   uint64
			err   error
		}
		ch := make(chan waited, 1)
		go func(want uint64) {
			frame, seq, err := f.store.ReadOrWait(want)
			ch <- waited{frame, seq, err}
		}(next)
		var frame VideoFrame
		var seq uint64
		var err error
		select {
		case <-ctx.Done():
			return
		case w := <-ch:
			frame, seq, err = w.frame, w.seq, w.err
		}
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		if seq < next {
			next = f.store.NextSeq()
			needKey = true
			continue
		}
		if discardableNALType(nalTypeOfAnnexB(frame.Payload)) {
			next = seq + 1
			continue
		}
		if frame.IsKeyframe() && seq == lastKeySeq {
			next = seq + 1
			needKey = true
			continue
		}
		if needKey && !frame.IsKeyframe() && !frame.IsConfig() {
			next = seq + 1
			continue
		}
		f.broadcast(frame)
		if frame.IsKeyframe() {
			lastKeySeq = seq
			needKey = false
		}
		next = seq + 1
	}
}

func (f *VideoUdpFanout) seed(sub *udpSub) {
	if f.store == nil {
		return
	}
	ts := videoNowMS()
	sps, pps := f.store.ConfigNALs()
	if len(sps) > 0 {
		f.sendTo(sub.addr, VideoFrame{TimestampMs: ts, Flags: VideoFlagConfig, Payload: sps})
	}
	if len(pps) > 0 {
		f.sendTo(sub.addr, VideoFrame{TimestampMs: ts, Flags: VideoFlagConfig, Payload: pps})
	}
	if kf, _, ok := f.store.LatestKeyframe(); ok {
		age := int64(ts) - int64(kf.TimestampMs)
		if age >= 0 && age <= int64(videoLiveKeyframeFreshMs) {
			f.sendTo(sub.addr, kf)
			sub.needKey = false
		}
	}
}

func (f *VideoUdpFanout) broadcast(frame VideoFrame) {
	now := time.Now()
	f.mu.Lock()
	addrs := make([]net.Addr, 0, len(f.subs))
	for key, sub := range f.subs {
		if now.Sub(sub.seen) > videoUdpSubTimeout {
			delete(f.subs, key)
			continue
		}
		if sub.needKey && !frame.IsKeyframe() && !frame.IsConfig() {
			continue
		}
		if frame.IsKeyframe() {
			sub.needKey = false
		}
		addrs = append(addrs, sub.addr)
	}
	videoUdpRuntime.subs.Store(int64(len(f.subs)))
	seq := f.seq
	f.seq++
	f.mu.Unlock()
	pkts := EncodeVideoUdpPackets(seq, frame)
	for _, addr := range addrs {
		for _, pkt := range pkts {
			_ = f.conn.SetWriteDeadline(time.Now().Add(20 * time.Millisecond))
			_, _ = f.conn.WriteTo(pkt, addr)
		}
	}
}

func (f *VideoUdpFanout) sendTo(addr net.Addr, frame VideoFrame) {
	f.mu.Lock()
	seq := f.seq
	f.seq++
	f.mu.Unlock()
	for _, pkt := range EncodeVideoUdpPackets(seq, frame) {
		_ = f.conn.SetWriteDeadline(time.Now().Add(20 * time.Millisecond))
		_, _ = f.conn.WriteTo(pkt, addr)
	}
}

const (
	// VideoFlagKeyframe marks an IDR NAL (bit 0).
	VideoFlagKeyframe uint8 = 1 << 0
	// VideoFlagConfig marks an SPS or PPS NAL (bit 1).
	VideoFlagConfig uint8 = 1 << 1

	VideoModeLive   uint8 = 0
	VideoModeReplay uint8 = 1

	videoFrameHeaderBytes   = 13 // uint32 len + uint64 ts + uint8 flags
	videoRequestBytes       = 9  // uint8 mode + uint64 startMs
	maxVideoFramePayload    = maxNALBytes
	videoWriteTimeoutMillis = 250
	// Catch up a long camera GOP (2–8 s at 30 fps) without falling back to
	// the same IDR. The old 45-NAL cap rewound every real-camera GOP.
	videoLiveMaxLag = 240
	// Live subscribe may seed one recent IDR. Older keyframes are leftover
	// synth or a previous session and must not start the live tail.
	videoLiveKeyframeFreshMs = 2500
)

// VideoFrame is one framed NAL on the video client protocol.
//
// Wire format (big-endian), documented in README:
//
//	uint32  payloadLen     length of the NAL payload only
//	uint64  timestampMs    wall-clock unix milliseconds
//	uint8   flags          bit0 = keyframe (IDR), bit1 = config (SPS/PPS)
//	[payloadLen] payload   Annex-B NAL including 00 00 00 01 start code
type VideoFrame struct {
	TimestampMs uint64
	Flags       uint8
	Payload     []byte
}

func (f VideoFrame) IsKeyframe() bool { return f.Flags&VideoFlagKeyframe != 0 }
func (f VideoFrame) IsConfig() bool   { return f.Flags&VideoFlagConfig != 0 }

func EncodeVideoFrame(w io.Writer, f VideoFrame) error {
	if len(f.Payload) > maxVideoFramePayload {
		return errors.New("video frame payload exceeds maximum size")
	}
	var hdr [videoFrameHeaderBytes]byte
	binary.BigEndian.PutUint32(hdr[0:4], uint32(len(f.Payload)))
	binary.BigEndian.PutUint64(hdr[4:12], f.TimestampMs)
	hdr[12] = f.Flags
	if _, err := w.Write(hdr[:]); err != nil {
		return err
	}
	if len(f.Payload) == 0 {
		return nil
	}
	_, err := w.Write(f.Payload)
	return err
}

// VideoRequest is the 9-byte subscribe header a client writes immediately after connect.
//
//	uint8   mode          0 = live, 1 = replay
//	uint64  startTimeMs   replay start (unix ms); ignored for live
type VideoRequest struct {
	Mode        uint8
	StartTimeMs uint64
}

// Extended subscribe modes. Modes 0 and 1 keep their exact 9-byte header so
// existing clients are unaffected; the mode byte selects the length, which is
// what makes growing the header backward-compatible (§8.2).
const (
	VideoModeReplayRange = uint8(2) // replay bounded by endTimeMs
	VideoModeLocate      = uint8(3) // video_locate
	VideoModeRange       = uint8(4) // byte-range read
	VideoModeClock       = uint8(5) // persist manual A/V nudge

	videoRangeMaxBytes = uint32(32 * 1024 * 1024)

	videoLocateStatusOK       = uint8(0)
	videoLocateStatusNotFound = uint8(1)
	videoLocateStatusError    = uint8(2)

	// Consecutive IDRs are 1–2 s apart. A cursor this far past a GOP is a
	// recording hole, not a frame-interval gap. Do not raise this to "until
	// EOF": that reintroduces the last-clip freeze.
	videoLocateHoldMs = uint64(2500)
)

// VideoRequestExt is the decoded subscribe header for any mode.
type VideoRequestExt struct {
	Mode        uint8
	StartTimeMs uint64
	EndTimeMs   uint64
	StreamID    uint8
	Prefetch    uint16
	SegmentID   uint32
	Offset      uint64
	Length      uint32
}

// DecodeVideoRequestExt reads the mode byte, then the remainder that mode
// implies. Modes 0 and 1 consume exactly videoRequestBytes.
func DecodeVideoRequestExt(r io.Reader) (VideoRequestExt, error) {
	var head [videoRequestBytes]byte
	if _, err := io.ReadFull(r, head[:]); err != nil {
		return VideoRequestExt{}, err
	}
	req := VideoRequestExt{
		Mode:        head[0],
		StartTimeMs: binary.BigEndian.Uint64(head[1:9]),
	}
	switch req.Mode {
	case VideoModeLive, VideoModeReplay:
		return req, nil
	case VideoModeReplayRange:
		var tail [8]byte
		if _, err := io.ReadFull(r, tail[:]); err != nil {
			return VideoRequestExt{}, err
		}
		req.EndTimeMs = binary.BigEndian.Uint64(tail[:])
		return req, nil
	case VideoModeLocate:
		var tail [3]byte
		if _, err := io.ReadFull(r, tail[:]); err != nil {
			return VideoRequestExt{}, err
		}
		req.StreamID = tail[0]
		req.Prefetch = binary.BigEndian.Uint16(tail[1:3])
		return req, nil
	case VideoModeClock:
		// StartTimeMs carries the signed offset as two's-complement bits.
		return req, nil
	case VideoModeRange:
		// StartTimeMs is unused here; the span is named explicitly.
		var tail [16]byte
		if _, err := io.ReadFull(r, tail[:]); err != nil {
			return VideoRequestExt{}, err
		}
		req.SegmentID = binary.BigEndian.Uint32(tail[0:4])
		req.Offset = binary.BigEndian.Uint64(tail[4:12])
		req.Length = binary.BigEndian.Uint32(tail[12:16])
		return req, nil
	default:
		return req, nil
	}
}

// EncodeVideoLocateReply serialises a locate answer.
//
//	u32  payloadLen        bytes that follow
//	u8   status
//	i64  clockOffsetMs
//	u8   clockProvenance
//	i64  residualMs
//	u32  totalRows
//	u64  minMs
//	u64  maxMs
//	u16  spsLen, sps
//	u16  ppsLen, pps
//	u16  rowCount, rows       entry first, then prefetch, 40 B each
//	u16  coverageCount, cov   u32 segmentId + u64 startMs + u64 endMs
func EncodeVideoLocateReply(w io.Writer, res VideoLocateResult) error {
	body := make([]byte, 0, 128+len(res.Prefetch)*vptRowSize+len(res.Coverage)*20)
	status := videoLocateStatusOK
	if !res.Found {
		status = videoLocateStatusNotFound
	}
	body = append(body, status)
	body = vptAppendU64(body, uint64(res.ClockOffsetMs))
	body = append(body, res.ClockProv)
	body = vptAppendU64(body, uint64(res.ResidualMs))
	body = vptAppendU32(body, res.TotalRows)
	body = vptAppendU64(body, res.MinMs)
	body = vptAppendU64(body, res.MaxMs)
	body = vptAppendBytes(body, res.SPS)
	body = vptAppendBytes(body, res.PPS)

	rows := make([]GopRow, 0, 1+len(res.Prefetch))
	if res.Found {
		rows = append(rows, res.Entry)
		rows = append(rows, res.Prefetch...)
	}
	body = vptAppendU16(body, uint16(len(rows)))
	for _, r := range rows {
		var rowBuf [vptRowSize]byte
		encodeGopRow(rowBuf[:], r)
		body = append(body, rowBuf[:]...)
	}
	body = vptAppendU16(body, uint16(len(res.Coverage)))
	for _, c := range res.Coverage {
		body = vptAppendU32(body, c.SegmentID)
		body = vptAppendU64(body, c.StartMs)
		body = vptAppendU64(body, c.EndMs)
	}

	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(body)))
	if _, err := w.Write(lenBuf[:]); err != nil {
		return err
	}
	_, err := w.Write(body)
	return err
}

// EncodeVideoRangeReply serialises a byte-range answer: u32 length + bytes.
// A zero length with a non-OK status byte would be ambiguous, so failures are
// reported by closing without a reply, matching how the live path signals a
// dead subscription.
func EncodeVideoClockReply(w io.Writer, offsetMs int64, prov uint8) error {
	body := make([]byte, 0, 9)
	body = vptAppendU64(body, uint64(offsetMs))
	body = append(body, prov)
	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(body)))
	if _, err := w.Write(lenBuf[:]); err != nil {
		return err
	}
	_, err := w.Write(body)
	return err
}

func EncodeVideoRangeReply(w io.Writer, payload []byte) error {
	var lenBuf [4]byte
	binary.BigEndian.PutUint32(lenBuf[:], uint32(len(payload)))
	if _, err := w.Write(lenBuf[:]); err != nil {
		return err
	}
	if len(payload) == 0 {
		return nil
	}
	_, err := w.Write(payload)
	return err
}

func vptAppendU16(dst []byte, v uint16) []byte {
	var b [2]byte
	binary.BigEndian.PutUint16(b[:], v)
	return append(dst, b[:]...)
}

func vptAppendU32(dst []byte, v uint32) []byte {
	var b [4]byte
	binary.BigEndian.PutUint32(b[:], v)
	return append(dst, b[:]...)
}

func vptAppendU64(dst []byte, v uint64) []byte {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], v)
	return append(dst, b[:]...)
}

func vptAppendBytes(dst []byte, payload []byte) []byte {
	dst = vptAppendU16(dst, uint16(len(payload)))
	return append(dst, payload...)
}
