package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"log"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

// scanBufferBytes is the read buffer for each chunk or segment file.
const scanBufferBytes = 1 << 20

// openChunkFile opens the files the scans and reads take. It is a variable so a
// test can make one of them fail.
var openChunkFile = os.Open

// scanRuns calls fn for every good record in the run folders under segmentsDir:
// runs in folder-name order, chunks in number order, records in order. The
// payload is only valid during the call.
//
// It reads manifests to find the chunks and never modifies anything. A server
// that is still writing its active chunk is fine: the chunk is read only up to
// the size it has when the scan opens it. A chunk whose file is missing, or that
// is in a state this version cannot read, is logged and skipped.
func scanRuns(segmentsDir string, fn func(run string, id MessageID, writeUnix uint32, payload []byte) error) error {
	names, err := listRunFolders(segmentsDir)
	if err != nil {
		return err
	}
	for _, name := range names {
		w := readRunManifest(segmentsDir, name)
		if w == nil {
			continue
		}
		for _, c := range w.chunks {
			if !c.readable() {
				log.Printf("[photon-server] backfill: skipping %s, which is %q, not a readable chunk", c.path, c.state)
				continue
			}
			if _, err := os.Stat(c.readPath()); err != nil {
				log.Printf("[photon-server] backfill: skipping %s: %v", c.readPath(), err)
				continue
			}
			run := name
			deliver := func(_ int64, id MessageID, writeUnix uint32, payload []byte) error {
				return fn(run, id, writeUnix, payload)
			}
			err := scanChunk(c, deliver)
			if errors.Is(err, os.ErrNotExist) {
				// The archiver may have compressed this chunk since the manifest was
				// read, and removed its .seg file. Read the manifest again for the new file.
				if fresh := chunkInManifest(segmentsDir, name, c.number); fresh != nil && fresh.readable() {
					err = scanChunk(fresh, deliver)
				}
			}
			if err != nil {
				return err
			}
		}
	}
	return nil
}

// scanChunk calls fn for the records of one chunk, read from its current file.
// An active chunk is read to the end of its file; any other up to its size in the
// manifest.
func scanChunk(c *chunk, fn func(off int64, id MessageID, writeUnix uint32, payload []byte) error) error {
	limit := int64(-1)
	if c.state != chunkStateActive {
		limit = c.bytes
	}
	_, err := scanRecordFile(c.readPath(), 0, limit, scanBufferBytes, fn)
	return err
}

// chunkInManifest returns the chunk numbered number in a run's manifest, or nil
// when the manifest cannot be read or has no such chunk.
func chunkInManifest(segmentsDir, run string, number int) *chunk {
	w := readRunManifest(segmentsDir, run)
	if w == nil {
		return nil
	}
	for _, c := range w.chunks {
		if c.number == number {
			return c
		}
	}
	return nil
}

// scanSegments calls fn for every good record in the old-format segment files of
// dir (%012d.log), in file order. A file ends at its first short or bad record,
// and the scan goes on to the next file. The payload is only valid during the
// call. It never modifies anything. It is used to migrate the old layout.
func scanSegments(dir string, fn func(id MessageID, writeUnix uint32, payload []byte) error) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	var nums []int
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if n, ok := legacySegmentNumber(e.Name()); ok {
			nums = append(nums, n)
		}
	}
	slices.Sort(nums)
	for _, n := range nums {
		if err := scanSegmentFile(filepath.Join(dir, segmentFileName(n)), fn); err != nil {
			return err
		}
	}
	return nil
}

// legacySegmentNumber parses an old-format segment file name, reporting false for
// any other name.
func legacySegmentNumber(name string) (int, bool) {
	var n int
	if _, err := fmt.Sscanf(name, "%012d.log", &n); err != nil || name != segmentFileName(n) {
		return 0, false
	}
	return n, true
}

