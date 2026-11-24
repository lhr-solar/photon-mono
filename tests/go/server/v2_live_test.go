package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"server/dbc"
)

// liveDBC lays out the messages the live and tile tests use. Each has the layout
// its tests need: plain integers, a scaled and a negative-factor signal, a float, a
// multiplexed message, a sample group, and an extended ID (the %d is its wire ID).
const liveDBC = `BO_ 256 Std100: 8 Node
 SG_ A : 0|8@1+ (1,0) [0|255] "" Node
 SG_ B : 8|8@1+ (1,0) [0|255] "" Node

BO_ 257 Std101: 8 Node
 SG_ C : 0|16@1+ (0.01,0) [0|655.35] "km/h" Node
 SG_ Neg : 16|8@1- (-0.5,10) [-10|10] "" Node

BO_ 291 Std123: 8 Node
 SG_ D : 0|8@1+ (1,0) [0|255] "" Node

BO_ 512 Flt200: 8 Node
 SG_ F : 0|32@1+ (1,0) [0|0] "" Node

SIG_VALTYPE_ 512 F : 1;

BO_ 768 Mux300: 8 Node
 SG_ Page M : 0|8@1+ (1,0) [0|255] "" Node
 SG_ P0 m0 : 8|8@1+ (1,0) [0|255] "" Node
 SG_ P1 m1 : 8|8@1+ (1,0) [0|255] "" Node

BO_ 1024 Samp: 8 Node
 SG_ Timestamp : 0|16@1+ (1,0) [0|0] "" Node
 SG_ Acc1 : 16|16@1+ (0.5,0) [0|0] "" Node
 SG_ Acc2 : 32|16@1+ (0.5,0) [0|0] "" Node

BO_ %d Ext: 8 Node
 SG_ G : 0|8@1+ (1,0) [0|255] "" Node
`

// loadDBC loads text as the one DBC file of a catalog, with the given sample rates.
func loadDBC(t *testing.T, text string, rates []dbc.SampleRate) *dbc.Catalog {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "Test.dbc"), []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	c, err := dbc.Load(dir, rates)
	if err != nil {
		t.Fatalf("load test DBC: %v", err)
	}
	return c
}

// liveCatalog is liveDBC as a catalog, with the Samp message sampled at 1 kHz.
func liveCatalog(t *testing.T) *dbc.Catalog {
	t.Helper()
	return loadDBC(t, fmt.Sprintf(liveDBC, canpExtended|0x18FEF100), []dbc.SampleRate{{Messages: "Samp", Hz: 1000}})
}

// signalAt is the catalog index and the signal of signal sig of message msg.
func signalAt(t *testing.T, cat *dbc.Catalog, msg, sig string) (int64, tileSignal) {
	t.Helper()
	for i, ts := range catalogSignals(cat) {
		if ts.msg.Name == msg && ts.sig.Name == sig {
			return int64(i), ts
		}
	}
	t.Fatalf("no signal %s.%s", msg, sig)
	return 0, tileSignal{}
}

// sigIndexer returns a function that gives the catalog index of a signal, by its
// message and signal names.
func sigIndexer(t *testing.T, cat *dbc.Catalog) func(msg, sig string) int64 {
	t.Helper()
	return func(msg, sig string) int64 {
		t.Helper()
		i, _ := signalAt(t, cat, msg, sig)
		return i
	}
}

// newLiveHub returns a LAN hub whose live ring holds capacity frames.
func newLiveHub(t *testing.T, capacity int) *v2Hub {
	t.Helper()
	return newV2Hub(&Config{Password: "secret"}, NewClientGate("secret"), func() string { return "run-test" }, newLiveRing(capacity), nil, nil)
}

// liveConn dials h and completes hello. When the hub has a catalog, the catalog
// that follows hello_ack is read too.
func liveConn(t *testing.T, h *v2Hub) *wsConn {
	t.Helper()
	conn := dialV2(t, serveHub(t, h, false), nil)
	sendV2(t, conn, hello(2, "secret"))
	var ack helloAck
	readInto(t, conn, &ack)
	if h.catalog != nil {
		readMsg(t, conn) // the catalog, sent after hello_ack
	}
	return conn
}

