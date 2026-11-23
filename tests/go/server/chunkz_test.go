package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math/rand"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// chunkzClip is the Brainerd recording in the old single-file layout.
const chunkzClip = "../../examples/brainerd-30s/2026-07-23-03-30s.log"

// v3Frame is one 29-byte CANP v3 frame, with every byte chosen by the test.
type v3Frame struct {
	id   uint32
	dlc  byte
	data [8]byte
	dt   [16]byte
}

// v3Batch builds one CANP v3 batch with the sequence number and time given.
func v3Batch(seq uint32, tsMS uint64, frames ...v3Frame) []byte {
	b := binary.BigEndian.AppendUint32(nil, canpMagic)
	b = binary.BigEndian.AppendUint16(b, canpVersion)
	b = binary.BigEndian.AppendUint16(b, uint16(len(frames)))
	b = binary.BigEndian.AppendUint32(b, seq)
	b = binary.BigEndian.AppendUint64(b, tsMS)
	for _, f := range frames {
		b = binary.BigEndian.AppendUint32(b, f.id)
		b = append(b, f.dlc)
		b = append(b, f.data[:]...)
		b = append(b, f.dt[:]...)
	}
	return b
}

// randomFrames returns n frames whose IDs come from ids and whose bytes are random.
func randomFrames(rng *rand.Rand, n int, ids ...uint32) []v3Frame {
	if len(ids) == 0 {
		ids = []uint32{0x100}
	}
	frames := make([]v3Frame, n)
	for i := range frames {
		f := &frames[i]
		f.id = ids[rng.Intn(len(ids))]
		f.dlc = 8
		binary.LittleEndian.PutUint64(f.data[:], rng.Uint64())
		binary.LittleEndian.PutUint64(f.dt[0:8], rng.Uint64())
		binary.LittleEndian.PutUint64(f.dt[8:16], rng.Uint64())
	}
	return frames
}

// batchRecords makes count records of batchesPer batches, each of frames frames,
// with write times and batch sequence numbers that run on from first.
func batchRecords(rng *rand.Rand, first MessageID, count, batchesPer, frames int, ids ...uint32) []segmentRecord {
	var recs []segmentRecord
	seq := uint32(first) * 7
	ts := uint64(1_700_000_000_000) + uint64(first)*1000
	for i := 0; i < count; i++ {
		var payload []byte
		for b := 0; b < batchesPer; b++ {
			payload = append(payload, v3Batch(seq, ts, randomFrames(rng, frames, ids...)...)...)
			seq++
			ts += 20
		}
		recs = append(recs, segmentRecord{
			id:        first + MessageID(i),
			writeUnix: uint32(1_700_000_000 + uint64(first) + uint64(i)),
			payload:   payload,
		})
	}
	return recs
}

// chunkFile returns the bytes of a .seg chunk holding recs, with the write times and
// crcs the records carry.
func chunkFile(recs ...segmentRecord) []byte {
	var file []byte
	for _, r := range recs {
		file = append(file, encodeFrameAt(r.id, r.writeUnix, r.payload)...)
	}
	return file
}

// withStoredCRC makes the record with the given header crc, which may be wrong.
func withStoredCRC(rec []byte, crc uint32) []byte {
	binary.BigEndian.PutUint32(rec[20:24], crc)
	return rec
}

// readAllChunkz returns the original bytes of a .segz file, read through openChunkz.
func readAllChunkz(t *testing.T, path string) []byte {
	t.Helper()
	r, err := openChunkz(path)
	if err != nil {
		t.Fatalf("openChunkz %s: %v", path, err)
	}
	defer r.Close()
	got, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("reading %s: %v", path, err)
	}
	return got
}

// fileLen is the size of a file.
func fileLen(t *testing.T, path string) int64 {
	t.Helper()
	st, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	return st.Size()
}

// chunkzCase is one chunk file for the round-trip test.
type chunkzCase struct {
	name string
	file []byte
}