// segmentFileName is the name of the old-format segment file numbered n.
func segmentFileName(n int) string {
	return fmt.Sprintf("%012d.log", n)
}

// scanSegmentFile calls fn for the good records of one segment file, and stops
// at the first short or bad one.
func scanSegmentFile(path string, fn func(id MessageID, writeUnix uint32, payload []byte) error) error {
	_, err := scanRecordFile(path, 0, -1, scanBufferBytes, func(_ int64, id MessageID, writeUnix uint32, payload []byte) error {
		return fn(id, writeUnix, payload)
	})
	return err
}

// scanRecordFile calls fn for the good records of one file, from offset start,
// up to limit bytes; a negative limit means the file's size when it is opened.
// It stops at the first short or bad record, and returns the offset where its
// good records end. fn gets the record's offset; the payload is only valid during
// the call. An error from fn stops the scan and is returned.
func scanRecordFile(path string, start, limit int64, bufBytes int, fn func(off int64, id MessageID, writeUnix uint32, payload []byte) error) (int64, error) {
	if strings.HasSuffix(path, chunkzExt) {
		return scanChunkzFile(path, start, limit, bufBytes, fn)
	}
	f, err := openChunkFile(path)
	if err != nil {
		return start, err
	}
	defer f.Close()
	if limit < 0 {
		info, err := f.Stat()
		if err != nil {
			return start, err
		}
		limit = info.Size()
	}
	if start >= limit {
		return start, nil
	}

	r := bufio.NewReaderSize(io.NewSectionReader(f, start, limit-start), bufBytes)
	return scanRecordStream(r, start, limit, fn)
}

// scanChunkzFile is scanRecordFile for a compressed chunk. It reads the original
// .seg bytes out of the .segz file, so the offsets are those of the .seg file.
func scanChunkzFile(path string, start, limit int64, bufBytes int, fn func(off int64, id MessageID, writeUnix uint32, payload []byte) error) (int64, error) {
	z, err := newChunkzReader(path, nil, nil)
	if err != nil {
		return start, err
	}
	defer z.Close()
	if limit < 0 {
		limit = z.size
	}
	if start >= limit {
		return start, nil
	}
	if _, err := io.CopyN(io.Discard, z, start); err != nil {
		return start, err
	}
	r := bufio.NewReaderSize(io.LimitReader(z, limit-start), bufBytes)
	return scanRecordStream(r, start, limit, fn)
}

// scanRecordStream is the loop of scanRecordFile. r holds the bytes from start up
// to limit.
func scanRecordStream(r *bufio.Reader, start, limit int64, fn func(off int64, id MessageID, writeUnix uint32, payload []byte) error) (int64, error) {
	off := start
	header := make([]byte, frameHeaderBytes)
	var payload []byte
	for limit-off >= frameHeaderBytes {
		if _, err := io.ReadFull(r, header); err != nil {
			return off, endOfFile(err)
		}
		n := int64(binary.BigEndian.Uint32(header[4:8]))
		if binary.BigEndian.Uint32(header[0:4]) != frameMagic || n > limit-off-frameHeaderBytes {
			return off, nil // bad magic, or a record that runs past the end
		}
		if int64(cap(payload)) < n {
			payload = make([]byte, n)
		}
		payload = payload[:n]
		if _, err := io.ReadFull(r, payload); err != nil {
			return off, endOfFile(err)
		}
		if crc32.ChecksumIEEE(payload) != binary.BigEndian.Uint32(header[20:24]) {
			return off, nil // bad checksum
		}
		id := MessageID(binary.BigEndian.Uint64(header[8:16]))
		if err := fn(off, id, binary.BigEndian.Uint32(header[16:20]), payload); err != nil {
			return off, err
		}
		off += frameHeaderBytes + n
	}
	return off, nil
}

// endOfFile turns a short read into the end of the file's good records; any
// other read error is returned.
func endOfFile(err error) error {
	if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
		return nil
	}
	return err
}
