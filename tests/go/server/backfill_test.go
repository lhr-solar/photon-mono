package main

import (
	"bytes"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

// backfillDirs makes a data directory with its segments folder, as the server does.
func backfillDirs(t *testing.T) (dataDir, segDir string) {
	t.Helper()
	dataDir = t.TempDir()
	segDir = filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	return dataDir, segDir
}

// storeBuffers appends payloads to the store in dataDir the way the server does,
// then closes it. The IDs go on from whatever the store already holds.
func storeBuffers(t *testing.T, dataDir string, payloads ...[]byte) {
	t.Helper()
	store, err := OpenStreamStore(dataDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range payloads {
		if _, err := store.Append(p); err != nil {
			t.Fatal(err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
}

// logSession records a run in sessions.log.
func logSession(t *testing.T, segDir, sid string, firstID MessageID) {
	t.Helper()
	if err := appendSessionStart(segDir, sid, firstID); err != nil {
		t.Fatal(err)
	}
}

// frameSessions returns the session tag of each frame row in lines, in order.
func frameSessions(lines []string) []string {
	var tags []string
	for _, row := range rowsWithPrefix(lines, "frame,") {
		tag, _, _ := strings.Cut(strings.TrimPrefix(row, "frame,session="), ",")
		tags = append(tags, tag)
	}
	return tags
}

// writeTwoRuns lays down two recorded runs: S1 with records 1 and 2, and S2 with
// record 3. Everything was written in 2024.
func writeTwoRuns(t *testing.T, segDir string) {
	t.Helper()
	w1 := uint32(time.Date(2024, 6, 1, 12, 0, 0, 0, time.UTC).Unix())
	w2 := w1 + 3600
	writeSegment(t, segDir, 1,
		segmentRecord{id: 1, writeUnix: w1, payload: frameBatch(uint64(w1)*1000, testFrame{id: 100, data: plainData})},
		segmentRecord{id: 2, writeUnix: w1, payload: frameBatch(uint64(w1)*1000+1000, testFrame{id: 100, data: plainData})})
	writeSegment(t, segDir, 3,
		segmentRecord{id: 3, writeUnix: w2, payload: frameBatch(uint64(w2)*1000, testFrame{id: 101, data: muxData(1)})})
	logSession(t, segDir, "S1", 1)
	logSession(t, segDir, "S2", 3)
}

// noEnvironment is an environment with nothing set.
func noEnvironment(string) string { return "" }

// The rows a backfill writes for a recording are the rows the live server wrote
// for the same frames: frame and signal_def rows exactly, and the final session
// row. Session rows may repeat, so only the last one is compared.
func TestBackfillWritesSameRowsAsLive(t *testing.T) {
	dir := writeFrameTestDBC(t)
	bufs := [][]byte{
		// Two frames of one message in the same millisecond, and a muxed message.
		frameBatch(testTSMS, testFrame{id: 100, data: plainData}, testFrame{id: 100, data: plainData},
			testFrame{id: 101, data: muxData(1)}, testFrame{id: 101, data: muxData(2)}),
		// An older frame, kept at its own time.
		frameBatch(testTSMS-2, testFrame{id: 100, data: plainData}),
		// Two batches in one buffer, a second later, with the mux page changing.
		append(frameBatch(testTSMS+1000, testFrame{id: 101, data: muxData(1)}),
			frameBatch(testTSMS+1000, testFrame{id: 100, data: plainData})...),
	}

	live := &recordSink{}
	liveSession := newFrameSessionID("S1", live)
	liveDec := newTestDecoder(t, dir, liveSession)
	for _, b := range bufs {
		liveDec.decode(b)
	}
	liveSession.Close(time.Second)
	if n := liveDec.dec.Stats.Frames; n != 7 {
		t.Fatalf("live decoder took %d frames, want 7", n)
	}

	dataDir, segDir := backfillDirs(t)
	storeBuffers(t, dataDir, bufs...)
	logSession(t, segDir, "S1", 1)
	replayed := &recordSink{}
	results, err := runBackfill(backfillOptions{SegmentsDir: segDir, CAN: CANConfig{DBCDir: dir}, All: true}, replayed)
	if err != nil {
		t.Fatal(err)
	}
	if replayed.closed {
		t.Fatal("runBackfill closed the sink")
	}
	if len(results) != 1 || results[0].Records != 3 || results[0].Frames != 7 || results[0].BadClock != 0 {
		t.Fatalf("results %+v, want one session with 3 records, 7 frames, no bad clock", results)
	}

	liveLines, replayedLines := live.lines(t), replayed.lines(t)
	for _, prefix := range []string{"frame,", "signal_def,"} {
		want := rowsWithPrefix(liveLines, prefix)
		got := rowsWithPrefix(replayedLines, prefix)
		slices.Sort(want)
		slices.Sort(got)
		if len(want) == 0 || !slices.Equal(got, want) {
			t.Fatalf("%s rows differ\nlive:     %q\nbackfill: %q", prefix, want, got)
		}
	}
	last := func(lines []string) string {
		rows := rowsWithPrefix(lines, "session,")
		if len(rows) == 0 {
			t.Fatal("no session row")
		}
		return rows[len(rows)-1]
	}
	if got, want := last(replayedLines), last(liveLines); got != want {
		t.Fatalf("final session row %q, want %q", got, want)
	}
}

// Two runs in one store each replay under their own session tag, and -session
// picks one. A name with no records is an error, and nothing is written first.
func TestBackfillSelectsSessions(t *testing.T) {
	dir := writeFrameTestDBC(t)
	dataDir, segDir := backfillDirs(t)
	storeBuffers(t, dataDir,
		frameBatch(testTSMS, testFrame{id: 100, data: plainData}),
		frameBatch(testTSMS+1000, testFrame{id: 100, data: plainData}))
	logSession(t, segDir, "S1", 1)
	storeBuffers(t, dataDir,
		frameBatch(testTSMS+5000, testFrame{id: 101, data: muxData(1)}),
		frameBatch(testTSMS+6000, testFrame{id: 101, data: muxData(2)}))
	logSession(t, segDir, "S2", 3)
	opts := backfillOptions{SegmentsDir: segDir, CAN: CANConfig{DBCDir: dir}}

	only := &recordSink{}
	opts.Sessions = []string{"S2"}
	if _, err := runBackfill(opts, only); err != nil {
		t.Fatal(err)
	}
	if got := frameSessions(only.lines(t)); !slices.Equal(got, []string{"S2", "S2"}) {
		t.Fatalf("selecting S2 wrote frame rows for %v", got)
	}

	all := &recordSink{}
	opts.Sessions, opts.All = nil, true
	results, err := runBackfill(opts, all)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 2 || results[0].SID != "S1" || results[1].SID != "S2" {
		t.Fatalf("results %+v, want S1 then S2", results)
	}
	if got := frameSessions(all.lines(t)); !slices.Equal(got, []string{"S1", "S1", "S2", "S2"}) {
		t.Fatalf("-all wrote frame rows for %v", got)
	}

	failed := &recordSink{}
	opts.All, opts.Sessions = false, []string{"S1", "S9"}
	if _, err := runBackfill(opts, failed); err == nil || !strings.Contains(err.Error(), "S9") {
		t.Fatalf("unknown session: err = %v, want an error naming S9", err)
	}
	if len(failed.bodies) != 0 {
		t.Fatalf("%d writes happened before the unknown session was refused", len(failed.bodies))
	}
}

// Records written before sessions.log existed have no recorded run. They all go
// to one session, named from the write time of the first of them.
func TestBackfillFallbackSession(t *testing.T) {
	dir := writeFrameTestDBC(t)
	_, segDir := backfillDirs(t)
	old := uint32(time.Date(2024, 6, 1, 12, 0, 0, 0, time.UTC).Unix())
	later := old + 3600
	writeSegment(t, segDir, 1,
		segmentRecord{id: 1, writeUnix: old, payload: frameBatch(uint64(old)*1000, testFrame{id: 100, data: plainData})},
		segmentRecord{id: 2, writeUnix: old, payload: frameBatch(uint64(old)*1000+1, testFrame{id: 101, data: muxData(1)})})
	writeSegment(t, segDir, 3,
		segmentRecord{id: 3, writeUnix: later, payload: frameBatch(uint64(later)*1000, testFrame{id: 100, data: plainData})})
	logSession(t, segDir, "S2", 3)

	sessions, err := listBackfillSessions(segDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(sessions) != 2 || sessions[0].SID != "20240601T120000Z" || sessions[0].Records != 2 ||
		sessions[0].FirstID != 1 || sessions[0].LastID != 2 || sessions[1].SID != "S2" || sessions[1].Records != 1 {
		t.Fatalf("sessions %+v, want the fallback run with records 1-2, then S2", sessions)
	}

	sink := &recordSink{}
	if _, err := runBackfill(backfillOptions{SegmentsDir: segDir, CAN: CANConfig{DBCDir: dir}, All: true}, sink); err != nil {
		t.Fatal(err)
	}
	want := []string{"20240601T120000Z", "20240601T120000Z", "S2"}
	if got := frameSessions(sink.lines(t)); !slices.Equal(got, want) {
		t.Fatalf("frame rows carry sessions %v, want %v", got, want)
	}
}

// The bad-clock check uses the time a buffer was written, not the time of the
// replay. A frame 30 s after its write time is kept; one 120 s after is dropped
// and counted. Replayed today, both would pass, since the write was long ago.
func TestBackfillBadClockUsesWriteTime(t *testing.T) {
	dir := writeFrameTestDBC(t)
	_, segDir := backfillDirs(t)
	written := time.Date(2024, 6, 1, 12, 0, 0, 0, time.UTC)
	buf := append(frameBatch(uint64(written.Add(30*time.Second).UnixMilli()), testFrame{id: 100, data: plainData}),
		frameBatch(uint64(written.Add(120*time.Second).UnixMilli()), testFrame{id: 100, data: plainData})...)
	writeSegment(t, segDir, 1, segmentRecord{id: 1, writeUnix: uint32(written.Unix()), payload: buf})
	logSession(t, segDir, "S1", 1)

	sink := &recordSink{}
	results, err := runBackfill(backfillOptions{SegmentsDir: segDir, CAN: CANConfig{DBCDir: dir}, All: true}, sink)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 || results[0].Frames != 1 || results[0].BadClock != 1 {
		t.Fatalf("results %+v, want 1 frame kept and 1 bad clock", results)
	}
	rows := rowsWithPrefix(sink.lines(t), "frame,")
	if want := fmt.Sprint(written.Add(30 * time.Second).UnixNano()); len(rows) != 1 || !strings.HasSuffix(rows[0], " "+want) {
		t.Fatalf("frame rows %q, want the one kept at %s", rows, want)
	}
}

// A session whose last record is within SkipRecent of now may still be live, so
// it is skipped, unless it is named.
func TestBackfillSkipsRecentSession(t *testing.T) {
	dir := writeFrameTestDBC(t)
	_, segDir := backfillDirs(t)
	now := time.Date(2024, 6, 1, 12, 0, 0, 0, time.UTC)
	at := func(ago time.Duration) (uint32, uint64) {
		when := now.Add(-ago)
		return uint32(when.Unix()), uint64(when.UnixMilli())
	}
	oldWrite, oldMS := at(10 * time.Minute)
	newWrite, newMS := at(30 * time.Second)
	writeSegment(t, segDir, 1, segmentRecord{id: 1, writeUnix: oldWrite, payload: frameBatch(oldMS, testFrame{id: 100, data: plainData})})
	writeSegment(t, segDir, 2, segmentRecord{id: 2, writeUnix: newWrite, payload: frameBatch(newMS, testFrame{id: 100, data: plainData})})
	logSession(t, segDir, "S1", 1)
	logSession(t, segDir, "S2", 2)
	opts := backfillOptions{
		SegmentsDir: segDir,
		CAN:         CANConfig{DBCDir: dir},
		All:         true,
		SkipRecent:  2 * time.Minute,
		Now:         func() time.Time { return now },
	}

	sink := &recordSink{}
	results, err := runBackfill(opts, sink)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 || results[0].SID != "S1" {
		t.Fatalf("results %+v, want only S1 (S2 is recent)", results)
	}
	if got := frameSessions(sink.lines(t)); !slices.Equal(got, []string{"S1"}) {
		t.Fatalf("frame rows carry sessions %v, want [S1]", got)
	}

	opts.All, opts.Sessions = false, []string{"S2"}
	sink = &recordSink{}
	results, err = runBackfill(opts, sink)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 || results[0].SID != "S2" {
		t.Fatalf("named S2: results %+v, want S2 replayed", results)
	}
	if got := frameSessions(sink.lines(t)); !slices.Equal(got, []string{"S2"}) {
		t.Fatalf("named S2: frame rows carry sessions %v, want [S2]", got)
	}
}

// -list prints one line per recorded run.
func TestBackfillMainList(t *testing.T) {
	dataDir, segDir := backfillDirs(t)
	writeTwoRuns(t, segDir)
	var out bytes.Buffer
	if code := backfillMain([]string{"-data-dir", dataDir, "-list"}, noEnvironment, &out); code != 0 {
		t.Fatalf("exit %d\n%s", code, out.String())
	}
	lines := strings.Split(strings.TrimSpace(out.String()), "\n")
	if len(lines) != 2 || !strings.HasPrefix(lines[0], "S1  records 2  ") || !strings.HasPrefix(lines[1], "S2  records 1  ") {
		t.Fatalf("list output:\n%s", out.String())
	}
	if !strings.Contains(lines[0], "ids 1-2") || !strings.Contains(lines[0], "first 2024-06-01T12:00:00Z") {
		t.Fatalf("S1 line lacks its id range or write time: %s", lines[0])
	}
}

// Missing a selection, or asking for -influx-url off with a write, is a usage
// error (2). An unrecorded session is an error (1).
func TestBackfillMainExitCodes(t *testing.T) {
	dataDir, segDir := backfillDirs(t)
	writeTwoRuns(t, segDir)
	cases := []struct {
		name string
		args []string
		want int
	}{
		{"no selection", []string{"-data-dir", dataDir}, 2},
		{"influx off for a write", []string{"-data-dir", dataDir, "-all", "-influx-url", "off"}, 2},
		{"unrecorded session", []string{"-data-dir", dataDir, "-session", "S9", "-dry-run"}, 1},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var out bytes.Buffer
			if got := backfillMain(tc.args, noEnvironment, &out); got != tc.want {
				t.Fatalf("exit %d, want %d\n%s", got, tc.want, out.String())
			}
		})
	}
}

// A dry run counts the rows and makes no request to InfluxDB.
func TestBackfillMainDryRunMakesNoRequest(t *testing.T) {
	dataDir, segDir := backfillDirs(t)
	writeTwoRuns(t, segDir)
	dir := writeFrameTestDBC(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("dry run sent %s %s", r.Method, r.URL.Path)
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer srv.Close()

	var out bytes.Buffer
	args := []string{"-data-dir", dataDir, "-dbc-dir", dir, "-all", "-dry-run", "-skip-recent", "0", "-influx-url", srv.URL}
	if code := backfillMain(args, noEnvironment, &out); code != 0 {
		t.Fatalf("exit %d\n%s", code, out.String())
	}
	text := out.String()
	for _, want := range []string{
		"session S1: 2 records, 2 frames, 0 bad clock,",
		"session S2: 1 records, 1 frames, 0 bad clock,",
		"total: 2 sessions, 3 records, 3 frames",
		"dry run: nothing was written",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("output lacks %q:\n%s", want, text)
		}
	}
}

// A write posts the rows to InfluxDB, into the database named, and reports none
// dropped.
func TestBackfillMainWritesToInflux(t *testing.T) {
	dataDir, segDir := backfillDirs(t)
	writeTwoRuns(t, segDir)
	dir := writeFrameTestDBC(t)
	f := &fakeInflux{}
	srv := httptest.NewServer(f)
	defer srv.Close()

	args := []string{"-data-dir", dataDir, "-dbc-dir", dir, "-all", "-skip-recent", "0",
		"-influx-url", srv.URL, "-influx-db", "photon_test"}
	var out bytes.Buffer
	if code := backfillMain(args, noEnvironment, &out); code != 0 {
		t.Fatalf("exit %d\n%s", code, out.String())
	}
	body := strings.Join(f.bodies, "")
	for _, want := range []string{"frame,session=S1,msg=Plain", "frame,session=S2,msg=Muxed,tap=1"} {
		if !strings.Contains(body, want) {
			t.Fatalf("InfluxDB got no %q row", want)
		}
	}
	for _, r := range f.reqs {
		if got := r.URL.Query().Get("db"); got != "photon_test" {
			t.Fatalf("write went to database %q", got)
		}
	}
	if !strings.Contains(out.String(), "dropped 0") {
		t.Fatalf("output does not report zero dropped:\n%s", out.String())
	}
}

// A row InfluxDB refuses is dropped, and the command says so with exit 1.
func TestBackfillMainReportsDroppedRows(t *testing.T) {
	dataDir, segDir := backfillDirs(t)
	writeTwoRuns(t, segDir)
	dir := writeFrameTestDBC(t)
	f := &fakeInflux{status: []int{http.StatusUnauthorized}} // refused, not retried
	srv := httptest.NewServer(f)
	defer srv.Close()

	args := []string{"-data-dir", dataDir, "-dbc-dir", dir, "-all", "-skip-recent", "0", "-influx-url", srv.URL}
	var out bytes.Buffer
	if code := backfillMain(args, noEnvironment, &out); code != 1 {
		t.Fatalf("exit %d, want 1 for refused rows\n%s", code, out.String())
	}
}
