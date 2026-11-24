package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"os"
	"slices"
	"testing"
	"time"
)

// bigNAL is an Annex-B NAL of type nalType and n bytes in all. Its bytes follow a
// pattern, so a piece taken from the wrong place shows.
func bigNAL(nalType byte, n int) []byte {
	b := make([]byte, n)
	copy(b, []byte{0x00, 0x00, 0x00, 0x01, nalType})
	for i := 5; i < n; i++ {
		b[i] = byte(i*7 + int(nalType))
	}
	return b
}

// newVideoStore opens a video store in a temporary directory. It is closed when the
// test ends.
func newVideoStore(t *testing.T) *VideoStore {
	t.Helper()
	store, err := OpenVideoStore(t.TempDir())
	if err != nil {
		t.Fatalf("open video store: %v", err)
	}
	t.Cleanup(func() { _ = store.Close() })
	return store
}

// newVideoHub returns a LAN hub that serves store as its video.
func newVideoHub(t *testing.T, store *VideoStore) *v2Hub {
	t.Helper()
	return newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), func() string { return "run-test" }, newLiveRing(liveRingMinFrames), nil, store)
}

// helloVideo dials h and completes hello. It returns the connection and the hello_ack.
func helloVideo(t *testing.T, h *v2Hub) (*wsConn, helloAck) {
	t.Helper()
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)
	if h.catalog != nil {
		readMsg(t, conn) // the catalog, sent after hello_ack
	}
	return conn, ack
}

// videoStatusMsg is a video_status as the tests read it.
type videoStatusMsg struct {
	Type  string  `json:"type"`
	Mode  string  `json:"mode"`
	TMS   uint64  `json:"t_ms"`
	Clock string  `json:"clock"`
	QID   *uint32 `json:"qid"`
}

// videoFrame is one frame as the tests read it, with its pieces put back together.
type videoFrame struct {
	TMS     uint64
	Frame   uint32
	Flags   uint8
	Payload []byte
}

// videoReader reads a client's messages. VideoChunk pieces are put together into
// frames, video_status messages are kept apart, and other text messages are kept too.
type videoReader struct {
	t        *testing.T
	conn     *wsConn
	partial  []videoChunk // the pieces of a frame that has not ended yet
	frames   []videoFrame // whole frames read and not yet taken
	statuses []videoStatusMsg
	others   [][]byte // other text messages, such as pong
	binaries int      // binary messages read so far
}

func newVideoReader(t *testing.T, conn *wsConn) *videoReader {
	return &videoReader{t: t, conn: conn}
}

// sort puts one message in its list. A frame's pieces must come in order.
func (r *videoReader) sort(typ wsMessageType, data []byte) {
	r.t.Helper()
	if typ == wsText {
		var s videoStatusMsg
		if err := json.Unmarshal(data, &s); err == nil && s.Type == "video_status" {
			r.statuses = append(r.statuses, s)
		} else {
			r.others = append(r.others, data)
		}
		return
	}
	r.binaries++
	ch, err := decodeVideoChunk(data)
	if err != nil {
		r.t.Fatalf("binary message is not a VideoChunk: %v", err)
	}
	if ch.Flags&v2VideoFirst != 0 {
		r.partial = nil // a frame that never ended is dropped, as the protocol says
	} else if len(r.partial) == 0 {
		r.t.Fatalf("piece of frame %d came with no first piece before it", ch.Frame)
	} else if first := r.partial[0]; first.Frame != ch.Frame || first.TMS != ch.TMS || first.Flags&^(v2VideoFirst|v2VideoLast) != ch.Flags&^(v2VideoFirst|v2VideoLast) {
		r.t.Fatalf("piece of frame %d joins frame %d", ch.Frame, first.Frame)
	}
	r.partial = append(r.partial, ch)
	if ch.Flags&v2VideoLast == 0 {
		return
	}
	var payload []byte
	for _, p := range r.partial {
		payload = append(payload, p.Payload...)
	}
	first := r.partial[0]
	r.frames = append(r.frames, videoFrame{
		TMS:     first.TMS,
		Frame:   first.Frame,
		Flags:   first.Flags &^ (v2VideoFirst | v2VideoLast),
		Payload: payload,
	})
	r.partial = nil
}

// readBy reads one message and sorts it. The read must finish by deadline.
func (r *videoReader) readBy(deadline time.Time) {
	r.t.Helper()
	ctx, cancel := context.WithDeadline(context.Background(), deadline)
	defer cancel()
	typ, data, err := r.conn.Read(ctx)
	if err != nil {
		r.t.Fatalf("waiting for a video message: %v", err)
	}
	r.sort(typ, data)
}

