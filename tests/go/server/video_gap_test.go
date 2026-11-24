package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// gapInfo is the gap object of a video_status.
type gapInfo struct {
	FromMS uint64 `json:"from_ms"`
	ToMS   uint64 `json:"to_ms"`
	Reason string `json:"reason"`
}

// gapStatusMsg is a video_status as the gap tests read it.
type gapStatusMsg struct {
	Type string   `json:"type"`
	Mode string   `json:"mode"`
	TMS  uint64   `json:"t_ms"`
	QID  *uint32  `json:"qid"`
	Gap  *gapInfo `json:"gap"`
}

// gapReader reads a client's messages on its own goroutine, so a wait can end at a
// deadline without cancelling a read on the connection. Binary pieces are put
// together by a videoReader; video_status messages are kept with their gap.
type gapReader struct {
	t        *testing.T
	v        *videoReader
	msgs     chan gapMsg
	statuses []gapStatusMsg
}

type gapMsg struct {
	typ  wsMessageType
	data []byte
	err  error
}

func newGapReader(t *testing.T, conn *wsConn) *gapReader {
	g := &gapReader{t: t, v: newVideoReader(t, conn), msgs: make(chan gapMsg, 4096)}
	go func() {
		for {
			typ, data, err := conn.Read(context.Background())
			g.msgs <- gapMsg{typ: typ, data: data, err: err}
			if err != nil {
				return
			}
		}
	}()
	return g
}

// pump handles the next message, and returns false when deadline comes first.
func (g *gapReader) pump(deadline time.Time) bool {
	g.t.Helper()
	select {
	case m := <-g.msgs:
		if m.err != nil {
			g.t.Fatalf("read: %v", m.err)
		}
		if m.typ == wsText {
			var s gapStatusMsg
			if err := json.Unmarshal(m.data, &s); err == nil && s.Type == "video_status" {
				g.statuses = append(g.statuses, s)
			}
			return true
		}
		g.v.sort(m.typ, m.data)
		return true
	case <-time.After(time.Until(deadline)):
		return false
	}
}

// nextFrame returns the next whole frame, waiting up to five seconds for it.
func (g *gapReader) nextFrame() videoFrame {
	g.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for len(g.v.frames) == 0 {
		if !g.pump(deadline) {
			g.t.Fatal("no video frame within 5 s")
		}
	}
	f := g.v.frames[0]
	g.v.frames = g.v.frames[1:]
	return f
}

// waitStatus returns the first video_status that want accepts, waiting up to five
// seconds for it. Statuses before it are dropped.
func (g *gapReader) waitStatus(want func(gapStatusMsg) bool) gapStatusMsg {
	g.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		for len(g.statuses) > 0 {
			s := g.statuses[0]
			g.statuses = g.statuses[1:]
			if want(s) {
				return s
			}
		}
		if !g.pump(deadline) {
			g.t.Fatal("no matching video_status within 5 s")
		}
	}
}

// quietUntil reads until deadline and returns how many video pieces came.
func (g *gapReader) quietUntil(deadline time.Time) int {
	g.t.Helper()
	before := g.v.binaries
	for g.pump(deadline) {
	}
	return g.v.binaries - before
}

// TestVideoReplayReportsHoleAsGap: a replay that reaches a 10 s hole reports it as a
// gap of reason none, from the last picture before the hole to the first keyframe
// after it, before it waits through the hole.
func TestVideoReplayReportsHoleAsGap(t *testing.T) {
	store := newVideoStore(t)
	mustAppend(t, store, annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a), 0)
	mustAppend(t, store, annexBNAL(nalTypePPS, 0xce, 0x38), 0)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01, 0x55), 0)
	for ts := uint64(100); ts <= 1900; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x55), ts)
	}
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x02, 0x55), 12_000)
	for ts := uint64(12_100); ts <= 12_500; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x66), ts)
	}

	h := newVideoHub(t, store)
	conn, _ := helloVideo(t, h)
	r := newGapReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "replay", "from_ms": 0})
	st := r.waitStatus(func(s gapStatusMsg) bool { return s.Gap != nil })
	if st.Gap.Reason != "none" || st.Gap.FromMS != 1900 || st.Gap.ToMS != 12_000 {
		t.Fatalf("gap %+v, want reason none from 1900 to 12000", *st.Gap)
	}
	if st.Mode != "replay" {
		t.Fatalf("mode %q during the gap, want replay", st.Mode)
	}
}

