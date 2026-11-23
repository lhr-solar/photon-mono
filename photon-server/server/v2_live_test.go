package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"net"
	"os"
	"testing"
	"time"
)

// newLiveHub returns a LAN hub whose live ring holds capacity frames.
func newLiveHub(t *testing.T, capacity int) *v2Hub {
	t.Helper()
	return newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), func() string { return "run-test" }, newLiveRing(capacity))
}

// liveConn dials h and completes hello. A catalog, if the hub has one, is still
// to be read.
func liveConn(t *testing.T, h *v2Hub) *wsConn {
	t.Helper()
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)
	return conn
}

// subscribeTo sends a subscribe message for ids. With no IDs it stops live data.
func subscribeTo(t *testing.T, conn *wsConn, ids ...uint32) {
	t.Helper()
	if ids == nil {
		ids = []uint32{}
	}
	sendV2(t, conn, map[string]any{"type": "subscribe", "messages": ids})
}

// readAny reads the next message, of either kind.
func readAny(t *testing.T, conn *wsConn) (wsMessageType, []byte) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	typ, data, err := conn.Read(ctx)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return typ, data
}

// readLive reads the next message, which must be a LiveFrames message.
func readLive(t *testing.T, conn *wsConn) (v2Header, []liveRecord) {
	t.Helper()
	typ, data := readAny(t, conn)
	if typ != wsBinary {
		t.Fatalf("got text %s, want a binary message", data)
	}
	hd, err := decodeV2Header(data)
	if err != nil {
		t.Fatal(err)
	}
	if hd.Type != v2MsgLiveFrames {
		t.Fatalf("message type %#x, want LiveFrames", hd.Type)
	}
	recs, err := decodeLiveFrames(data)
	if err != nil {
		t.Fatal(err)
	}
	return hd, recs
}

// collectLive reads LiveFrames messages until they hold n frames, and returns
// those frames. It fails if they hold more.
func collectLive(t *testing.T, conn *wsConn, n int) []liveRecord {
	t.Helper()
	var all []liveRecord
	for len(all) < n {
		_, recs := readLive(t, conn)
		all = append(all, recs...)
	}
	if len(all) != n {
		t.Fatalf("got %d frames, want %d", len(all), n)
	}
	return all
}

// recordIDs is the wire ID of each frame.
func recordIDs(recs []liveRecord) []uint32 {
	ids := make([]uint32, len(recs))
	for i, r := range recs {
		ids[i] = r.ID
	}
	return ids
}

// drainUntilSilent reads messages until none arrives for d, and returns how many
// came. It is the last read on conn: a timed-out read leaves the connection
// unreadable.
func drainUntilSilent(t *testing.T, conn *wsConn, d time.Duration) int {
	t.Helper()
	n := 0
	for {
		ctx, cancel := context.WithTimeout(context.Background(), d)
		typ, _, err := conn.Read(ctx)
		cancel()
		if err == nil {
			if typ != wsBinary {
				t.Fatal("got a text message while draining live data")
			}
			n++
			continue
		}
		// The read's deadline can pass before the context reports it, so either
		// error says whether this was the timeout.
		if !errors.Is(err, os.ErrDeadlineExceeded) && !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("connection failed: %v", err)
		}
		return n
	}
}

// waitUntil polls cond until it holds, and fails the test after five seconds.
func waitUntil(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(time.Millisecond)
	}
}

// pipeClient returns a client whose connection is a net.Pipe, and the test's end
// of it. A pipe holds nothing between its ends: a write waits until the other end
// reads. So the client's queues fill exactly when the test stops reading. Nothing
// runs yet; the caller starts what it needs (runLive starts all of it).
func pipeClient(t *testing.T) (*v2Client, *wsConn) {
	t.Helper()
	srv, cli := net.Pipe()
	c := newV2Client(newWSConn(srv, bufio.NewReader(srv), true))
	t.Cleanup(func() {
		close(c.done)
		_ = srv.Close()
		_ = cli.Close()
	})
	return c, newWSConn(cli, bufio.NewReader(cli), false)
}