// pumpUntil reads messages until deadline, sorting them, and returns how many binary
// messages came. It must be the last read on the connection: a read that its
// deadline cut short leaves the connection unreadable.
func (r *videoReader) pumpUntil(deadline time.Time) int {
	r.t.Helper()
	before := r.binaries
	for {
		ctx, cancel := context.WithDeadline(context.Background(), deadline)
		typ, data, err := r.conn.Read(ctx)
		cancel()
		if err != nil {
			if errors.Is(err, os.ErrDeadlineExceeded) || errors.Is(err, context.DeadlineExceeded) {
				return r.binaries - before
			}
			r.t.Fatalf("read: %v", err)
		}
		r.sort(typ, data)
	}
}

// nextFrame returns the next whole frame, waiting up to five seconds for it.
func (r *videoReader) nextFrame() videoFrame {
	r.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for len(r.frames) == 0 {
		r.readBy(deadline)
	}
	f := r.frames[0]
	r.frames = r.frames[1:]
	return f
}

// waitStatus returns the first video_status that want accepts, waiting up to five
// seconds for it. Statuses before it are dropped.
func (r *videoReader) waitStatus(want func(videoStatusMsg) bool) videoStatusMsg {
	r.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		for len(r.statuses) > 0 {
			s := r.statuses[0]
			r.statuses = r.statuses[1:]
			if want(s) {
				return s
			}
		}
		r.readBy(deadline)
	}
}

// checkFrame fails unless f has the given time, flags, frame number and payload.
func checkFrame(t *testing.T, what string, f videoFrame, tms uint64, flags uint8, frame uint32, payload []byte) {
	t.Helper()
	if f.TMS != tms || f.Flags != flags || f.Frame != frame || !bytes.Equal(f.Payload, payload) {
		t.Fatalf("%s: t_ms %d flags %#x frame %d (%d bytes), want t_ms %d flags %#x frame %d (%d bytes)",
			what, f.TMS, f.Flags, f.Frame, len(f.Payload), tms, flags, frame, len(payload))
	}
}

// fixtureNAL is one NAL of replayFixture as it was stored.
type fixtureNAL struct {
	seq  uint64
	ts   uint64
	data []byte
	key  bool
}

// replayFixture fills store for the replay tests: the config NALs at 0 ms, keyframes
// at 0, 2000 and 4000 ms, and a P frame every 100 ms up to 6000 ms. It returns the
// config NALs and the frames in store order.
func replayFixture(t *testing.T, store *VideoStore) (sps, pps []byte, frames []fixtureNAL) {
	t.Helper()
	sps = annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps = annexBNAL(nalTypePPS, 0xce, 0x38)
	mustAppend(t, store, sps, 0)
	mustAppend(t, store, pps, 0)
	for ts := uint64(0); ts <= 6000; ts += 100 {
		key := ts == 0 || ts == 2000 || ts == 4000
		data := annexBNAL(1, byte(ts/100), 0x55)
		if key {
			data = annexBNAL(nalTypeIDR, byte(ts/100), 0x55)
		}
		seq := mustAppend(t, store, data, ts)
		frames = append(frames, fixtureNAL{seq: seq, ts: ts, data: data, key: key})
	}
	return sps, pps, frames
}

// TestVideoChunkRoundTrip checks the VideoChunk layout: the header, then t_ms,
// frame, flags, stream and two reserved bytes, then the payload.
func TestVideoChunkRoundTrip(t *testing.T) {
	in := videoChunk{
		TMS:     1_700_000_000_123,
		Frame:   0xdeadbeef,
		Flags:   VideoFlagKeyframe | v2VideoFirst | v2VideoReplay,
		Stream:  3,
		Payload: []byte{0x00, 0x00, 0x00, 0x01, 0x65, 0x09},
	}
	b := encodeVideoChunk(in)
	h, err := decodeV2Header(b)
	if err != nil || h.Type != v2MsgVideoChunk || h.Seq != 0 {
		t.Fatalf("header %+v err %v, want a VideoChunk with seq 0", h, err)
	}
	if got := binary.LittleEndian.Uint64(b[12:20]); got != in.TMS {
		t.Fatalf("t_ms on the wire %d, want %d", got, in.TMS)
	}
	if got := binary.LittleEndian.Uint32(b[20:24]); got != in.Frame {
		t.Fatalf("frame on the wire %#x, want %#x", got, in.Frame)
	}
	if b[24] != in.Flags || b[25] != in.Stream {
		t.Fatalf("flags %#x stream %d on the wire, want %#x and %d", b[24], b[25], in.Flags, in.Stream)
	}
	out, err := decodeVideoChunk(b)
	if err != nil {
		t.Fatal(err)
	}
	if out.TMS != in.TMS || out.Frame != in.Frame || out.Flags != in.Flags || out.Stream != in.Stream || !bytes.Equal(out.Payload, in.Payload) {
		t.Fatalf("round trip gave %+v, want %+v", out, in)
	}
	if _, err := decodeVideoChunk(b[:len(b)-1]); err == nil {
		t.Fatal("a short VideoChunk decoded")
	}
}

