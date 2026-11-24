package main

import "testing"

// BenchmarkTilePieces builds a level 0 tile of ten full pieces and sends each one
// as the writer would, releasing it after the send. allocs/op divided by ten is
// the allocations per tile piece.
func BenchmarkTilePieces(b *testing.B) {
	const pieces = 10
	n := pieces * tilePieceSamples(0)
	job := tileJob{qid: 1, signal: 1, level: 0, index: 0}
	vals := make([]float64, 1)
	b.ReportAllocs()
	for b.Loop() {
		tb := newTileBuilder(job, true, func(piece *pieceBuf, last bool) bool {
			piece.release()
			return true
		})
		for i := range n {
			if err := tb.add(float64(i), vals); err != nil {
				b.Fatal(err)
			}
		}
		if err := tb.end(); err != nil {
			b.Fatal(err)
		}
	}
}
