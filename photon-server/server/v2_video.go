package main

import (
	"encoding/json"
	"errors"
	"sync"
	"time"
)

// Protocol v2 video. The store's frames go to a client as VideoChunk pieces on its
// bulk queue, which is sent only when nothing else is queued. Each client has one
// pusher goroutine, which plays the mode it was last given: live, replay or off. The
// messages are described in photon-next/docs/protocol-v2.md.

const (
	v2VideoLead        = 2 * time.Second // replay may run this far ahead of real time, so the client can buffer
	v2VideoStall       = 2 * time.Second // live video skips to the newest keyframe when the client takes no piece this long
	v2VideoStatusEvery = time.Second     // how often video_status is sent while video streams
)

// The modes of video_subscribe.
const (
	videoOff    = "off"
	videoLive   = "live"
	videoReplay = "replay"
)

// videoCmd is a request to a client's pusher: a video_subscribe, or a video_seek,
// which is a replay jump with the qid to echo.
type videoCmd struct {
	mode   string // videoOff, videoLive or videoReplay
	fromMS int64  // where a replay starts, Unix ms
	qid    uint32 // from a video_seek, echoed in the next video_status
	seek   bool   // the command is a video_seek
}

// videoStop says why a send or a wait ended before it finished.
type videoStop int

const (
	stopNone    videoStop = iota // it finished
	stopCmd                      // a command arrived; the pusher's held field has it
	stopGone                     // the client is done
	stopStalled                  // the client took no piece for v2VideoStall
)

// videoPusher is one client's video goroutine. Commands reach it on cmds. The mode,
// the last frame time and a pending qid are read by the status messages, under mu.
type videoPusher struct {
	cmds   chan videoCmd // the next command; a newer one replaces it
	held   videoCmd      // the command a wait took from cmds; only the pusher reads it
	stop   chan struct{} // closed when the pusher ends, which ends its status loop
	exited chan struct{} // closed when the pusher has returned

	mu   sync.Mutex
	mode string      // videoOff, videoLive or videoReplay
	tMS  uint64      // time of the last frame queued
	qid  uint32      // the qid of the last video_seek
	echo bool        // qid has not been echoed in a status yet
	gap  *v2VideoGap // the gap the statuses report, nil when there is none
}

// v2VideoGap is the span a video_status reports with no video: from_ms to to_ms,
// and why. reason is "none" (nothing was recorded), "archived" (the segment is
// stored elsewhere) or "stale" (live: the camera has been silent since from_ms).
type v2VideoGap struct {
	FromMS uint64 `json:"from_ms"`
	ToMS   uint64 `json:"to_ms"`
	Reason string `json:"reason"`
}

// newVideoPusher returns a pusher with no command yet.
func newVideoPusher() *videoPusher {
	return &videoPusher{cmds: make(chan videoCmd, 1), stop: make(chan struct{}), exited: make(chan struct{})}
}

// post hands cmd to the pusher, replacing a command it has not taken yet. Only the
// client's reader posts, under videoMu, so the buffer is empty after the drain.
func (p *videoPusher) post(cmd videoCmd) {
	select {
	case <-p.cmds:
	default:
	}
	p.cmds <- cmd
}

// leave returns what a stopped mode hands back: the command it stopped for, or false
// when the client is gone.
func (p *videoPusher) leave(stop videoStop) (videoCmd, bool) {
	if stop == stopCmd {
		return p.held, true
	}
	return videoCmd{}, false
}

// getMode returns the pusher's mode.
func (p *videoPusher) getMode() string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.mode
}

// v2VideoStatus is the video_status message.
type v2VideoStatus struct {
	Type  string      `json:"type"`
	Mode  string      `json:"mode"`
	TMS   uint64      `json:"t_ms"`
	Clock string      `json:"clock"`
	QID   *uint32     `json:"qid,omitempty"`
	Gap   *v2VideoGap `json:"gap,omitempty"`
}