// TestVideoPiecesSplitFrames checks the cut: a 40 KiB frame is three pieces of at
// most 16 KiB, with the first and last bits on the right ones, and the bytes come
// back whole.
func TestVideoPiecesSplitFrames(t *testing.T) {
	frame := bigNAL(nalTypeIDR, 40<<10)
	pieces := videoPieces(1000, 7, VideoFlagKeyframe, frame)
	if len(pieces) != 3 {
		t.Fatalf("40 KiB frame gave %d pieces, want 3", len(pieces))
	}
	var got []byte
	for i, p := range pieces {
		ch, err := decodeVideoChunk(p)
		if err != nil {
			t.Fatal(err)
		}
		if len(ch.Payload) > 16<<10 {
			t.Fatalf("piece %d has %d payload bytes, more than 16 KiB", i, len(ch.Payload))
		}
		if first := ch.Flags&v2VideoFirst != 0; first != (i == 0) {
			t.Fatalf("piece %d first bit %v", i, first)
		}
		if last := ch.Flags&v2VideoLast != 0; last != (i == len(pieces)-1) {
			t.Fatalf("piece %d last bit %v", i, last)
		}
		if ch.Flags&VideoFlagKeyframe == 0 || ch.TMS != 1000 || ch.Frame != 7 {
			t.Fatalf("piece %d is %+v, want the frame's keyframe bit, time and number", i, ch)
		}
		got = append(got, ch.Payload...)
	}
	if !bytes.Equal(got, frame) {
		t.Fatal("the pieces do not join back into the frame")
	}
	for _, n := range []int{3, 16 << 10, 16<<10 + 1} {
		want := 1
		if n > 16<<10 {
			want = 2
		}
		if got := len(videoPieces(5, 1, 0, make([]byte, n))); got != want {
			t.Fatalf("a %d-byte frame gave %d pieces, want %d", n, got, want)
		}
	}
	small, err := decodeVideoChunk(videoPieces(5, 1, 0, []byte{1, 2, 3})[0])
	if err != nil {
		t.Fatal(err)
	}
	if small.Flags&(v2VideoFirst|v2VideoLast) != v2VideoFirst|v2VideoLast {
		t.Fatalf("a small frame's only piece has flags %#x, want first and last", small.Flags)
	}
}

