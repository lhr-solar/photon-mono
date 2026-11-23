package main

import (
	"container/list"
	"sync"
)

const (
	// tileCacheMax caps the bytes of cached tiles. A user decision: 8 to 16 MB.
	tileCacheMax = 16 << 20
	// tileCacheWindowMS is how long a tile stays cached after it ends: the live
	// view window, about 10 minutes.
	tileCacheWindowMS = 10 * 60 * 1000
)

// tileKey names one tile of one run. The signal index means something only for
// the catalog with the given sha.
type tileKey struct {
	run    string
	sha    string
	signal uint32
	level  uint8
	index  int64
}

// tileEntry is one cached tile: its Tile messages, with seq 0. The messages are
// never sent as they are, so they do not change once cached.
type tileEntry struct {
	key  tileKey
	end  int64    // tile end, Unix ms
	msgs [][]byte // the Tile messages of the tile
	size int      // bytes in msgs
	elem *list.Element
}

// tileCache holds the sealed tiles of the live run, shared by every client. It
// holds the tiles of one run only: when the live run changes, it empties.
type tileCache struct {
	mu      sync.Mutex
	run     string // the run the entries belong to
	entries map[tileKey]*tileEntry
	order   *list.List // most recently used first; values are *tileEntry
	bytes   int
	hits    uint64
}

func newTileCache() *tileCache {
	return &tileCache{entries: make(map[tileKey]*tileEntry), order: list.New()}
}

// syncLocked empties the cache when run is not the run its entries belong to.
// c.mu must be held.
func (c *tileCache) syncLocked(run string) {
	if run == c.run {
		return
	}
	c.run = run
	c.entries = make(map[tileKey]*tileEntry)
	c.order.Init()
	c.bytes = 0
}

// get returns the cached messages for key in run, and whether there are any. The
// messages are shared, so a caller copies them before it changes them.
func (c *tileCache) get(run string, key tileKey) ([][]byte, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.syncLocked(run)
	e, ok := c.entries[key]
	if !ok {
		return nil, false
	}
	c.order.MoveToFront(e.elem)
	c.hits++
	return e.msgs, true
}

// put caches msgs for key in run, for a tile that ends at endMS. First it drops
// the tiles that ended more than tileCacheWindowMS before nowMS, then the least
// recently used until the new tile fits. A tile bigger than the whole cache is
// not kept.
func (c *tileCache) put(run string, key tileKey, endMS int64, msgs [][]byte, nowMS int64) {
	size := 0
	for _, m := range msgs {
		size += len(m)
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	c.syncLocked(run)
	cutoff := nowMS - tileCacheWindowMS
	for _, e := range c.entries {
		if e.end < cutoff {
			c.removeLocked(e)
		}
	}
	if old, ok := c.entries[key]; ok {
		c.removeLocked(old)
	}
	if size > tileCacheMax {
		return
	}
	for c.bytes+size > tileCacheMax {
		c.removeLocked(c.order.Back().Value.(*tileEntry))
	}
	e := &tileEntry{key: key, end: endMS, msgs: msgs, size: size}
	e.elem = c.order.PushFront(e)
	c.entries[key] = e
	c.bytes += size
}

// removeLocked drops e from the cache. c.mu must be held.
func (c *tileCache) removeLocked(e *tileEntry) {
	c.order.Remove(e.elem)
	delete(c.entries, e.key)
	c.bytes -= e.size
}

// stats is the bytes the cached tiles take, and how many requests were answered
// from the cache.
func (c *tileCache) stats() (bytes int64, hits uint64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return int64(c.bytes), c.hits
}