// subscribeVideo handles video_subscribe: live, replay from from_ms, or off.
func (c *v2Client) subscribeVideo(data []byte) {
	if c.hub.video == nil {
		c.enqueueError("video", "no video")
		return
	}
	var msg struct {
		Mode   string `json:"mode"`
		FromMS *int64 `json:"from_ms"`
	}
	if err := json.Unmarshal(data, &msg); err != nil {
		c.enqueueError("bad_video_subscribe", "video_subscribe needs a mode")
		return
	}
	cmd := videoCmd{mode: msg.Mode}
	switch msg.Mode {
	case videoOff, videoLive:
	case videoReplay:
		if msg.FromMS == nil {
			c.enqueueError("bad_video_subscribe", "replay needs from_ms")
			return
		}
		cmd.fromMS = *msg.FromMS
	default:
		c.enqueueError("bad_video_subscribe", "video mode must be live, replay or off")
		return
	}
	c.videoCommand(cmd)
}

// seekVideo handles video_seek: in replay, a jump to t_ms.
func (c *v2Client) seekVideo(data []byte) {
	if c.hub.video == nil {
		c.enqueueError("video", "no video")
		return
	}
	var msg struct {
		QID uint32 `json:"qid"`
		TMS *int64 `json:"t_ms"`
	}
	if err := json.Unmarshal(data, &msg); err != nil || msg.TMS == nil {
		c.enqueueError("bad_video_seek", "video_seek needs qid and t_ms")
		return
	}
	c.videoCommand(videoCmd{mode: videoReplay, fromMS: *msg.TMS, qid: msg.QID, seek: true})
}

// videoCommand gives cmd to the client's pusher, starting one when there is none. A
// seek needs a replay in progress. An off command waits for the pusher to end, so a
// later command never runs beside it.
func (c *v2Client) videoCommand(cmd videoCmd) {
	c.videoMu.Lock()
	defer c.videoMu.Unlock()
	p := c.livePusher()
	if cmd.seek {
		if p == nil || p.getMode() != videoReplay {
			c.enqueueError("bad_video_seek", "video_seek needs a replay in progress")
			return
		}
		p.post(cmd)
		return
	}
	if p == nil {
		if cmd.mode != videoOff {
			p = newVideoPusher()
			c.vid = p
			go c.runVideo(p, cmd)
		}
		return
	}
	p.post(cmd)
	if cmd.mode == videoOff {
		select {
		case <-p.exited:
		case <-c.done:
		}
		c.vid = nil
	}
}

// livePusher returns the client's pusher, or nil when there is none or it has ended.
// videoMu must be held.
func (c *v2Client) livePusher() *videoPusher {
	if c.vid == nil {
		return nil
	}
	select {
	case <-c.vid.exited:
		c.vid = nil
		return nil
	default:
		return c.vid
	}
}

// videoStreaming reports whether the client has a pusher running, for the metadata.
func (c *v2Client) videoStreaming() bool {
	c.videoMu.Lock()
	defer c.videoMu.Unlock()
	return c.livePusher() != nil
}

// runVideo is the client's video pusher. It plays cmd's mode, then each command that
// changes it, until it is turned off or the client is gone.
func (c *v2Client) runVideo(p *videoPusher, cmd videoCmd) {
	defer close(p.exited)
	defer close(p.stop)
	go c.videoStatusLoop(p)
	for {
		var ok bool
		switch cmd.mode {
		case videoLive:
			cmd, ok = c.playLive(p)
		case videoReplay:
			cmd, ok = c.playReplay(p, cmd)
		default:
			c.endVideo(p)
			return
		}
		if !ok {
			return
		}
	}
}

// videoStatusLoop sends video_status once a second while video streams, until the
// pusher ends.
func (c *v2Client) videoStatusLoop(p *videoPusher) {
	tick := time.NewTicker(v2VideoStatusEvery)
	defer tick.Stop()
	for {
		select {
		case <-tick.C:
			p.mu.Lock()
			if p.mode == videoLive || p.mode == videoReplay {
				c.videoStatusLocked(p)
			}
			p.mu.Unlock()
		case <-p.stop:
			return
		case <-c.done:
			return
		}
	}
}