// A live subscriber gets the config NALs, then the newest keyframe in pieces, then
// every frame after it in store order, each reassembled to the bytes the store has.
// hello_ack lists video, and video_status says live once a second.
func TestVideoLiveStreamsInOrder(t *testing.T) {
	store := newVideoStore(t)
	base := videoNowMS()
	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	idr := bigNAL(nalTypeIDR, 40<<10)
	mustAppend(t, store, sps, base)
	mustAppend(t, store, pps, base)
	idrSeq := mustAppend(t, store, idr, base)

	h := newVideoHub(t, store)
	conn, ack := helloVideo(t, h)
	if !slices.Contains(ack.Features, "video") {
		t.Fatalf("features %v, want video", ack.Features)
	}
	r := newVideoReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "live"})

	checkFrame(t, "SPS", r.nextFrame(), base, VideoFlagConfig, 0, sps)
	checkFrame(t, "PPS", r.nextFrame(), base, VideoFlagConfig, 0, pps)
	checkFrame(t, "keyframe", r.nextFrame(), base, VideoFlagKeyframe, uint32(idrSeq), idr)

	type storedNAL struct {
		seq  uint64
		ts   uint64
		data []byte
	}
	var want []storedNAL
	for i := 1; i <= 50; i++ {
		data := bigNAL(1, 400+i*1000) // 1.4 KB to 50.4 KB: some frames take several pieces
		ts := base + uint64(i)*33
		want = append(want, storedNAL{seq: mustAppend(t, store, data, ts), ts: ts, data: data})
	}
	for _, w := range want {
		checkFrame(t, "P frame", r.nextFrame(), w.ts, 0, uint32(w.seq), w.data)
	}

	st := r.waitStatus(func(s videoStatusMsg) bool { return s.Mode == "live" })
	if st.Clock != "receive" {
		t.Fatalf("clock %q, want receive: the store has no RTP clock", st.Clock)
	}
	waitUntil(t, "the frames to be counted", func() bool { return v2Metadata(h).VideoFrames == 53 })
	m := v2Metadata(h)
	wantBytes := uint64(len(sps) + len(pps) + len(idr))
	for _, w := range want {
		wantBytes += uint64(len(w.data))
	}
	if m.VideoBytes != wantBytes || m.VideoClients != 1 || m.VideoSkips != 0 {
		t.Fatalf("metadata %+v: want %d bytes, 1 video client, no skips", m, wantBytes)
	}

	// video_status comes once a second while the video streams: two in 2.2 s. This
	// is the last read, since it ends on a deadline.
	r.statuses = nil
	if n := r.pumpUntil(time.Now().Add(2200 * time.Millisecond)); n != 0 {
		t.Fatalf("%d video pieces with no new frames stored", n)
	}
	if len(r.statuses) < 2 {
		t.Fatalf("%d statuses in 2.2 s, want at least 2", len(r.statuses))
	}
}

// Until a keyframe the live stream sends nothing: a P frame stored before the first
// keyframe must not go out. The stream then starts with the config NALs and the
// keyframe.
func TestVideoLiveNeverStartsWithNonKeyframe(t *testing.T) {
	store := newVideoStore(t)
	base := videoNowMS()
	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	mustAppend(t, store, sps, base)
	mustAppend(t, store, pps, base)
	mustAppend(t, store, annexBNAL(1, 0x01), base+33)

	h := newVideoHub(t, store)
	conn, _ := helloVideo(t, h)
	r := newVideoReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "live"})
	// Wait until the stream is running and waiting at the live edge, then store a P
	// frame while it waits: that frame is read, and must not be sent.
	r.waitStatus(func(s videoStatusMsg) bool { return s.Mode == "live" })
	time.Sleep(100 * time.Millisecond)
	mustAppend(t, store, annexBNAL(1, 0x02), base+66)
	time.Sleep(100 * time.Millisecond)
	idr := annexBNAL(nalTypeIDR, 0x65, 0x77)
	idrSeq := mustAppend(t, store, idr, base+99)

	// The config NALs carry the keyframe's time, since they are sent just before it.
	checkFrame(t, "first frame", r.nextFrame(), base+99, VideoFlagConfig, 0, sps)
	checkFrame(t, "second frame", r.nextFrame(), base+99, VideoFlagConfig, 0, pps)
	checkFrame(t, "keyframe", r.nextFrame(), base+99, VideoFlagKeyframe, uint32(idrSeq), idr)
}

// A replay starts at the keyframe that covers the start time, with the config NALs
// first. Its frames come in store order, flagged as replay, and paced to real time:
// the stretch from 2000 to 6000 ms takes about 2 s to arrive, since the client may
// run 2 s ahead. At the end of the stored video, video_status says so.
func TestVideoReplayFromTime(t *testing.T) {
	store := newVideoStore(t)
	sps, pps, stored := replayFixture(t, store)
	h := newVideoHub(t, store)
	conn, _ := helloVideo(t, h)
	r := newVideoReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "replay", "from_ms": 2500})

	const replayFlags = v2VideoReplay
	checkFrame(t, "SPS", r.nextFrame(), 2000, VideoFlagConfig|replayFlags, 0, sps)
	checkFrame(t, "PPS", r.nextFrame(), 2000, VideoFlagConfig|replayFlags, 0, pps)
	start := -1
	for i, f := range stored {
		if f.ts == 2000 {
			start = i
		}
	}
	key := stored[start]
	checkFrame(t, "keyframe at 2000", r.nextFrame(), key.ts, VideoFlagKeyframe|replayFlags, uint32(key.seq), key.data)
	began := time.Now()
	for _, f := range stored[start+1:] {
		flags := uint8(replayFlags)
		if f.key {
			flags |= VideoFlagKeyframe
		}
		checkFrame(t, "replay frame", r.nextFrame(), f.ts, flags, uint32(f.seq), f.data)
	}
	if took := time.Since(began); took < 1500*time.Millisecond {
		t.Fatalf("2000 to 6000 ms of video arrived in %v: replay ran ahead of real time", took)
	}
	r.waitStatus(func(s videoStatusMsg) bool { return s.Mode == "replay" && s.TMS == 6000 })
}

