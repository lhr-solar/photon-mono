package main

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"server/dbc"
)

// dropTestDatabase deletes db from the InfluxDB 3 at url. A database that was
// never written to does not exist, which is fine.
func dropTestDatabase(t *testing.T, url, db string) {
	t.Helper()
	req, err := http.NewRequest(http.MethodDelete, strings.TrimRight(url, "/")+"/api/v3/configure/database?db="+db, nil)
	if err != nil {
		t.Errorf("delete %s: %v", db, err)
		return
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Errorf("delete %s: %v", db, err)
		return
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusNotFound {
		t.Errorf("delete %s: HTTP %d", db, resp.StatusCode)
	}
}

// TestBackfillIntoRealInfluxDB replays recorded frames into a fresh InfluxDB 3
// database, then replays them again. The second run must leave the same rows
// and values. It runs only when PHOTON_TEST_INFLUX names an InfluxDB 3, for
// example http://127.0.0.1:8181. The database is deleted afterwards: InfluxDB 3
// Core allows only 5 databases.
func TestBackfillIntoRealInfluxDB(t *testing.T) {
	url := os.Getenv("PHOTON_TEST_INFLUX")
	if url == "" {
		t.Skip("PHOTON_TEST_INFLUX is not set")
	}
	cat := loadTestCatalog(t)
	db := fmt.Sprintf("photon_test_%d", time.Now().UnixNano())
	t.Cleanup(func() { dropTestDatabase(t, url, db) })

	_, fl := findSignal(t, cat, func(ts tileSignal) bool {
		return ts.msg.Name == "MC_VelocityMeasurement" && ts.sig.Name == "MC_MotorVelocity"
	})
	_, in := findSignal(t, cat, func(ts tileSignal) bool {
		return ts.sig.Kind == dbc.Integer && ts.sig.Mux < 0 && ts.msg.Selector < 0 && len(ts.msg.Groups) == 0 &&
			ts.sig.Bits >= 8 && ts.msg.Length <= 8 && ts.msg.Name != "MC_VelocityMeasurement"
	})

	// Twenty buffers, 10 ms apart, two hours back. Each holds one frame of each
	// message, with values that are easy to check afterwards.
	const buffers = 20
	t0 := time.Now().Add(-2 * time.Hour).UnixMilli()
	var payloads [][]byte
	var wantFloat, wantInt []float64
	for k := range buffers {
		dFloat := make([]byte, fl.msg.Length)
		setSignalRaw(dFloat, fl.sig, uint64(math.Float32bits(float32(k)*0.25)))
		dInt := make([]byte, in.msg.Length)
		setSignalRaw(dInt, in.sig, uint64(k))
		v, _ := in.sig.RawInt(dInt)
		wantInt = append(wantInt, float64(v))
		wantFloat = append(wantFloat, float64(float32(k)*0.25))
		payloads = append(payloads, canp3Batch(uint64(t0)+uint64(k)*10,
			canpTestFrame{id: fl.msg.ID, data: dFloat},
			canpTestFrame{id: in.msg.ID, data: dInt}))
	}

	dataDir, segDir := backfillDirs(t)
	storeBuffers(t, dataDir, payloads...)
	logSession(t, segDir, "S1", 1)

	cfg, err := ParseConfigWithEnv(os.Getenv, nil, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	q := newInfluxQuerier(url, db, cfg.InfluxToken, 1)
	ctx := context.Background()
	count := func() string {
		rows, err := q.query(ctx, "SELECT count(*) AS n FROM frame", nil)
		if err != nil || len(rows) != 1 {
			t.Fatalf("count query: %v (%d rows)", err, len(rows))
		}
		return fmt.Sprint(rows[0]["n"])
	}
	// check reads field of every frame row for msg, in time order, and compares.
	check := func(msg, field string, want []float64) {
		t.Helper()
		rows, err := q.query(ctx, fmt.Sprintf(`SELECT "%s" FROM frame WHERE msg = '%s' ORDER BY time`, field, msg), nil)
		if err != nil {
			t.Fatalf("%s.%s query: %v", msg, field, err)
		}
		if len(rows) != len(want) {
			t.Fatalf("%s.%s: %d rows, want %d", msg, field, len(rows), len(want))
		}
		for i, r := range rows {
			got, err := strconv.ParseFloat(fmt.Sprint(r[field]), 64)
			if err != nil || got != want[i] {
				t.Fatalf("%s.%s row %d = %v (%v), want %v", msg, field, i, r[field], err, want[i])
			}
		}
	}

	args := []string{"-data-dir", dataDir, "-dbc-dir", testDBCDir, "-influx-url", url, "-influx-db", db,
		"-all", "-skip-recent", "0"}
	var counts [2]string
	for run := range 2 {
		var out bytes.Buffer
		if code := backfillMain(args, os.Getenv, &out); code != 0 {
			t.Fatalf("run %d: exit %d\n%s", run+1, code, out.String())
		}
		// InfluxDB makes a write queryable once its WAL flushes, about 250 ms
		// with the server's settings.
		time.Sleep(1500 * time.Millisecond)
		counts[run] = count()
	}

	if want := fmt.Sprint(2 * buffers); counts[0] != want || counts[1] != want {
		t.Fatalf("frame rows after run 1 and run 2: %s and %s, want %s each (the rerun must not add rows)",
			counts[0], counts[1], want)
	}
	check(fl.msg.Name, fl.sig.Name, wantFloat)
	check(in.msg.Name, in.sig.Name, wantInt)
}