// beginVideo sets the pusher's mode and sends the status that announces it. The
// pieces queued for the old mode go first, so none of them follows the status. A
// non-nil qid is echoed in that status.
func (c *v2Client) beginVideo(p *videoPusher, mode string, qid *uint32) {
	c.dropVideo()
	p.mu.Lock()
	defer p.mu.Unlock()
	p.mode = mode
	p.gap = nil
	if qid != nil {
		p.qid, p.echo = *qid, true
	}
	c.videoStatusLocked(p)
}

// endVideo turns video off. The bulk queue is emptied first, so no piece follows the
// off status: the one piece already being written is the last.
func (c *v2Client) endVideo(p *videoPusher) {
	c.dropVideo()
	p.mu.Lock()
	defer p.mu.Unlock()
	p.mode = videoOff
	p.gap = nil
	c.videoStatusLocked(p)
}

// videoStatusLocked queues a video_status for the pusher's state. p.mu must be held.
func (c *v2Client) videoStatusLocked(p *videoPusher) {
	msg := v2VideoStatus{
		Type:  "video_status",
		Mode:  p.mode,
		TMS:   p.tMS,
		Clock: clockProvenanceName(c.hub.video.ClockProvenance()),
		Gap:   p.gap,
	}
	if p.echo {
		q := p.qid
		msg.QID = &q
		p.echo = false
	}
	c.enqueue(encodeV2(msg))
}

// sendGap sets the gap that the statuses report, and sends a status now. The gap
// stays in the statuses until video flows again (clearGap).
func (c *v2Client) sendGap(p *videoPusher, gap *v2VideoGap) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.gap = gap
	c.videoStatusLocked(p)
}

// clearGap ends a gap once video flows again, and says so in a plain status.
func (c *v2Client) clearGap(p *videoPusher) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.gap == nil {
		return
	}
	p.gap = nil
	c.videoStatusLocked(p)
}

// dropVideo empties the bulk queue, which only the pusher fills.
func (c *v2Client) dropVideo() {
	for {
		select {
		case <-c.bulk:
		default:
			return
		}
	}
}

// skipStalled is a live skip for a client that takes no piece for v2VideoStall. What
// is queued is stale, so it goes, and the skip is counted.
func (c *v2Client) skipStalled() {
	c.dropVideo()
	c.hub.videoSkips.Add(1)
}

// videoRead is one frame read from the store, or the error that ended the read.
type videoRead struct {
	frame VideoFrame
	seq   uint64
	err   error
}

// playLive sends the live video until a command arrives. It starts at the newest
// keyframe, after the config NALs, and then sends each frame after it in order. Until
// a keyframe it sends nothing. A client that the store has run past, or that takes no
// piece for v2VideoStall, skips to the newest keyframe, or waits for the next one if
// there is none newer than what it sent. It returns the command that ended it.
func (c *v2Client) playLive(p *videoPusher) (videoCmd, bool) {
	store := c.hub.video
	c.beginVideo(p, videoLive, nil)
	var (
		lastSent uint64 // the store sequence of the last frame queued
		next     uint64 // the store sequence to read next
		needKey  bool   // send nothing but a keyframe, and the config NALs before it
		restart  = true // start from the newest keyframe
	)
	for {
		if restart {
			restart = false
			kf, kfSeq, ok := store.LatestKeyframe()
			if !ok || kfSeq <= lastSent {
				next, needKey = store.NextSeq(), true
				continue
			}
			if age := int64(videoNowMS()) - int64(kf.TimestampMs); age > int64(videoLiveKeyframeFreshMs) {
				// Too old to start on. Say the camera has been silent since the
				// newest frame, and wait for the next keyframe.
				c.sendGap(p, &v2VideoGap{FromMS: store.NewestMs(), ToMS: 0, Reason: "stale"})
				next, needKey = store.NextSeq(), true
				continue
			}
			switch stop := c.startLive(p, kf, kfSeq); stop {
			case stopNone:
				lastSent, next, needKey = kfSeq, kfSeq+1, false
				c.clearGap(p)
			case stopStalled:
				c.skipStalled()
				restart = true
			default:
				return p.leave(stop)
			}
			continue
		}
		r, stop := c.readVideo(p, next)
		if stop != stopNone {
			return p.leave(stop)
		}
		if r.err != nil {
			return videoCmd{mode: videoOff}, true // the store is closed
		}
		if r.seq != next {
			// The store is past frames this client has not read: skip to the newest keyframe.
			c.hub.videoSkips.Add(1)
			restart = true
			continue
		}
		next = r.seq + 1
		if needKey {
			restart = r.frame.IsKeyframe()
			continue
		}
		switch stop := c.queueFrame(p, r.frame.TimestampMs, uint32(r.seq), r.frame.Flags, r.frame.Payload, v2VideoStall); stop {
		case stopNone:
			lastSent = r.seq
		case stopStalled:
			c.skipStalled()
			restart = true
		default:
			return p.leave(stop)
		}
	}
}