// subscribeTo sends a subscribe message for catalog signal indexes. With none it
// stops live data.
func subscribeTo(t *testing.T, conn *wsConn, indexes ...int64) {
	t.Helper()
	if indexes == nil {
		indexes = []int64{}
	}
	sendV2(t, conn, map[string]any{"type": "subscribe", "signals": indexes})
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

// readSamples reads the next message, which must be a LiveSamples message, and
// returns its records.
func readSamples(t *testing.T, conn *wsConn) []liveSample {
	t.Helper()
	typ, data := readAny(t, conn)
	if typ != wsBinary {
		t.Fatalf("got text %s, want a binary message", data)
	}
	hd, err := decodeV2Header(data)
	if err != nil {
		t.Fatal(err)
	}
	if hd.Type != v2MsgLiveSamples {
		t.Fatalf("message type %#x, want LiveSamples", hd.Type)
	}
	recs, err := decodeLiveSamples(data)
	if err != nil {
		t.Fatal(err)
	}
	return recs
}

// collectSamples reads LiveSamples messages until they hold n records, and returns
// those records. It fails if they hold more.
func collectSamples(t *testing.T, conn *wsConn, n int) []liveSample {
	t.Helper()
	var all []liveSample
	for len(all) < n {
		all = append(all, readSamples(t, conn)...)
	}
	if len(all) != n {
		t.Fatalf("got %d samples, want %d", len(all), n)
	}
	return all
}

// checkSample fails unless got is the sample at time tns of catalog signal sig,
// with value v.
func checkSample(t *testing.T, got liveSample, tns, sig int64, v float64) {
	t.Helper()
	if got.TNS != tns || int64(got.Signal) != sig || math.Abs(got.Value-v) > 1e-9 {
		t.Fatalf("sample %+v, want time %d, signal %d, value %v", got, tns, sig, v)
	}
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

// expectNothingElse fails if live data keeps arriving. It is the last read on conn.
func expectNothingElse(t *testing.T, conn *wsConn) {
	t.Helper()
	if n := drainUntilSilent(t, conn, 200*time.Millisecond); n != 0 {
		t.Fatalf("%d more live messages, want none", n)
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
	c.hub = h
	go c.readLoop(context.Background(), time.Minute)
	go c.writeLoop()
	go c.pushLive(h)
}

// A subscriber gets its signals decoded and scaled, at the frame's time, and the
// frames it did not subscribe to give nothing.
func TestLiveSamplesAreDecodedAndScaled(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<14)
	h.SetCatalog(cat)
	ts := nowMS()
	h.ring.appendBuffer(frameBatch(ts,
		testFrame{id: 0x100, data: []byte{5, 7}},             // A = 5, B = 7 (not subscribed)
		testFrame{id: 0x101, data: []byte{0xD2, 0x04, 0xFC}}, // C raw 1234, Neg raw -4
		testFrame{id: 0x123, data: []byte{9}},                // D (not subscribed)
	))
	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Std100", "A"), ix("Std101", "C"), ix("Std101", "Neg"))
	recs := collectSamples(t, conn, 3)

	at := int64(ts) * 1_000_000
	checkSample(t, recs[0], at, ix("Std100", "A"), 5)
	checkSample(t, recs[1], at, ix("Std101", "C"), 12.34) // 1234 * 0.01
	checkSample(t, recs[2], at, ix("Std101", "Neg"), 12)  // -4 * -0.5 + 10
	expectNothingElse(t, conn)
}

// A client that subscribes joins from the oldest frame in the ring, so frames it was
// not yet subscribed for, but which are still there, are sent too.
func TestLiveJoinsFromOldestFrame(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<14)
	h.SetCatalog(cat)
	h.ring.appendBuffer(frameBatch(nowMS(), testFrame{id: 0x100, data: []byte{1}}, testFrame{id: 0x101, data: []byte{0}}))
	h.ring.appendBuffer(frameBatch(nowMS(), testFrame{id: 0x100, data: []byte{2}}))

	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Std100", "A"))
	recs := collectSamples(t, conn, 2)
	if recs[0].Value != 1 || recs[1].Value != 2 {
		t.Fatalf("values %v, %v; want 1 then 2", recs[0].Value, recs[1].Value)
	}
}

// Only the subscribed signals arrive, standard and extended IDs alike.
func TestLiveOnlySubscribedSignalsArrive(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<14)
	h.SetCatalog(cat)
	ts := nowMS()
	h.ring.appendBuffer(frameBatch(ts,
		testFrame{id: 0x100, data: []byte{1, 2}},
		testFrame{id: 0x101, data: []byte{3, 0, 4}},
		testFrame{id: canpExtended | 0x18FEF100, data: []byte{6}},
		testFrame{id: 0x7FF, data: []byte{8}}, // no DBC defines it
		testFrame{id: 0x123, data: []byte{9}},
	))
	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Std100", "A"), ix("Ext", "G"))
	recs := collectSamples(t, conn, 2)
	at := int64(ts) * 1_000_000
	checkSample(t, recs[0], at, ix("Std100", "A"), 1)
	checkSample(t, recs[1], at, ix("Ext", "G"), 6)
	expectNothingElse(t, conn)
}

