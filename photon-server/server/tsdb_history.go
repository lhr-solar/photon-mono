package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"os/exec"
	"sync"
	"time"
)

// Saved runs live in the TSDB. History used to ask Influx, which is not the
// production store, so a paused log left the dashboard on "--" while video
// locate held the socket. photon-tsdb-export --serve keeps one store open
// and returns the same history chunks the UI already decodes.

type historyPoint struct {
	T uint64  `json:"t"`
	V float64 `json:"v"`
}

type historySeriesMeta struct {
	SignalID uint32        `json:"signal_id"`
	Level    int           `json:"level"`
	BucketMS uint32        `json:"bucket_ms"`
	Points   uint32        `json:"points"`
	TMin     uint64        `json:"t_min"`
	TMax     uint64        `json:"t_max"`
	Chunks   uint32        `json:"chunks"`
	Prior    *historyPoint `json:"prior,omitempty"`
	Next     *historyPoint `json:"next,omitempty"`
}

type historyHeader struct {
	OK        bool                `json:"ok"`
	Error     string              `json:"error"`
	Level     int                 `json:"level"`
	BucketMS  uint32              `json:"bucket_ms"`
	Truncated bool                `json:"truncated"`
	Chunks    int                 `json:"chunks"`
	Series    []historySeriesMeta `json:"series"`
}

type historyProc struct {
	dir    string
	mu     sync.Mutex
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stdout *bufio.Reader
	wait   chan struct{}
	err    error
}

var historyToolOverride string

var historyProcs = struct {
	sync.Mutex
	m map[string]*historyProc
}{m: map[string]*historyProc{}}

func closeHistoryProcs() {
	historyProcs.Lock()
	procs := historyProcs.m
	historyProcs.m = map[string]*historyProc{}
	historyProcs.Unlock()
	for _, p := range procs {
		p.close()
	}
}

func (p *historyProc) close() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.stopLocked()
}

func (p *historyProc) stopLocked() {
	if p.stdin != nil {
		_ = p.stdin.Close()
		p.stdin = nil
	}
	if p.cmd != nil && p.cmd.Process != nil {
		_ = p.cmd.Process.Kill()
	}
	if p.wait != nil {
		<-p.wait
		p.wait = nil
	}
	p.cmd = nil
	p.stdout = nil
}

func historyTool() string {
	if historyToolOverride != "" {
		return historyToolOverride
	}
	return resolveTsdbExportTool()
}

func (p *historyProc) ensure() error {
	if p.cmd != nil && p.stdin != nil && p.stdout != nil {
		return nil
	}
	bin := historyTool()
	if bin == "" {
		return fmt.Errorf("TSDB history tool is not installed")
	}
	cmd := exec.Command(bin, "--serve", p.dir)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	cmd.Stderr = io.Discard
	if err = cmd.Start(); err != nil {
		return err
	}
	wait := make(chan struct{})
	p.cmd = cmd
	p.stdin = stdin
	p.stdout = bufio.NewReaderSize(stdout, 64*1024)
	p.wait = wait
	go func() {
		p.err = cmd.Wait()
		close(wait)
	}()
	return nil
}

func (p *historyProc) query(ctx context.Context, queryID uint32, from, to uint64, max int, ids []uint32) (historyHeader, [][]byte, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	var zero historyHeader
	if err := p.ensure(); err != nil {
		return zero, nil, err
	}
	body, err := json.Marshal(map[string]any{
		"query_id": queryID, "from_ms": from, "to_ms": to, "max_points": max, "signal_ids": ids,
	})
	if err != nil {
		return zero, nil, err
	}
	if _, err = p.stdin.Write(append(body, '\n')); err != nil {
		p.stopLocked()
		return zero, nil, err
	}
	type result struct {
		h   historyHeader
		c   [][]byte
		err error
	}
	done := make(chan result, 1)
	go func() {
		var r result
		r.h, r.c, r.err = readHistoryReply(p.stdout)
		done <- r
	}()
	select {
	case <-ctx.Done():
		p.stopLocked()
		return zero, nil, ctx.Err()
	case r := <-done:
		if r.err != nil {
			p.stopLocked()
		}
		return r.h, r.c, r.err
	case <-time.After(20 * time.Second):
		p.stopLocked()
		return zero, nil, fmt.Errorf("TSDB history timed out")
	}
}