// TestVideoSeekIntoHoleReportsGapThenNextKeyframe: a seek into a hole reports the gap
// and then continues from the keyframe after it, with its config first.
func TestVideoSeekIntoHoleReportsGapThenNextKeyframe(t *testing.T) {
	store := newVideoStore(t)
	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	mustAppend(t, store, sps, 0)
	mustAppend(t, store, pps, 0)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01, 0x55), 0)
	for ts := uint64(100); ts <= 1900; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x55), ts)
	}
	idr := annexBNAL(nalTypeIDR, 0x02, 0x55)
	idrSeq := mustAppend(t, store, idr, 12_000)
	for ts := uint64(12_100); ts <= 12_500; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x66), ts)
	}

	h := newVideoHub(t, store)
	conn, _ := helloVideo(t, h)
	r := newGapReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "replay", "from_ms": 0})
	r.nextFrame() // the first keyframe, so the replay is running

	sendV2(t, conn, map[string]any{"type": "video_seek", "qid": 9, "t_ms": 6000})
	st := r.waitStatus(func(s gapStatusMsg) bool { return s.QID != nil })
	if *st.QID != 9 {
		t.Fatalf("status echoes qid %d, want 9", *st.QID)
	}
	gap := r.waitStatus(func(s gapStatusMsg) bool { return s.Gap != nil })
	if gap.Gap.Reason != "none" || gap.Gap.FromMS != 1900 || gap.Gap.ToMS != 12_000 {
		t.Fatalf("seek gap %+v, want reason none from 1900 to 12000", *gap.Gap)
	}

	// Frames from before the seek may still come. The new stream starts with the
	// config at 12 000 ms, then the keyframe there.
	for {
		f := r.nextFrame()
		if f.Flags&VideoFlagConfig != 0 && f.TMS == 12_000 {
			checkFrame(t, "SPS after the hole", f, 12_000, VideoFlagConfig|v2VideoReplay, 0, sps)
			break
		}
	}
	checkFrame(t, "PPS after the hole", r.nextFrame(), 12_000, VideoFlagConfig|v2VideoReplay, 0, pps)
	checkFrame(t, "keyframe after the hole", r.nextFrame(), 12_000, VideoFlagKeyframe|v2VideoReplay, uint32(idrSeq), idr)
}

// TestVideoReplayReportsArchivedSegment: a segment whose .h264 is not on disk is
// reported as archived over its time range, and replay goes on from the keyframe
// after it.
func TestVideoReplayReportsArchivedSegment(t *testing.T) {
	dir := t.TempDir()
	store, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	mustAppend(t, store, sps, 1000)
	mustAppend(t, store, pps, 1000)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01, 0x55), 1000)
	for ts := uint64(1100); ts <= 3000; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x55), ts)
	}
	// Past 60 s, on a keyframe: segment 1 is sealed and segment 2 starts with its config.
	idr := annexBNAL(nalTypeIDR, 0x02, 0x66)
	idrSeq := mustAppend(t, store, idr, 61_000)
	for ts := uint64(61_100); ts <= 61_500; ts += 100 {
		mustAppend(t, store, annexBNAL(1, byte(ts/100), 0x66), ts)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	// Archive segment 1: its data leaves the disk, its keyframe file stays.
	if err := os.Remove(videoDataPath(filepath.Join(dir, "video", "segments"), 1)); err != nil {
		t.Fatal(err)
	}
	reopened, err := OpenVideoStore(dir)
	if err != nil {
		t.Fatalf("reopen with an archived segment: %v", err)
	}
	defer reopened.Close()

	h := newVideoHub(t, reopened)
	conn, _ := helloVideo(t, h)
	r := newGapReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "replay", "from_ms": 0})
	st := r.waitStatus(func(s gapStatusMsg) bool { return s.Gap != nil })
	if st.Gap.Reason != "archived" || st.Gap.FromMS != 1000 || st.Gap.ToMS != 3000 {
		t.Fatalf("gap %+v, want reason archived over 1000 to 3000", *st.Gap)
	}
	checkFrame(t, "SPS after the archived segment", r.nextFrame(), 61_000, VideoFlagConfig|v2VideoReplay, 0, sps)
	checkFrame(t, "PPS after the archived segment", r.nextFrame(), 61_000, VideoFlagConfig|v2VideoReplay, 0, pps)
	checkFrame(t, "keyframe after the archived segment", r.nextFrame(), 61_000,
		VideoFlagKeyframe|v2VideoReplay, uint32(idrSeq), idr)
}

// TestVideoLiveStaleKeyframeWaitsForNew: a live subscriber is never started on a
// keyframe more than 2.5 s old. It is told the camera has been silent since the
// newest frame, gets no video, and starts when a new keyframe arrives.
func TestVideoLiveStaleKeyframeWaitsForNew(t *testing.T) {
	store := newVideoStore(t)
	old := videoNowMS() - 5000
	sps := annexBNAL(nalTypeSPS, 0x42, 0x00, 0x0a)
	pps := annexBNAL(nalTypePPS, 0xce, 0x38)
	mustAppend(t, store, sps, old)
	mustAppend(t, store, pps, old)
	mustAppend(t, store, annexBNAL(nalTypeIDR, 0x01, 0x77), old)
	mustAppend(t, store, annexBNAL(1, 0x02, 0x77), old+33)

	h := newVideoHub(t, store)
	conn, _ := helloVideo(t, h)
	r := newGapReader(t, conn)
	sendV2(t, conn, map[string]any{"type": "video_subscribe", "mode": "live"})
	st := r.waitStatus(func(s gapStatusMsg) bool { return s.Gap != nil })
	if st.Gap.Reason != "stale" || st.Gap.ToMS != 0 || st.Gap.FromMS != old+33 {
		t.Fatalf("gap %+v, want reason stale from %d to 0", *st.Gap, old+33)
	}
	if n := r.quietUntil(time.Now().Add(300 * time.Millisecond)); n != 0 {
		t.Fatalf("%d video pieces before a fresh keyframe", n)
	}

	now := videoNowMS()
	idr := annexBNAL(nalTypeIDR, 0x03, 0x88)
	idrSeq := mustAppend(t, store, idr, now)
	checkFrame(t, "SPS", r.nextFrame(), now, VideoFlagConfig, 0, sps)
	checkFrame(t, "PPS", r.nextFrame(), now, VideoFlagConfig, 0, pps)
	checkFrame(t, "keyframe", r.nextFrame(), now, VideoFlagKeyframe, uint32(idrSeq), idr)
	r.waitStatus(func(s gapStatusMsg) bool { return s.Mode == "live" && s.Gap == nil })
}