// startLive queues the config NALs the store has, then the keyframe kf at seq. That
// starts a stream the client can decode.
func (c *v2Client) startLive(p *videoPusher, kf VideoFrame, seq uint64) videoStop {
	sps, pps := c.hub.video.ConfigNALs()
	for _, nal := range [][]byte{sps, pps} {
		if len(nal) > 0 {
			if stop := c.queueFrame(p, kf.TimestampMs, 0, VideoFlagConfig, nal, v2VideoStall); stop != stopNone {
				return stop
			}
		}
	}
	return c.queueFrame(p, kf.TimestampMs, uint32(seq), kf.Flags, kf.Payload, v2VideoStall)
}

// readVideo waits for the store to hand over the frame at want, or a later one when
// the store has moved past it. It stops early for a command or the client's end. The
// read is done in its own goroutine; if the pusher stops first, the result is dropped,
// and the goroutine ends when the store next moves.
func (c *v2Client) readVideo(p *videoPusher, want uint64) (videoRead, videoStop) {
	ch := make(chan videoRead, 1)
	go func() {
		frame, seq, err := c.hub.video.ReadOrWait(want)
		ch <- videoRead{frame: frame, seq: seq, err: err}
	}()
	select {
	case r := <-ch:
		return r, stopNone
	case cmd := <-p.cmds:
		p.held = cmd
		return videoRead{}, stopCmd
	case <-c.done:
		return videoRead{}, stopGone
	}
}

// playReplay sends the stored video from the keyframe that covers the start time,
// paced to real time. A video_seek or another replay starts it again from its time.
// At the end of the stored video it says so, and sends nothing until a command. It
// returns the command that ended replay.
func (c *v2Client) playReplay(p *videoPusher, cmd videoCmd) (videoCmd, bool) {
	for {
		var qid *uint32
		if cmd.seek {
			q := cmd.qid
			qid = &q
		}
		c.beginVideo(p, videoReplay, qid)
		stop := c.replayFrom(p, cmd.fromMS)
		if stop == stopNone {
			p.mu.Lock()
			c.videoStatusLocked(p)
			p.mu.Unlock()
			stop = c.waitFor(p, nil)
		}
		next, ok := p.leave(stop)
		if !ok || next.mode != videoReplay {
			return next, ok
		}
		cmd = next
	}
}