// chunkzCases are the chunk files the round trip must give back exactly.
func chunkzCases(t *testing.T, rng *rand.Rand) []chunkzCase {
	t.Helper()
	var cases []chunkzCase
	add := func(name string, file []byte) { cases = append(cases, chunkzCase{name, file}) }

	add("whole batches", chunkFile(batchRecords(rng, 1, 40, 3, 20, 0x100, 0x101, 0x7FF)...))
	add("extended and can0-bit IDs", chunkFile(batchRecords(rng, 1, 20, 2, 10, 0x1FFFFFFF, 0x80000123, 0xFFFFFFFF)...))

	// dlc above 8 with 8 data bytes still stored: every one of the 29 bytes is kept.
	var odd []v3Frame
	for _, dlc := range []byte{0, 9, 15, 255} {
		f := randomFrames(rng, 1, 0x321)[0]
		f.dlc = dlc
		odd = append(odd, f)
	}
	add("dlc above 8", chunkFile(segmentRecord{id: 1, writeUnix: 1_700_000_000, payload: v3Batch(1, 5, odd...)}))

	// Payloads that are not whole batches are kept as opaque bytes.
	one := batchRecords(rng, 1, 1, 1, 5)[0].payload
	empty := []byte{}
	count0 := v3Batch(9, 9, randomFrames(rng, 2, 0x55)...)
	binary.BigEndian.PutUint16(count0[6:8], 0)
	random := make([]byte, 100)
	rng.Read(random)
	add("opaque payloads", chunkFile(
		segmentRecord{id: 1, writeUnix: 1_700_000_000, payload: append(append([]byte{}, one...), 1, 2, 3, 4, 5)},
		segmentRecord{id: 2, writeUnix: 1_700_000_001, payload: empty},
		segmentRecord{id: 3, writeUnix: 1_700_000_002, payload: count0},
		segmentRecord{id: 4, writeUnix: 1_700_000_003, payload: random},
		segmentRecord{id: 5, writeUnix: 1_700_000_004, payload: one},
	))

	// A stored crc that is not crc32 of the payload is kept.
	bad := encodeFrameAt(7, 1_700_000_007, one)
	add("wrong stored crc", append(withStoredCRC(bad, 0xDEADBEEF), chunkFile(segmentRecord{id: 8, writeUnix: 1_700_000_008, payload: one})...))

	// A record bigger than a block is a block of its own, batches or not.
	// Four batches of 40000 frames: more than 4 MiB in one record, and each batch count fits in 16 bits.
	big := batchRecords(rng, 100, 1, 4, 40_000, 0x200, 0x201)[0]
	noise := make([]byte, 5<<20)
	rng.Read(noise)
	add("records bigger than a block",
		chunkFile(segmentRecord{id: 99, writeUnix: 1_700_000_099, payload: one}, big,
			segmentRecord{id: 300, writeUnix: 1_700_000_300, payload: noise},
			segmentRecord{id: 301, writeUnix: 1_700_000_301, payload: one}))

	// Enough records for several blocks.
	many := batchRecords(rng, 1, 4500, 1, 100, 0x300, 0x301, 0x302, 0x303)
	file := chunkFile(many...)
	if len(file) < 3*chunkzBlockBytes {
		t.Fatalf("many-block case is %d bytes, want at least %d", len(file), 3*chunkzBlockBytes)
	}
	add("many blocks", file)

	add("empty chunk", nil)

	// Bytes after the last whole record: random junk, a torn record, and a bad
	// magic with a good record after it.
	junk := make([]byte, 13)
	rng.Read(junk)
	add("odd tail bytes", append(chunkFile(batchRecords(rng, 1, 3, 1, 4)...), junk...))
	torn := encodeFrameAt(50, 1_700_000_050, make([]byte, 300))[:300]
	add("torn record", append(chunkFile(batchRecords(rng, 1, 3, 1, 4)...), torn...))
	garbage := append([]byte("not a record at all, just bytes that go on for a while........"), 0)
	add("bad magic then a good record", append(append(chunkFile(batchRecords(rng, 1, 2, 1, 4)...), garbage...),
		chunkFile(segmentRecord{id: 60, writeUnix: 1_700_000_060, payload: one})...))
	return cases
}

// The chunk round trip is byte for byte: decompressing gives back the .seg file,
// the .segz file is checked, and no temporary file is left behind.
func TestChunkzRoundTripIsByteExact(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	for _, tc := range chunkzCases(t, rng) {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			src := filepath.Join(dir, chunkFileName(1))
			dst := filepath.Join(dir, chunkzFileName(1))
			if err := os.WriteFile(src, tc.file, 0o644); err != nil {
				t.Fatal(err)
			}

			info, err := compressChunk(src, dst)
			if err != nil {
				t.Fatalf("compress: %v", err)
			}
			if info.Size != int64(len(tc.file)) || info.ZBytes != fileLen(t, dst) {
				t.Fatalf("info %+v: want size %d and %d bytes on disk", info, len(tc.file), fileLen(t, dst))
			}
			if want := sha256.Sum256(tc.file); info.Sha256 != want {
				t.Fatalf("info sha256 %x, want %x", info.Sha256, want)
			}
			if got := readAllChunkz(t, dst); !bytes.Equal(got, tc.file) {
				t.Fatalf("decompressed %d bytes, want the %d original bytes (first difference at %d)",
					len(got), len(tc.file), firstDifference(got, tc.file))
			}
			if _, err := os.Stat(dst + ".tmp"); !errors.Is(err, fs.ErrNotExist) {
				t.Fatalf("temporary file left behind: %v", err)
			}

			// Scanning the .segz file finds the same records as scanning the .seg file.
			want := scanAllRecords(t, src, -1)
			got := scanAllRecords(t, dst, -1)
			if strings.Join(got, "\n") != strings.Join(want, "\n") {
				t.Fatalf("scan of the .segz file found %d records, the .seg file %d", len(got), len(want))
			}
		})
	}
}

