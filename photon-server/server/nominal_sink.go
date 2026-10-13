package main

import (
	"context"
	"log"
	"os"
	"strings"
	"sync"
	"time"

	"server/nominal"
)

func nominalStreamEnabled(cfg *Config) bool {
	rid := strings.TrimSpace(cfg.NominalDatasetRID)
	if rid == "" || rid == "-" || strings.EqualFold(rid, "off") {
		return false
	}
	key := strings.TrimSpace(cfg.NominalAPIKey)
	file := strings.TrimSpace(cfg.NominalKeyFile)
	if file == "" && (key == "" || strings.EqualFold(key, "off") || key == "-") {
		return false
	}
	return true
}

func nominalAPIKey(cfg *Config) (string, error) {
	if key := strings.TrimSpace(cfg.NominalAPIKey); key != "" && !strings.EqualFold(key, "off") && key != "-" {
		return key, nil
	}
	b, err := os.ReadFile(strings.TrimSpace(cfg.NominalKeyFile))
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(b)), nil
}

// liveNominal forwards CANP chunks to Nominal. A nil receiver is a no-op so
// the server runs with the stream off.
type liveNominal struct {
	sink   *nominal.Sink
	cancel context.CancelFunc
	once   sync.Once
}

func (n *liveNominal) Feed(payload []byte) {
	if n == nil || n.sink == nil {
		return
	}
	n.sink.Feed(payload)
}

func (n *liveNominal) Close() {
	if n == nil || n.sink == nil {
		return
	}
	n.once.Do(func() {
		errCh := make(chan error, 1)
		go func() { errCh <- n.sink.Close() }()
		select {
		case err := <-errCh:
			if err != nil {
				log.Printf("[photon-server] Nominal stream close: %v", err)
			}
		case <-time.After(3 * time.Second):
			log.Printf("[photon-server] Nominal stream close timed out")
		}
		n.cancel()
	})
}

// startNominalSink streams decoded CANP signals to a Nominal dataset.
// A missing key or dataset leaves the server running with the stream off.
func startNominalSink(cfg *Config) *liveNominal {
	if !nominalStreamEnabled(cfg) {
		log.Printf("[photon-server] Nominal stream off (set NOMINAL_API_KEY and NOMINAL_DATASET_RID)")
		return nil
	}
	key, err := nominalAPIKey(cfg)
	if err != nil || key == "" {
		log.Printf("[photon-server] Nominal stream skipped: api key unavailable")
		return nil
	}
	dbcPath := resolveDbc(cfg)
	if dbcPath == "" {
		log.Printf("[photon-server] Nominal stream skipped: no DBC (pass -dbc)")
		return nil
	}
	db, err := nominal.LoadDBC(dbcPath)
	if err != nil {
		log.Printf("[photon-server] Nominal stream skipped: dbc: %v", err)
		return nil
	}
	ctx, cancel := context.WithCancel(context.Background())
	sink, err := nominal.Open(ctx, nominal.Options{
		APIKey:     key,
		DatasetRID: strings.TrimSpace(cfg.NominalDatasetRID),
		BaseURL:    cfg.NominalBaseURL,
		DBC:        db,
	})
	if err != nil {
		cancel()
		log.Printf("[photon-server] Nominal stream skipped: %v", err)
		return nil
	}
	log.Printf("[photon-server] Nominal stream -> %s", strings.TrimSpace(cfg.NominalDatasetRID))
	return &liveNominal{sink: sink, cancel: cancel}
}