// replayFrom sends the stored video from the keyframe that covers fromMS, paced to
// real time: each frame goes out when its time comes, up to v2VideoLead early. A
// recording hole is reported as a gap, and replay goes on from the keyframe after
// it; so does a segment that is archived. It returns stopNone at the end of the
// stored video.
func (c *v2Client) replayFrom(p *videoPusher, fromMS int64) videoStop {
	store := c.hub.video
	t := uint64(max(fromMS, 0))
	seq, _, _, ok := store.ReplayStart(t)
	if !ok {
		return stopNone
	}
	if from, to, next, hole := store.ReplayHole(t); hole {
		c.sendGap(p, &v2VideoGap{FromMS: from, ToMS: to, Reason: "none"})
		seq = next
	}

	var (
		base   uint64    // time of the first frame; pacing runs from there
		wall   time.Time // real time at base
		begun  bool
		prev   uint64 // time of the last picture queued in this run; 0 for none
		newRun = true // the next keyframe starts a run, so its config goes first
	)
	for {
		f, found, err := store.ReadSeq(seq)
		if errors.Is(err, errVideoArchived) {
			from, to, next, more := store.ArchivedSpan(seq)
			c.sendGap(p, &v2VideoGap{FromMS: from, ToMS: to, Reason: "archived"})
			if !more {
				return stopNone
			}
			seq, prev, newRun = next, 0, true
			continue
		}
		if err != nil || !found {
			return stopNone
		}
		if !begun {
			base, wall, begun = f.TimestampMs, time.Now(), true
		}
		if newRun && f.IsKeyframe() {
			sps, pps := store.ConfigBefore(seq)
			if stop := c.queueConfig(p, sps, pps, f.TimestampMs); stop != stopNone {
				return stop
			}
			newRun = false
		}
		if !f.IsConfig() && prev != 0 && f.TimestampMs > prev+videoGapMs {
			c.sendGap(p, &v2VideoGap{FromMS: prev, ToMS: f.TimestampMs, Reason: "none"})
		}
		due := wall.Add(time.Duration(int64(f.TimestampMs)-int64(base))*time.Millisecond - v2VideoLead)
		if stop := c.waitUntil(p, due); stop != stopNone {
			return stop
		}
		if stop := c.queueFrame(p, f.TimestampMs, uint32(seq), f.Flags|v2VideoReplay, f.Payload, 0); stop != stopNone {
			return stop
		}
		c.clearGap(p)
		if !f.IsConfig() {
			prev = f.TimestampMs
		}
		seq++
	}
}

// queueConfig queues the SPS and PPS that a decoder needs before a keyframe, stamped
// ts. A missing one is skipped.
func (c *v2Client) queueConfig(p *videoPusher, sps, pps []byte, ts uint64) videoStop {
	for _, nal := range [][]byte{sps, pps} {
		if len(nal) > 0 {
			if stop := c.queueFrame(p, ts, 0, VideoFlagConfig|v2VideoReplay, nal, 0); stop != stopNone {
				return stop
			}
		}
	}
	return stopNone
}

// queueFrame queues the pieces of one frame on the bulk queue, waiting for room. A
// positive stall is how long the wait for room may last before the send stops with
// stopStalled; zero waits as long as it takes. It stops early for a command or the
// client's end. A whole frame that is queued is counted and sets the last frame time.
func (c *v2Client) queueFrame(p *videoPusher, tms uint64, frame uint32, flags uint8, payload []byte, stall time.Duration) videoStop {
	for _, piece := range videoPieces(tms, frame, flags, payload) {
		if stop := c.queuePiece(p, piece, stall); stop != stopNone {
			return stop
		}
	}
	c.hub.videoFrames.Add(1)
	c.hub.videoBytes.Add(uint64(len(payload)))
	p.mu.Lock()
	p.tMS = tms
	p.mu.Unlock()
	return stopNone
}

// queuePiece queues one VideoChunk message, with the same stop rules as queueFrame.
func (c *v2Client) queuePiece(p *videoPusher, piece []byte, stall time.Duration) videoStop {
	m := outMsg{typ: wsBinary, data: piece}
	select {
	case c.bulk <- m:
		return stopNone
	default:
	}
	var stalled <-chan time.Time
	if stall > 0 {
		t := time.NewTimer(stall)
		defer t.Stop()
		stalled = t.C
	}
	select {
	case c.bulk <- m:
		return stopNone
	case cmd := <-p.cmds:
		p.held = cmd
		return stopCmd
	case <-c.done:
		return stopGone
	case <-stalled:
		return stopStalled
	}
}

// waitUntil waits until t, or until a command arrives or the client is gone.
func (c *v2Client) waitUntil(p *videoPusher, t time.Time) videoStop {
	d := time.Until(t)
	if d <= 0 {
		return stopNone
	}
	timer := time.NewTimer(d)
	defer timer.Stop()
	return c.waitFor(p, timer.C)
}

// waitFor waits until fire delivers, or until a command arrives or the client is
// gone. A nil fire waits for a command or the client alone.
func (c *v2Client) waitFor(p *videoPusher, fire <-chan time.Time) videoStop {
	select {
	case <-fire:
		return stopNone
	case cmd := <-p.cmds:
		p.held = cmd
		return stopCmd
	case <-c.done:
		return stopGone
	}
}