// video_seek in replay jumps to t_ms. The stream then resumes at the keyframe that
// covers it, with the config first, and the next video_status echoes the qid.
func TestVideoSeekJumpsAndEchoesQID(t *testing.T) {
	store := newVideoStore(t)
	sps, pps, stored := replayFixture(t, store)
	h := newVideoHub(t, store)
	conn, _ := helloVideo(t, h)
	r := newVideoReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "replay", "from_ms": 0})
	checkFrame(t, "SPS", r.nextFrame(), 0, VideoFlagConfig|v2VideoReplay, 0, sps)
	checkFrame(t, "PPS", r.nextFrame(), 0, VideoFlagConfig|v2VideoReplay, 0, pps)
	checkFrame(t, "keyframe at 0", r.nextFrame(), 0, VideoFlagKeyframe|v2VideoReplay, uint32(stored[0].seq), stored[0].data)

	sendV2(t, conn, map[string]any{"type": "video_seek", "qid": 42, "t_ms": 4500})
	st := r.waitStatus(func(s videoStatusMsg) bool { return s.QID != nil })
	if *st.QID != 42 || st.Mode != "replay" {
		t.Fatalf("status %+v, want mode replay echoing qid 42", st)
	}

	// Frames from before the seek may still arrive; the new stream begins with the
	// config NALs at 4000 ms, which no earlier frame has.
	for {
		f := r.nextFrame()
		if f.Flags&VideoFlagConfig != 0 && f.TMS == 4000 {
			checkFrame(t, "SPS after the seek", f, 4000, VideoFlagConfig|v2VideoReplay, 0, sps)
			break
		}
	}
	checkFrame(t, "PPS after the seek", r.nextFrame(), 4000, VideoFlagConfig|v2VideoReplay, 0, pps)
	var key fixtureNAL
	var next []fixtureNAL
	for _, f := range stored {
		if f.ts == 4000 {
			key = f
		} else if f.ts > 4000 {
			next = append(next, f)
		}
	}
	checkFrame(t, "keyframe at 4000", r.nextFrame(), 4000, VideoFlagKeyframe|v2VideoReplay, uint32(key.seq), key.data)
	for _, f := range next[:5] {
		checkFrame(t, "frame after the seek", r.nextFrame(), f.ts, v2VideoReplay, uint32(f.seq), f.data)
	}
}

// video_subscribe off stops the video: the status says off, and nothing follows it.
func TestVideoOffStopsSending(t *testing.T) {
	store := newVideoStore(t)
	base := videoNowMS()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), base)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), base)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x65, 0x77), base)
	h := newVideoHub(t, store)
	conn, _ := helloVideo(t, h)
	r := newVideoReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "live"})
	r.nextFrame() // the config NALs and the keyframe, to be sure the stream is running
	r.nextFrame()
	r.nextFrame()
	for i := 1; i <= 30; i++ {
		mustAppend(t, store, bigNAL(1, 8<<10), base+uint64(i)*33)
	}

	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "off"})
	r.waitStatus(func(s videoStatusMsg) bool { return s.Mode == "off" })
	r.frames, r.partial = nil, nil // frames read before the off status come before it
	if n := r.pumpUntil(time.Now().Add(300 * time.Millisecond)); n != 0 {
		t.Fatalf("%d video pieces after the off status", n)
	}
}

// Without a video store, hello_ack has no video feature, and video_subscribe and
// video_seek get an error that says so.
func TestVideoWithoutStoreHasNoVideo(t *testing.T) {
	h := newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), func() string { return "run-test" }, newLiveRing(liveRingMinFrames), nil, nil)
	conn, ack := helloVideo(t, h)
	if slices.Contains(ack.Features, "video") {
		t.Fatalf("features %v, want no video", ack.Features)
	}
	for _, msg := range []map[string]any{
		{"type": "video_subscribe", "mode": "live"},
		{"type": "video_seek", "qid": 1, "t_ms": 5},
	} {
		sendV2(t, conn, msg)
		var e struct {
			Type    string `json:"type"`
			Code    string `json:"code"`
			Message string `json:"message"`
		}
		readInto(t, conn, &e)
		if e.Type != "error" || e.Code != "video" || e.Message != "no video" {
			t.Fatalf("answer to %v: %+v, want the no video error", msg["type"], e)
		}
	}
}