// runLive starts the reader, writer and pusher of a pipe client, as serveWS does.
func runLive(c *v2Client, h *v2Hub) {
	go c.readLoop(context.Background(), time.Minute)
	go c.writeLoop()
	go c.pushLive(h)
}

// A client that subscribes joins from the oldest frame in the ring, so frames it
// was not yet subscribed for, but which are still there, are sent too.
func TestLiveJoinsFromOldestFrame(t *testing.T) {
	h := newLiveHub(t, 1<<14)
	h.ring.appendBuffer(liveBatch(nowMS(), []uint32{0x100, 0x101}))
	h.ring.appendBuffer(liveBatch(nowMS(), []uint32{0x102, 0x100}))

	conn := liveConn(t, h)
	subscribeTo(t, conn, 0x100, 0x102)
	got := recordIDs(collectLive(t, conn, 3))
	want := []uint32{0x100, 0x102, 0x100}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("IDs %x, want %x", got, want)
		}
	}
}

// Only the subscribed IDs are sent, standard and extended alike.
func TestLiveOnlySubscribedIDsArrive(t *testing.T) {
	h := newLiveHub(t, 1<<14)
	var ext uint32 = canpExtended | 0x18FEF100
	conn := liveConn(t, h)
	subscribeTo(t, conn, 0x100, ext)
	h.ring.appendBuffer(liveBatch(nowMS(), []uint32{0x100, 0x101, ext, 0x200, 0x100, ext | 1, 0x7FF}))

	got := recordIDs(collectLive(t, conn, 3))
	want := []uint32{0x100, ext, 0x100}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("IDs %x, want %x", got, want)
		}
	}
	expectNothingElse(t, conn)
}

// expectNothingElse fails if live data keeps arriving. It is the last read on conn.
func expectNothingElse(t *testing.T, conn *wsConn) {
	t.Helper()
	if n := drainUntilSilent(t, conn, 200*time.Millisecond); n != 0 {
		t.Fatalf("%d more live messages, want none", n)
	}
}

// Every binary message takes the next seq, and text messages do not use one.
func TestLiveSeqCountsBinaryMessages(t *testing.T) {
	h := newLiveHub(t, 1<<14)
	conn := liveConn(t, h)
	subscribeTo(t, conn, 0x100)
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 5})
	for range 3 {
		h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 2600)))
	}

	frames, pongs, next := 0, 0, uint32(0)
	for frames < 7800 || pongs == 0 {
		typ, data := readAny(t, conn)
		if typ == wsText {
			var p struct {
				Type string `json:"type"`
			}
			if err := json.Unmarshal(data, &p); err != nil || p.Type != "pong" {
				t.Fatalf("text message %s, want pong", data)
			}
			pongs++
			continue
		}
		hd, err := decodeV2Header(data)
		if err != nil {
			t.Fatal(err)
		}
		if hd.Seq != next {
			t.Fatalf("binary message seq %d, want %d", hd.Seq, next)
		}
		next++
		recs, err := decodeLiveFrames(data)
		if err != nil {
			t.Fatal(err)
		}
		frames += len(recs)
	}
	if next == 0 {
		t.Fatal("no live messages arrived")
	}
}

// With a catalog, each frame carries its message's bus index; an ID no DBC
// defines carries 0xFF.
func TestLiveBusIndexFromCatalog(t *testing.T) {
	cat := loadTestCatalog(t)
	h := newLiveHub(t, 1<<12)
	h.SetCatalog(cat)
	conn := liveConn(t, h)
	readMsg(t, conn) // the catalog, sent after hello_ack

	carCAN := -1
	for i, b := range cat.Buses {
		if b == "CarCAN" {
			carCAN = i
		}
	}
	if carCAN < 0 {
		t.Fatal("the spec has no CarCAN bus")
	}
	unknown := uint32(0x7FF)
	for cat.Lookup(unknown) != nil {
		unknown--
	}

	subscribeTo(t, conn, 81, unknown) // wire ID 81 is BPSCommand, on CarCAN
	h.ring.appendBuffer(liveBatch(nowMS(), []uint32{81, unknown}))
	recs := collectLive(t, conn, 2)
	if recs[0].Bus != uint8(carCAN) {
		t.Fatalf("ID 81 bus %d, want CarCAN (%d)", recs[0].Bus, carCAN)
	}
	if recs[1].Bus != v2BusUnknown {
		t.Fatalf("unknown ID bus %#x, want %#x", recs[1].Bus, v2BusUnknown)
	}
}