// Every binary message takes the next seq, and text messages do not use one.
func TestLiveSeqCountsBinaryMessages(t *testing.T) {
	cat := liveCatalog(t)
	h := newLiveHub(t, 1<<14)
	h.SetCatalog(cat)
	conn := liveConn(t, h)
	subscribeTo(t, conn, sigIndexer(t, cat)("Std100", "A"))
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 5})
	for range 3 {
		h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 2600)))
	}

	samples, pongs, next := 0, 0, uint32(0)
	for samples < 7800 || pongs == 0 {
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
		recs, err := decodeLiveSamples(data)
		if err != nil {
			t.Fatal(err)
		}
		samples += len(recs)
	}
	if next == 0 {
		t.Fatal("no live messages arrived")
	}
}

// A can0 frame carries bit 12 in the ring. A DBC that defines its message without
// the bit still decodes the frame, and the subscription reads the frame.
func TestLiveCan0FrameDecodesWithoutTheBit(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<12)
	h.SetCatalog(cat)
	ts := nowMS()
	h.ring.appendBuffer(frameBatch(ts, testFrame{id: 0x1123, data: []byte{9}}))

	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Std123", "D"))
	recs := collectSamples(t, conn, 1)
	checkSample(t, recs[0], int64(ts)*1_000_000, ix("Std123", "D"), 9)
}

// A multiplexed message gives the signals on the page its frame carried, and the
// selector itself on every frame.
func TestLiveMuxedMessageGivesItsPage(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<12)
	h.SetCatalog(cat)
	ts := nowMS()
	h.ring.appendBuffer(frameBatch(ts,
		testFrame{id: 0x300, data: []byte{0, 11}}, // page 0: P0 = 11
		testFrame{id: 0x300, data: []byte{1, 22}}, // page 1: P1 = 22
	))

	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Mux300", "Page"), ix("Mux300", "P0"), ix("Mux300", "P1"))
	recs := collectSamples(t, conn, 4)
	at := int64(ts) * 1_000_000
	checkSample(t, recs[0], at, ix("Mux300", "Page"), 0)
	checkSample(t, recs[1], at, ix("Mux300", "P0"), 11)
	checkSample(t, recs[2], at, ix("Mux300", "Page"), 1)
	checkSample(t, recs[3], at, ix("Mux300", "P1"), 22)
}

// Signals in a sample group are taken before the frame: with a 1 kHz rate, the
// first of two samples is 1 ms before it. A signal that is not a sample is at the
// frame's time.
func TestLiveSampleGroupTimes(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<12)
	h.SetCatalog(cat)
	ts := nowMS()
	h.ring.appendBuffer(frameBatch(ts, testFrame{id: 0x400, data: []byte{7, 0, 10, 0, 20, 0}}))

	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Samp", "Timestamp"), ix("Samp", "Acc1"), ix("Samp", "Acc2"))
	recs := collectSamples(t, conn, 3)
	at := int64(ts) * 1_000_000
	checkSample(t, recs[0], at, ix("Samp", "Timestamp"), 7)
	checkSample(t, recs[1], at-1_000_000, ix("Samp", "Acc1"), 5) // 10 * 0.5
	checkSample(t, recs[2], at, ix("Samp", "Acc2"), 10)          // 20 * 0.5
}

