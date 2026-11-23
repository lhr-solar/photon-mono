package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"slices"
)

// scanBufferBytes is the read buffer for each segment file.
const scanBufferBytes = 1 << 20

// scanSegments calls fn for every good record in the segment files of dir, in
// ID order. A file ends at its first short or bad record, so the half-written
// tail of the active segment is left out, and the scan goes on to the next
// file. The payload is only valid during the call.
//
// It never modifies anything: the files are opened read-only, and each is read
// only up to the size it had when it was opened, so a server that is still
// writing the active segment is fine. Unlike OpenStreamStore, it does not
// index the records or truncate a tail.
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
		var n int
		if _, err := fmt.Sscanf(e.Name(), "%012d.log", &n); err == nil && e.Name() == segmentFileName(n) {
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

// segmentFileName is the name of the segment file that starts at id n.
func segmentFileName(n int) string {
	return fmt.Sprintf("%012d.log", n)
}

// scanSegmentFile calls fn for the good records of one segment file, and stops
// at the first short or bad one.
func scanSegmentFile(path string, fn func(id MessageID, writeUnix uint32, payload []byte) error) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return err
	}

	left := info.Size()
	r := bufio.NewReaderSize(io.LimitReader(f, left), scanBufferBytes)
	header := make([]byte, frameHeaderBytes)
	var payload []byte
	for left >= frameHeaderBytes {
		if _, err := io.ReadFull(r, header); err != nil {
			return endOfFile(err)
		}
		n := int64(binary.BigEndian.Uint32(header[4:8]))
		if binary.BigEndian.Uint32(header[0:4]) != frameMagic || n > left-frameHeaderBytes {
			return nil // bad magic, or a record that runs past the end of the file
		}
		if int64(cap(payload)) < n {
			payload = make([]byte, n)
		}
		payload = payload[:n]
		if _, err := io.ReadFull(r, payload); err != nil {
			return endOfFile(err)
		}
		if crc32.ChecksumIEEE(payload) != binary.BigEndian.Uint32(header[20:24]) {
			return nil // bad checksum
		}
		left -= frameHeaderBytes + n
		id := MessageID(binary.BigEndian.Uint64(header[8:16]))
		if err := fn(id, binary.BigEndian.Uint32(header[16:20]), payload); err != nil {
			return err
		}
	}
	return nil
}

// endOfFile turns a short read into the end of the file's good records; any
// other read error is returned.
func endOfFile(err error) error {
	if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
		return nil
	}
	return err
}