// A subscribe that is not a list of wire IDs gets bad_subscribe, and the
// connection stays open.
func TestBadSubscribeGetsError(t *testing.T) {
	h := newLiveHub(t, 1024)
	conn := liveConn(t, h)
	for _, body := range []string{
		`{"type":"subscribe","messages":"0x100"}`,
		`{"type":"subscribe","messages":[2048]}`, // 12 bits, no extended flag
	} {
		sendV2(t, conn, json.RawMessage(body))
		var e struct {
			Type string `json:"type"`
			Code string `json:"code"`
		}
		readInto(t, conn, &e)
		if e.Type != "error" || e.Code != "bad_subscribe" {
			t.Fatalf("reply to %s = %+v, want error bad_subscribe", body, e)
		}
	}
}

// The metadata counts the clients that have a subscription, the ring's size and
// its head.
func TestV2MetadataCountsLive(t *testing.T) {
	h := newLiveHub(t, 4096)
	conn := liveConn(t, h)
	if m := v2Metadata(h); m.LiveRingFrames != 4096 || m.LiveRingHead != 0 || m.LiveSubscribers != 0 || m.Clients != 1 {
		t.Fatalf("before subscribing: %+v", m)
	}
	subscribeTo(t, conn, 0x100)
	waitUntil(t, "the subscriber to be counted", func() bool { return v2Metadata(h).LiveSubscribers == 1 })
	h.ring.appendBuffer(liveBatch(nowMS(), []uint32{0x100, 0x101}))
	if m := v2Metadata(h); m.LiveRingHead != 2 {
		t.Fatalf("ring head %d after two frames, want 2", m.LiveRingHead)
	}
}

// A client whose queues are full while the ring overwrites its frames is sent a
// Gap, after the messages it already has, and then gets the frames from the
// newest onward.
func TestSlowClientGetsGapThenResumes(t *testing.T) {
	h := newLiveHub(t, 1024)
	c, peer := pipeClient(t)
	runLive(c, h)
	subscribeTo(t, peer, 0x100)

	// The writer holds one message and the live queue holds four, all waiting for
	// the test to read. Fill them.
	for i := 0; len(c.live) < cap(c.live); i++ {
		if i == 100 {
			t.Fatal("live queue never filled")
		}
		h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 10)))
		time.Sleep(time.Millisecond)
	}
	// The ring holds 1024 frames, so this overwrites what the client has not read.
	h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 5000)))

	var gap []byte
	for n := 0; gap == nil; n++ {
		if n == 8 {
			t.Fatal("no gap after 8 messages")
		}
		typ, data := readAny(t, peer)
		if typ != wsBinary {
			t.Fatalf("got text %s, want binary", data)
		}
		hd, err := decodeV2Header(data)
		if err != nil {
			t.Fatal(err)
		}
		if hd.Type == v2MsgGap {
			gap = data
		}
	}
	from, to, err := decodeGap(gap)
	if err != nil {
		t.Fatal(err)
	}
	if from == 0 || to < from {
		t.Fatalf("gap from %d to %d", from, to)
	}
	if n := h.gaps.Load(); n != 1 {
		t.Fatalf("gaps counter = %d, want 1", n)
	}

	h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 10)))
	for _, r := range collectLive(t, peer, 10) {
		if r.TNS < to {
			t.Fatalf("frame at %d after the gap, want at or after its end %d", r.TNS, to)
		}
	}
}