// A NaN or infinite float gives no sample; a finite one does.
func TestLiveNonFiniteFloatsAreSkipped(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<12)
	h.SetCatalog(cat)
	ts := nowMS()
	f32le := func(f float32) []byte { return binary.LittleEndian.AppendUint32(nil, math.Float32bits(f)) }
	h.ring.appendBuffer(frameBatch(ts,
		testFrame{id: 0x200, data: f32le(float32(math.NaN()))},
		testFrame{id: 0x200, data: f32le(float32(math.Inf(1)))},
		testFrame{id: 0x200, data: f32le(1.5)},
	))

	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Flt200", "F"))
	recs := collectSamples(t, conn, 1)
	checkSample(t, recs[0], int64(ts)*1_000_000, ix("Flt200", "F"), 1.5)
	expectNothingElse(t, conn)
}

// A subscribe that is not a list of catalog indexes, or names one that is not
// there, gets bad_subscribe, and so does any subscribe while there is no catalog.
// The connection stays open.
func TestBadSubscribeGetsError(t *testing.T) {
	h := newLiveHub(t, 1024)
	h.SetCatalog(liveCatalog(t))
	conn := liveConn(t, h)
	for _, body := range []string{
		`{"type":"subscribe","signals":"0"}`,
		`{"type":"subscribe","signals":[-1]}`,
		`{"type":"subscribe","signals":[1.5]}`,
		`{"type":"subscribe","signals":[1000000]}`,
		`{"type":"subscribe","signals":null}`,
		`{"type":"subscribe"}`,
	} {
		expectBadSubscribe(t, conn, body)
	}

	noCatalog := liveConn(t, newLiveHub(t, 1024))
	expectBadSubscribe(t, noCatalog, `{"type":"subscribe","signals":[]}`)
}

// expectBadSubscribe sends body and fails unless the reply is bad_subscribe.
func expectBadSubscribe(t *testing.T, conn *wsConn, body string) {
	t.Helper()
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

// A new catalog clears every subscription: no samples come until the client
// subscribes again. The new subscription starts from the oldest frame in the ring.
func TestCatalogChangeClearsSubscription(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<12)
	h.SetCatalog(cat)
	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Std100", "A"))

	h.ring.appendBuffer(frameBatch(nowMS(), testFrame{id: 0x100, data: []byte{1}}))
	collectSamples(t, conn, 1)

	alt := *cat
	alt.SHA = "alt-sha-test"
	h.SetCatalog(&alt)
	readMsg(t, conn) // the new catalog
	h.ring.appendBuffer(frameBatch(nowMS(), testFrame{id: 0x100, data: []byte{2}}))

	// A ping and its pong are the only messages: no samples while nothing is
	// subscribed. The pong comes first because control messages go out first.
	sendV2(t, conn, map[string]any{"type": "ping", "t_ms": 7})
	for {
		typ, data := readAny(t, conn)
		if typ == wsBinary {
			t.Fatal("live samples arrived after the catalog changed, before a new subscribe")
		}
		var p struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(data, &p); err != nil {
			t.Fatal(err)
		}
		if p.Type == "pong" {
			break
		}
	}

	subscribeTo(t, conn, ix("Std100", "A"))
	recs := collectSamples(t, conn, 2) // frames 1 and 2 are still in the ring
	if recs[0].Value != 1 || recs[1].Value != 2 {
		t.Fatalf("values %v, %v after resubscribing; want 1 then 2", recs[0].Value, recs[1].Value)
	}
}

// An empty subscribe stops live data. Messages already queued still go out, but
// no more: at most the writer's message, the queue and one the pusher was holding.
func TestEmptySubscribeStopsLiveData(t *testing.T) {
	cat := liveCatalog(t)
	h := newLiveHub(t, 1<<16)
	h.SetCatalog(cat)
	c, peer := pipeClient(t)
	runLive(c, h)
	subscribeTo(t, peer, sigIndexer(t, cat)("Std100", "A"))
	// 30000 frames would be 24 messages with no stop.
	h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 30000)))
	waitUntil(t, "the live queue to fill", func() bool { return len(c.live) == cap(c.live) })

	subscribeTo(t, peer)
	got := drainUntilSilent(t, peer, 300*time.Millisecond)
	if got < v2LiveQueue+1 || got > v2LiveQueue+2 {
		t.Fatalf("%d live messages after the stop, want %d or %d", got, v2LiveQueue+1, v2LiveQueue+2)
	}
}

