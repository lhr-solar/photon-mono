package main

import (
	"bytes"
	"context"
	"crypto/subtle"
	"encoding/binary"
	"errors"
	"log"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

const (
	videoUdpMagic         = uint32(0x50565531) // "PVU1"
	videoUdpHeaderBytes   = 22
	videoUdpMaxPayload    = 1200
	videoUdpCmdSubscribe  = 1
	videoUdpCmdUnsubscribe = 0
	videoUdpSubTimeout    = 3 * time.Second
	videoUdpMaxSubs       = 8
)

var videoUdpRuntime struct {
	listening atomic.Bool
	subs      atomic.Int64
}

// VideoUdpPacket is one datagram on the live video fan-out.
//
//	uint32 magic      "PVU1"
//	uint32 frameSeq
//	uint16 fragIndex  0-based
//	uint16 fragCount
//	uint64 timestampMs
//	uint8  flags      same as TCP VideoFrame (key / config)
//	uint8  reserved
//	[]     payload    fragment of the Annex-B NAL
type VideoUdpPacket struct {
	FrameSeq    uint32
	FragIndex   uint16
	FragCount   uint16
	TimestampMs uint64
	Flags       uint8
	Payload     []byte
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

func DecodeVideoUdpPacket(b []byte) (VideoUdpPacket, error) {
	if len(b) < videoUdpHeaderBytes {
		return VideoUdpPacket{}, errors.New("short video udp packet")
	}
	if binary.BigEndian.Uint32(b[0:4]) != videoUdpMagic {
		return VideoUdpPacket{}, errors.New("bad video udp magic")
	}
	count := binary.BigEndian.Uint16(b[10:12])
	index := binary.BigEndian.Uint16(b[8:10])
	if count == 0 || index >= count {
		return VideoUdpPacket{}, errors.New("bad video udp fragment")
	}
	payload := append([]byte(nil), b[videoUdpHeaderBytes:]...)
	return VideoUdpPacket{
		FrameSeq:    binary.BigEndian.Uint32(b[4:8]),
		FragIndex:   index,
		FragCount:   count,
		TimestampMs: binary.BigEndian.Uint64(b[12:20]),
		Flags:       b[20],
		Payload:     payload,
	}, nil
}

type videoUdpAssembler struct {
	seq     uint32
	count   uint16
	got     uint16
	ts      uint64
	flags   uint8
	parts   [][]byte
	have    []bool
}

func (a *videoUdpAssembler) add(p VideoUdpPacket) (VideoFrame, bool) {
	if a.parts == nil || a.seq != p.FrameSeq || a.count != p.FragCount {
		a.seq = p.FrameSeq
		a.count = p.FragCount
		a.ts = p.TimestampMs
		a.flags = p.Flags
		a.parts = make([][]byte, p.FragCount)
		a.have = make([]bool, p.FragCount)
		a.got = 0
	}
	if int(p.FragIndex) >= len(a.parts) || a.have[p.FragIndex] {
		return VideoFrame{}, false
	}
	a.have[p.FragIndex] = true
	a.parts[p.FragIndex] = p.Payload
	a.got++
	if a.got < a.count {
		return VideoFrame{}, false
	}
	var payload []byte
	for _, part := range a.parts {
		payload = append(payload, part...)
	}
	a.parts = nil
	return VideoFrame{TimestampMs: a.ts, Flags: a.flags, Payload: payload}, true
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

func (f *VideoUdpFanout) SetPassword(password string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.want = []byte(password)
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

func EncodeVideoUdpSubscribe(password string, subscribe bool) ([]byte, error) {
	var buf bytes.Buffer
	if err := WriteClientAuth(&buf, password); err != nil {
		return nil, err
	}
	cmd := byte(videoUdpCmdUnsubscribe)
	if subscribe {
		cmd = videoUdpCmdSubscribe
	}
	buf.WriteByte(cmd)
	return buf.Bytes(), nil
}
