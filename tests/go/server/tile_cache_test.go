package main

import "testing"

// The cache holds at most tileCacheMax bytes, and evicts the least recently used.
func TestTileCacheEvictsLeastRecentlyUsed(t *testing.T) {
	c := newTileCache()
	const now = int64(1_800_000_000_000)
	key := func(i int) tileKey { return tileKey{run: "r", sha: "s", signal: 1, index: int64(i)} }
	oneMiB := make([]byte, 1<<20)
	for i := range 20 {
		c.put("r", key(i), now, [][]byte{append([]byte(nil), oneMiB...)}, now)
	}
	if bytes, _ := c.stats(); bytes > tileCacheMax {
		t.Fatalf("cache holds %d bytes, over the %d cap", bytes, tileCacheMax)
	}
	if _, ok := c.get("r", key(0)); ok {
		t.Fatal("the oldest tile is still cached")
	}
	if _, ok := c.get("r", key(19)); !ok {
		t.Fatal("the newest tile was evicted")
	}
}

// A tile that ended more than the window ago leaves the cache on the next insert.
func TestTileCacheDropsTilesPastTheWindow(t *testing.T) {
	c := newTileCache()
	const now = int64(1_800_000_000_000)
	old := tileKey{run: "r", index: 1}
	fresh := tileKey{run: "r", index: 2}
	c.put("r", old, now-tileCacheWindowMS-1, [][]byte{{1, 2, 3}}, now)
	c.put("r", fresh, now, [][]byte{{4, 5}}, now)
	if _, ok := c.get("r", old); ok {
		t.Fatal("a tile past the window is still cached")
	}
	if _, ok := c.get("r", fresh); !ok {
		t.Fatal("a fresh tile is missing")
	}
	if bytes, hits := c.stats(); bytes != 2 || hits != 1 {
		t.Fatalf("bytes %d hits %d, want 2 and 1", bytes, hits)
	}
}

// The live run's tiles take at most 8 MiB, a half of the budget they had before the
// server had a memory limit.
func TestTileCacheBudgetIs8MiB(t *testing.T) {
	if tileCacheMax != 8<<20 {
		t.Fatalf("tileCacheMax = %d, want 8 MiB", tileCacheMax)
	}
}