// Control messages go out before queued live data. Here the live queue is full
// and the writer is holding one message; a pong queued now must not wait behind
// the four that are waiting.
func TestControlOvertakesQueuedLive(t *testing.T) {
	h := newLiveHub(t, 1<<14)
	c, peer := pipeClient(t)
	runLive(c, h)
	subscribeTo(t, peer, 0x100)
	for range 5 {
		h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 1300)))
	}
	waitUntil(t, "the live queue to fill", func() bool { return len(c.live) == cap(c.live) })
	c.enqueue(encodeV2(v2Pong{Type: "pong", TMS: json.RawMessage("7")}))

	// At most the message already in the writer can precede the pong.
	before, frames := 0, 0
	for {
		typ, data := readAny(t, peer)
		if typ == wsText {
			if before > 1 {
				t.Fatalf("%d live messages before the pong, want at most 1", before)
			}
			var p struct {
				Type string `json:"type"`
			}
			if err := json.Unmarshal(data, &p); err != nil || p.Type != "pong" {
				t.Fatalf("text message %s, want pong", data)
			}
			break
		}
		recs, err := decodeLiveFrames(data)
		if err != nil {
			t.Fatal(err)
		}
		before++
		frames += len(recs)
	}
	for frames < 6500 {
		_, recs := readLive(t, peer)
		frames += len(recs)
	}
	if frames != 6500 {
		t.Fatalf("got %d frames, want 6500", frames)
	}
}

// The three queues go out in order: control, then live, then bulk, each in the
// order it was queued. The writer starts only after everything is queued, so
// the order is fixed.
func TestWriterSendsControlThenLiveThenBulk(t *testing.T) {
	c, peer := pipeClient(t)
	c.enqueue(encodeV2(v2Pong{Type: "pong", TMS: json.RawMessage("1")}))
	if !c.enqueueLive(outMsg{typ: wsBinary, data: encodeGap(1, 2)}) || !c.enqueueLive(outMsg{typ: wsBinary, data: encodeGap(3, 4)}) {
		t.Fatal("live queue refused a message")
	}
	c.bulk <- outMsg{typ: wsBinary, data: encodeGap(5, 6)}
	go c.writeLoop()

	typ, data := readAny(t, peer)
	if typ != wsText {
		t.Fatalf("first message is binary, want the control pong")
	}
	var p struct {
		Type string `json:"type"`
	}
	if err := json.Unmarshal(data, &p); err != nil || p.Type != "pong" {
		t.Fatalf("first message %s, want pong", data)
	}
	for i, want := range [][2]int64{{1, 2}, {3, 4}, {5, 6}} {
		typ, data := readAny(t, peer)
		if typ != wsBinary {
			t.Fatalf("message %d is text, want a gap", i)
		}
		hd, err := decodeV2Header(data)
		if err != nil {
			t.Fatal(err)
		}
		if hd.Seq != uint32(i) {
			t.Fatalf("message %d seq %d, want %d", i, hd.Seq, i)
		}
		from, to, err := decodeGap(data)
		if err != nil {
			t.Fatal(err)
		}
		if from != want[0] || to != want[1] {
			t.Fatalf("message %d = gap %d..%d, want %d..%d", i, from, to, want[0], want[1])
		}
	}
}

// An empty subscribe stops live data. Messages already queued still go out, but
// no more: at most the writer's message, the queue and one the pusher was
// holding.
func TestEmptySubscribeStopsLiveData(t *testing.T) {
	h := newLiveHub(t, 1<<16)
	c, peer := pipeClient(t)
	runLive(c, h)
	subscribeTo(t, peer, 0x100)
	// 30000 frames would be 24 messages with no stop.
	h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 30000)))
	waitUntil(t, "the live queue to fill", func() bool { return len(c.live) == cap(c.live) })

	subscribeTo(t, peer)
	got := drainUntilSilent(t, peer, 300*time.Millisecond)
	if got < v2LiveQueue+1 || got > v2LiveQueue+2 {
		t.Fatalf("%d live messages after the stop, want %d or %d", got, v2LiveQueue+1, v2LiveQueue+2)
	}
}