// A subscriber to a rare signal still gets it promptly when it sits behind more
// than liveScanMax other frames and nothing is appended after it.
func TestLiveRareIDBehindScanCap(t *testing.T) {
	cat := liveCatalog(t)
	ix := sigIndexer(t, cat)
	h := newLiveHub(t, 1<<15)
	h.SetCatalog(cat)
	for i := 0; i < 20000/64; i++ {
		h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 64)))
	}
	ts := nowMS()
	h.ring.appendBuffer(frameBatch(ts, testFrame{id: 0x123, data: []byte{42}}))

	conn := liveConn(t, h)
	subscribeTo(t, conn, ix("Std123", "D"))
	recs := collectSamples(t, conn, 1)
	checkSample(t, recs[0], int64(ts)*1_000_000, ix("Std123", "D"), 42)
}

// The metadata counts the clients that have a subscription, the ring's size and
// its head, and the samples sent.
func TestV2MetadataCountsLive(t *testing.T) {
	cat := liveCatalog(t)
	h := newLiveHub(t, 4096)
	h.SetCatalog(cat)
	conn := liveConn(t, h)
	if m := v2Metadata(h); m.LiveRingFrames != 4096 || m.LiveRingHead != 0 || m.LiveSubscribers != 0 || m.Clients != 1 || m.LiveSamplesSent != 0 {
		t.Fatalf("before subscribing: %+v", m)
	}
	subscribeTo(t, conn, sigIndexer(t, cat)("Std100", "A"))
	waitUntil(t, "the subscriber to be counted", func() bool { return v2Metadata(h).LiveSubscribers == 1 })
	h.ring.appendBuffer(frameBatch(nowMS(), testFrame{id: 0x100, data: []byte{1}}, testFrame{id: 0x100, data: []byte{2}}))
	if m := v2Metadata(h); m.LiveRingHead != 2 {
		t.Fatalf("ring head %d after two frames, want 2", m.LiveRingHead)
	}
	collectSamples(t, conn, 2)
	waitUntil(t, "the samples to be counted", func() bool { return v2Metadata(h).LiveSamplesSent == 2 })
}

// A client whose queues are full while the ring overwrites its frames is sent a
// Gap, after the messages it already has, and then gets the frames from the newest
// onward.
func TestSlowClientGetsGapThenResumes(t *testing.T) {
	cat := liveCatalog(t)
	h := newLiveHub(t, 1024)
	h.SetCatalog(cat)
	c, peer := pipeClient(t)
	runLive(c, h)
	subscribeTo(t, peer, sigIndexer(t, cat)("Std100", "A"))

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
	for _, r := range collectSamples(t, peer, 10) {
		if r.TNS < to {
			t.Fatalf("sample at %d after the gap, want at or after its end %d", r.TNS, to)
		}
	}
}

// Control messages go out before queued live data. Here the live queue is full
// and the writer is holding one message; a pong queued now must not wait behind
// the four that are waiting.
func TestControlOvertakesQueuedLive(t *testing.T) {
	cat := liveCatalog(t)
	h := newLiveHub(t, 1<<14)
	h.SetCatalog(cat)
	c, peer := pipeClient(t)
	runLive(c, h)
	subscribeTo(t, peer, sigIndexer(t, cat)("Std100", "A"))
	for range 5 {
		h.ring.appendBuffer(liveBatch(nowMS(), repeatID(0x100, 1300)))
	}
	waitUntil(t, "the live queue to fill", func() bool { return len(c.live) == cap(c.live) })
	c.enqueue(encodeV2(v2Pong{Type: "pong", TMS: json.RawMessage("7")}))

	// At most the message already in the writer can precede the pong.
	before, samples := 0, 0
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
		recs, err := decodeLiveSamples(data)
		if err != nil {
			t.Fatal(err)
		}
		before++
		samples += len(recs)
	}
	for samples < 6500 {
		samples += len(readSamples(t, peer))
	}
	if samples != 6500 {
		t.Fatalf("got %d samples, want 6500", samples)
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

