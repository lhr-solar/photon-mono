package main

import (
	"context"
	"encoding/binary"
	"net/http"
	"testing"
	"time"
)

// tilePiecesOut is how many tile piece buffers are taken and not yet given back.
func tilePiecesOut() int64 {
	return tilePieces.gets.Load() - tilePieces.puts.Load()
}

// waitTilePiecesBack waits until no more tile piece buffers are out than before.
// Other tests' writers may give buffers back meanwhile, which only helps.
func waitTilePiecesBack(t *testing.T, out0 int64) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for tilePiecesOut() > out0 {
		if time.Now().After(deadline) {
			t.Fatalf("%d tile piece buffers are not given back (%d before the test)", tilePiecesOut(), out0)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// A tile streamed in three pieces gives each piece back once it is written.
func TestTilePiecesReturnToThePoolAfterWriting(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	n := 2*tilePieceSamples(0) + 100
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(n, from) }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	out0, gets0 := tilePiecesOut(), tilePieces.gets.Load()
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	if pieces := readTile(t, conn, 1); len(pieces) != 3 {
		t.Fatalf("%d pieces, want 3", len(pieces))
	}
	if taken := tilePieces.gets.Load() - gets0; taken < 3 {
		t.Fatalf("%d piece buffers taken for a three piece tile", taken)
	}
	waitTilePiecesBack(t, out0)
}

// A client that goes away with pieces still to send does not keep their buffers.
func TestTilePiecesReturnWhenTheClientGoesAway(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	n := 6 * tilePieceSamples(0)
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(n, from) }}
	conn := tileConn(t, newTileHub(t, f, fixedLive(tileTestRun)))

	out0 := tilePiecesOut()
	requestTile(t, conn, 1, "run-old", signal, 0, index)
	_ = conn.CloseNow()
	waitTilePiecesBack(t, out0)
}

// The messages still queued when a writer stops give their buffers back, and the
// queue is left empty.
func TestQueuedTilePiecesReturnWhenTheWriterStops(t *testing.T) {
	c := newV2Client(nil)
	out0 := tilePiecesOut()
	for range 3 {
		pb := tilePieces.get()
		pb.b = append(pb.b, make([]byte, 16)...)
		c.live <- outMsg{typ: wsBinary, data: pb.b, buf: pb}
	}
	pb := tilePieces.get()
	c.bulk <- outMsg{typ: wsBinary, data: pb.b, buf: pb}
	c.releaseQueued()
	if n := len(c.live) + len(c.bulk); n != 0 {
		t.Fatalf("%d messages left queued", n)
	}
	if got := tilePiecesOut(); got != out0 {
		t.Fatalf("%d buffers out after the writer stopped, want %d", got, out0)
	}
}

// A tile served from the cache is copied into pool buffers, and the cached copy is
// left as it was: its qid is still the one of the request that filled the cache.
func TestCachedTileIsUnaffectedByPooling(t *testing.T) {
	signal, _ := plainSignal(t)
	index := pastIndex(0)
	from, _ := tileBounds(0, index)
	n := tilePieceSamples(0) + 10
	f := &fakeQueries{reply: func(context.Context, fakeQuery) (int, string) { return http.StatusOK, rawRows(n, from) }}
	h := newTileHub(t, f, fixedLive(tileTestRun))
	conn := tileConn(t, h)

	requestTile(t, conn, 1, tileTestRun, signal, 0, index)
	first := readTile(t, conn, 1)
	out0 := tilePiecesOut()
	requestTile(t, conn, 2, tileTestRun, signal, 0, index)
	second := readTile(t, conn, 2)
	if len(second) != len(first) {
		t.Fatalf("cached tile has %d pieces, want %d", len(second), len(first))
	}
	waitTilePiecesBack(t, out0)

	key := tileKey{run: tileTestRun, sha: h.catalogSHA, signal: uint32(signal), level: 0, index: index}
	msgs, ok := h.tiles.get(tileTestRun, key)
	if !ok || len(msgs) != len(first) {
		t.Fatalf("cache holds %d pieces (found %v), want %d", len(msgs), ok, len(first))
	}
	for i, m := range msgs {
		if qid := binary.LittleEndian.Uint32(m[v2HeaderSize:]); qid != 1 {
			t.Fatalf("cached piece %d has qid %d, want 1", i, qid)
		}
	}
	if len(f.queries()) != 1 {
		t.Fatalf("%d queries, want 1", len(f.queries()))
	}
}

// A piece buffer comes back empty, and the pool counts what it hands out and takes
// back.
func TestPiecePoolBuffersComeBackEmpty(t *testing.T) {
	p := newPiecePool(64)
	gets0, puts0 := p.gets.Load(), p.puts.Load()
	pb := p.get()
	if len(pb.b) != 0 || cap(pb.b) < 64 {
		t.Fatalf("new buffer len %d cap %d, want empty with room for 64", len(pb.b), cap(pb.b))
	}
	pb.b = append(pb.b, 1, 2, 3)
	pb.release()
	again := p.get()
	if len(again.b) != 0 {
		t.Fatalf("buffer from the pool has %d bytes, want none", len(again.b))
	}
	again.release()
	if g, q := p.gets.Load()-gets0, p.puts.Load()-puts0; g != 2 || q != 2 {
		t.Fatalf("gets %d and puts %d, want 2 and 2", g, q)
	}
}