// Bad video commands get errors that name them: a mode that is not one of the three,
// a replay with no from_ms, and a seek with no replay in progress.
func TestVideoBadCommandsGetErrors(t *testing.T) {
	h := newVideoHub(t, newVideoStore(t))
	conn, _ := helloVideo(t, h)
	for _, c := range []struct {
		msg  map[string]any
		code string
	}{
		{map[string]any{"type": "video_subscribe", "mode": "sideways"}, "bad_video_subscribe"},
		{map[string]any{"type": "video_subscribe", "mode": "replay"}, "bad_video_subscribe"},
		{map[string]any{"type": "video_seek", "qid": 1, "t_ms": 5}, "bad_video_seek"},
	} {
		sendV2(t, conn, c.msg)
		var e struct {
			Type string `json:"type"`
			Code string `json:"code"`
		}
		readInto(t, conn, &e)
		if e.Type != "error" || e.Code != c.code {
			t.Fatalf("answer to %v: %+v, want error %s", c.msg, e, c.code)
		}
	}
}

// A client that reads nothing fills its bulk queue, and a pong sent to it still
// comes out before the queued video: at most the piece being written precedes it.
func TestVideoSlowClientDoesNotBlockTelemetry(t *testing.T) {
	store := newVideoStore(t)
	base := videoNowMS()
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), base)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), base)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x65, 0x77), base)
	h := newVideoHub(t, store)
	c, peer := pipeClient(t)
	runLive(c, h)
	r := newVideoReader(t, peer)
	sendV2(t, peer, map[string]any{"type": "video_subscribe", "mode": "live"})
	for i := 1; i <= 100; i++ {
		mustAppend(t, store, bigNAL(1, 4<<10), base+uint64(i))
	}
	waitUntil(t, "the bulk queue to fill", func() bool { return len(c.bulk) == cap(c.bulk) })

	before := len(c.control)
	sendV2(t, peer, map[string]any{"type": "ping", "t_ms": 7})
	waitUntil(t, "the pong to be queued", func() bool { return len(c.control) > before })

	videoBefore := r.binaries
	deadline := time.Now().Add(5 * time.Second)
	for len(r.others) == 0 {
		r.readBy(deadline)
	}
	if n := r.binaries - videoBefore; n > 1 {
		t.Fatalf("%d video pieces before the pong, want at most 1", n)
	}
	if !bytes.Contains(r.others[0], []byte(`"pong"`)) {
		t.Fatalf("text message %s, want the pong", r.others[0])
	}
	for range 30 {
		r.nextFrame() // the video goes on after the pong
	}
}

// A client that takes no piece for v2VideoStall is skipped: its queued video is
// dropped and counted, and the stream restarts with the config NALs and the newest
// keyframe, which is stored while the client is stalled.
func TestVideoLiveSkipsToKeyframeWhenClientStalls(t *testing.T) {
	store := newVideoStore(t)
	base := videoNowMS()
	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	mustAppend(t, store, sps, base)
	mustAppend(t, store, pps, base)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x65, 0x01), base)
	h := newVideoHub(t, store)
	c, peer := pipeClient(t)
	runLive(c, h)
	r := newVideoReader(t, peer)
	sendV2(t, peer, map[string]any{"type": "video_subscribe", "mode": "live"})
	for i := 1; i <= 40; i++ {
		mustAppend(t, store, bigNAL(1, 4<<10), base+uint64(i))
	}
	waitUntil(t, "the bulk queue to fill", func() bool { return len(c.bulk) == cap(c.bulk) })
	idr2 := bigNAL(nalTypeIDR, 3<<10)
	idr2Seq := mustAppend(t, store, idr2, base+100)

	waitUntil(t, "a video skip", func() bool { return h.videoSkips.Load() >= 1 })
	var prev videoFrame
	for {
		f := r.nextFrame()
		if f.Flags&VideoFlagKeyframe != 0 && f.Frame == uint32(idr2Seq) {
			break
		}
		prev = f
	}
	checkFrame(t, "the keyframe's config before it", prev, base+100, VideoFlagConfig, 0, pps)
	if h.videoSkips.Load() < 1 {
		t.Fatal("no skip counted")
	}
}