// firstDifference is the first index where a and b differ, or the shorter length.
func firstDifference(a, b []byte) int {
	for i := range min(len(a), len(b)) {
		if a[i] != b[i] {
			return i
		}
	}
	return min(len(a), len(b))
}

// scanAllRecords lists the records a scan of path finds: offset, ID, write time and
// the sha256 of the payload, one string each.
func scanAllRecords(t *testing.T, path string, limit int64) []string {
	t.Helper()
	var out []string
	_, err := scanRecordFile(path, 0, limit, scanBufferBytes, func(off int64, id MessageID, writeUnix uint32, payload []byte) error {
		out = append(out, fmt.Sprintf("%d %d %d %x", off, id, writeUnix, sha256.Sum256(payload)))
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// The Brainerd recording, migrated to run folders and compressed, comes back byte
// for byte, and compresses at least 4x. Its speed is logged.
func TestChunkzBrainerdRecording(t *testing.T) {
	clip, err := os.ReadFile(chunkzClip)
	if err != nil {
		t.Fatalf("the Brainerd recording is needed for this test: %v", err)
	}
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(segDir, "000000000001.log"), clip, 0o644); err != nil {
		t.Fatal(err)
	}
	mig, err := migrateLegacySegments(segDir)
	if err != nil {
		t.Fatal(err)
	}

	var raw, zed int64
	var largest string
	names, err := listRunFolders(segDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range names {
		w := readRunManifest(segDir, name)
		if w == nil {
			t.Fatalf("run %s has no manifest", name)
		}
		for _, c := range w.chunks {
			dst := filepath.Join(segDir, name, chunkzFileName(c.number))
			orig, err := os.ReadFile(c.path)
			if err != nil {
				t.Fatal(err)
			}
			info, err := compressChunk(c.path, dst)
			if err != nil {
				t.Fatalf("compress %s: %v", c.path, err)
			}
			if got := readAllChunkz(t, dst); !bytes.Equal(got, orig) {
				t.Fatalf("%s does not decompress to the original bytes", dst)
			}
			raw += info.Size
			zed += info.ZBytes
			if info.Size > 0 && largest == "" {
				largest = c.path
			}
		}
	}
	ratio := float64(raw) / float64(zed)
	t.Logf("Brainerd recording: %d records in %d runs, %d bytes as .seg, %d bytes as .segz, ratio %.2fx (clip file %d bytes)",
		mig.Records, mig.Runs, raw, zed, ratio, len(clip))
	if ratio < 4 {
		t.Fatalf("compression ratio %.2fx, want at least 4x", ratio)
	}

	// Speed: the best of five compressions and decompressions of the same file. One
	// worker does the compressions, as the archiver's does.
	w, err := newChunkzWorker()
	if err != nil {
		t.Fatal(err)
	}
	defer w.close()
	var bestC, bestD time.Duration = time.Hour, time.Hour
	dst := filepath.Join(t.TempDir(), "speed.segz")
	for i := 0; i < 5; i++ {
		start := time.Now()
		if _, err := w.compress(context.Background(), largest, dst); err != nil {
			t.Fatal(err)
		}
		bestC = min(bestC, time.Since(start))
		start = time.Now()
		r, err := openChunkz(dst)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := io.Copy(io.Discard, r); err != nil {
			t.Fatal(err)
		}
		r.Close()
		bestD = min(bestD, time.Since(start))
	}
	mb := float64(len(clip)) / 1e6
	t.Logf("Brainerd speed, best of 5: compress (with the check) %.1f MB/s, decompress %.1f MB/s",
		mb/bestC.Seconds(), mb/bestD.Seconds())
}

// Compressing one full 32 MiB chunk: the heap while it runs is sampled and logged.
// The records are written straight to the file, so the test holds none of them.
// The archiver keeps one worker for its whole life, so the warm numbers are the
// ones that matter; the cold ones include the encoder's first allocation.
func TestChunkzCompressMemoryOn32MiBChunk(t *testing.T) {
	if testing.Short() {
		t.Skip("a 32 MiB chunk takes a few seconds")
	}
	rng := rand.New(rand.NewSource(2))
	dir := t.TempDir()
	src := filepath.Join(dir, chunkFileName(1))
	f, err := os.Create(src)
	if err != nil {
		t.Fatal(err)
	}
	var size int64
	var records int
	for id := MessageID(1); ; id++ {
		rec := batchRecords(rng, id, 1, 1, 49, 0x100, 0x101, 0x102, 0x103, 0x104, 0x105)[0]
		enc := encodeFrameAt(rec.id, rec.writeUnix, rec.payload)
		if size+int64(len(enc)) > segmentSize {
			break
		}
		if _, err := f.Write(enc); err != nil {
			t.Fatal(err)
		}
		size += int64(len(enc))
		records++
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}

	const mib = 1 << 20
	measure := func(label string, w *chunkzWorker) {
		t.Helper()
		dst := filepath.Join(dir, label+chunkzFileName(1))
		runtime.GC()
		var before runtime.MemStats
		runtime.ReadMemStats(&before)
		peakAlloc := before.HeapAlloc
		var peakLive uint64
		stop := make(chan struct{})
		sampled := make(chan struct{})
		go func() {
			defer close(sampled)
			var m runtime.MemStats
			for tick := 0; ; tick++ {
				runtime.ReadMemStats(&m)
				peakAlloc = max(peakAlloc, m.HeapAlloc)
				if tick%20 == 19 {
					// A collection now leaves only what is live: the heap after it is the live heap.
					runtime.GC()
					runtime.ReadMemStats(&m)
					peakLive = max(peakLive, m.HeapAlloc)
				}
				select {
				case <-stop:
					return
				case <-time.After(time.Millisecond):
				}
			}
		}()
		start := time.Now()
		info, err := w.compress(context.Background(), src, dst)
		elapsed := time.Since(start)
		close(stop)
		<-sampled
		if err != nil {
			t.Fatal(err)
		}
		runtime.GC()
		var after runtime.MemStats
		runtime.ReadMemStats(&after)

		t.Logf("%s: 32 MiB chunk, %d records, %d -> %d bytes (%.2fx) in %s (%.1f MB/s with the check)",
			label, records, info.Size, info.ZBytes, float64(info.Size)/float64(info.ZBytes),
			elapsed.Round(time.Millisecond), float64(info.Size)/1e6/elapsed.Seconds())
		t.Logf("%s: heap above the start: peak live (last GC) %.1f MiB, peak HeapAlloc %.1f MiB; total allocated %.1f MiB; retained after %.1f MiB",
			label, float64(int64(peakLive)-int64(before.HeapAlloc))/mib, float64(int64(peakAlloc)-int64(before.HeapAlloc))/mib,
			float64(after.TotalAlloc-before.TotalAlloc)/mib, float64(int64(after.HeapAlloc)-int64(before.HeapAlloc))/mib)
		// Only the warm figure is the limit: the cold one includes the encoder's tables.
		if got := int64(peakLive) - int64(before.HeapAlloc); label == "warm" && got > 64*mib {
			t.Errorf("warm: live heap grew %.1f MiB, want under 64 MiB", float64(got)/mib)
		}
	}

	cold, err := newChunkzWorker()
	if err != nil {
		t.Fatal(err)
	}
	defer cold.close()
	measure("cold", cold)

	warm, err := newChunkzWorker()
	if err != nil {
		t.Fatal(err)
	}
	defer warm.close()
	small := filepath.Join(dir, "small.seg")
	if err := os.WriteFile(small, chunkFile(batchRecords(rng, 1, 2, 1, 4)...), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := warm.compress(context.Background(), small, filepath.Join(dir, "small.segz")); err != nil {
		t.Fatal(err)
	}
	measure("warm", warm)
}

// A compression stopped by its context removes its temporary file and leaves the
// .seg file and any .segz file alone.
func TestChunkzStoppedCompressionLeavesNoTempFile(t *testing.T) {
	rng := rand.New(rand.NewSource(3))
	dir := t.TempDir()
	src := filepath.Join(dir, chunkFileName(1))
	dst := filepath.Join(dir, chunkzFileName(1))
	file := chunkFile(batchRecords(rng, 1, 50, 1, 10, 0x10)...)
	if err := os.WriteFile(src, file, 0o644); err != nil {
		t.Fatal(err)
	}
	w, err := newChunkzWorker()
	if err != nil {
		t.Fatal(err)
	}
	defer w.close()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := w.compress(ctx, src, dst); !errors.Is(err, context.Canceled) {
		t.Fatalf("compress with a stopped context: %v, want context.Canceled", err)
	}
	for _, p := range []string{dst, dst + ".tmp"} {
		if _, err := os.Stat(p); !errors.Is(err, fs.ErrNotExist) {
			t.Fatalf("%s after a stopped compression: %v", p, err)
		}
	}
	if got, _ := os.ReadFile(src); !bytes.Equal(got, file) {
		t.Fatal("the .seg file changed")
	}
}