func readHistoryReply(r *bufio.Reader) (historyHeader, [][]byte, error) {
	var h historyHeader
	line, err := r.ReadBytes('\n')
	if err != nil {
		return h, nil, err
	}
	if err = json.Unmarshal(line, &h); err != nil {
		return h, nil, err
	}
	if !h.OK {
		if h.Error == "" {
			h.Error = "TSDB history failed"
		}
		return h, nil, fmt.Errorf("%s", h.Error)
	}
	if h.Chunks < 0 || h.Chunks > 4096 {
		return h, nil, fmt.Errorf("bad history chunk count")
	}
	chunks := make([][]byte, 0, h.Chunks)
	for i := 0; i < h.Chunks; i++ {
		var nbuf [4]byte
		if _, err = io.ReadFull(r, nbuf[:]); err != nil {
			return h, nil, err
		}
		n := binary.LittleEndian.Uint32(nbuf[:])
		if n > 8*1024*1024 {
			return h, nil, fmt.Errorf("history chunk too large")
		}
		buf := make([]byte, n)
		if _, err = io.ReadFull(r, buf); err != nil {
			return h, nil, err
		}
		chunks = append(chunks, buf)
	}
	return h, chunks, nil
}

func historyProcFor(dir string) (*historyProc, error) {
	historyProcs.Lock()
	defer historyProcs.Unlock()
	p := historyProcs.m[dir]
	if p == nil {
		p = &historyProc{dir: dir}
		historyProcs.m[dir] = p
	}
	return p, nil
}

func (c *browserClient) runTsdbHistory(ctx context.Context, q browserControl, generation uint64, sess tsdbSession, send func(browserOutput) bool, fail func(error)) {
	ids := q.SignalIDs
	if len(ids) == 0 {
		fail(fmt.Errorf("signal_ids required"))
		return
	}
	if len(ids) > 512 {
		fail(fmt.Errorf("too many signals"))
		return
	}
	proc, err := historyProcFor(sess.Dir)
	if err != nil {
		fail(err)
		return
	}
	header, chunks, err := coalesceHistory(ctx, sess.Dir, q, proc.query)
	if err != nil {
		log.Printf("tsdb history %s: %v", sess.ID, err)
		fail(err)
		return
	}
	ownPoints := pointsFor(header, q.SignalIDs)
	sendResult := func(completed bool) bool {
		series := seriesJSON(header, q.SignalIDs)
		text := map[string]any{
			"type": "history_result", "query_id": q.QueryID, "level": header.Level, "bucket_ms": header.BucketMS,
			"from_ms": q.From, "to_ms": q.To, "series": series, "truncated": header.Truncated,
		}
		return send(browserOutput{text: text, query: q.QueryID, generation: generation, completedQuery: completed})
	}
	if len(chunks) == 0 {
		sendResult(true)
		return
	}
	// An empty result drops the page's in-flight query before later chunks
	// are applied, which would throw away every other signal carried on this
	// reply. Send the result first only when this signal itself has samples.
	if ownPoints > 0 && !sendResult(false) {
		return
	}
	for i, chunk := range chunks {
		payload := chunk
		if ownPoints == 0 {
			payload = rewriteChunk(chunk, q.QueryID, false)
		}
		last := ownPoints > 0 && i == len(chunks)-1
		if !send(browserOutput{kind: 6, payload: payload, history: true, query: q.QueryID, generation: generation, completedQuery: last}) {
			return
		}
	}
	if ownPoints == 0 {
		sendResult(true)
	}
}

func pointsFor(h historyHeader, ids []uint32) uint32 {
	want := map[uint32]bool{}
	for _, id := range ids {
		want[id] = true
	}
	var n uint32
	for _, s := range h.Series {
		if want[s.SignalID] {
			n += s.Points
		}
	}
	return n
}

func seriesJSON(h historyHeader, ids []uint32) []map[string]any {
	want := map[uint32]bool{}
	for _, id := range ids {
		want[id] = true
	}
	series := make([]map[string]any, 0, len(ids))
	for _, s := range h.Series {
		if len(want) > 0 && !want[s.SignalID] {
			continue
		}
		row := map[string]any{
			"signal_id": s.SignalID, "level": s.Level, "bucket_ms": s.BucketMS,
			"points": s.Points, "t_min": s.TMin, "t_max": s.TMax, "chunks": s.Chunks,
		}
		if s.Prior != nil {
			row["prior"] = map[string]any{"t": s.Prior.T, "v": s.Prior.V}
		}
		if s.Next != nil {
			row["next"] = map[string]any{"t": s.Next.T, "v": s.Next.V}
		}
		series = append(series, row)
	}
	return series
}
