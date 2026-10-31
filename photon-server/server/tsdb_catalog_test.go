package main

import (
	"os"
	"path/filepath"
	"server/cache"
	"testing"
)

func TestDriveDaysFromCatalog(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "tsdb", "20260723-212137-00")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "session.json"), []byte(`{"samples":3}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "signals.json"), []byte(`[{"id":1,"message":"MC_VelocityMeasurement","name":"MC_MotorVelocity","unit":"rpm","min":0,"max":0}]`), 0o644); err != nil {
		t.Fatal(err)
	}
	catalog := `{"sessions":[{"id":"20260723-212137-00","dbc_sha":"abc","min_ms":10,"max_ms":4010,"path":"` + filepath.ToSlash(dir) + `"}]}`
	if err := os.WriteFile(filepath.Join(root, "tsdb", "catalog.json"), []byte(catalog), 0o644); err != nil {
		t.Fatal(err)
	}
	days := driveDays(root)
	if len(days) != 1 {
		t.Fatalf("days %d", len(days))
	}
	if days[0]["date"] != "2026-07-23" || days[0]["session_count"] != 1 {
		t.Fatalf("day %+v", days[0])
	}
	if _, err := loadTsdbSignals(dir); err != nil {
		t.Fatal(err)
	}
	// A second read must hit the catalog cache and still see the session.
	if again := loadTsdbCatalog(root); len(again) != 1 || again[0].ID != "20260723-212137-00" {
		t.Fatalf("cached catalog %+v", again)
	}
}

func TestAttachMessageIDsSplitsSavedRunCatalog(t *testing.T) {
	text := `
BO_ 1641 BPS_Module_Override_Ack: 8 BPS
 SG_ Module18_Override : 36|2@1+ (1,0) [0|3] "" Vector__XXX
BO_ 1056 MC_OdometerBusAhMeasurement: 8 MC
 SG_ MC_TripOdometer : 0|16@1+ (1,0) [0|0] "" Vector__XXX
`
	defs, err := cache.ParseDBC(text)
	if err != nil {
		t.Fatal(err)
	}
	signals := []map[string]any{
		{"id": uint32(7935720), "message": "BPS_Module_Override_Ack", "name": "Module18_Override"},
		{"id": uint32(20666351), "message": "MC_OdometerBusAhMeasurement", "name": "MC_TripOdometer"},
	}
	attachMessageIDs(signals, canIDsByMessage(defs))
	if signals[0]["message_id"] != uint32(1641) || signals[1]["message_id"] != uint32(1056) {
		t.Fatalf("ids %#v %#v", signals[0]["message_id"], signals[1]["message_id"])
	}
}
