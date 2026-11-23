package main

import "testing"

// A wrong token counts as a failed attempt, and enough of them block the IP.
func TestCheckTokenBlocksAfterFailures(t *testing.T) {
	g := NewClientGate("secret")
	const ip = "10.1.2.3" // not a trusted hop, so the per-IP limits apply

	if err := g.CheckToken(ip, "secret"); err != nil {
		t.Fatalf("right token: %v", err)
	}
	for i := 1; i < maxAuthFailsPerMin; i++ {
		if err := g.CheckToken(ip, "wrong"); err != errAuthRejected {
			t.Fatalf("wrong token %d: got %v, want errAuthRejected", i, err)
		}
	}
	// One short of the limit, the IP can still connect.
	if err := g.AdmitIP(ip); err != nil {
		t.Fatalf("admit after %d failures: %v", maxAuthFailsPerMin-1, err)
	}
	g.ReleaseIP(ip)

	if err := g.CheckToken(ip, "wrong"); err != errAuthRejected {
		t.Fatalf("wrong token %d: got %v, want errAuthRejected", maxAuthFailsPerMin, err)
	}
	if err := g.AdmitIP(ip); err != errClientBusy {
		t.Fatalf("admit after %d failures: got %v, want errClientBusy", maxAuthFailsPerMin, err)
	}
}

// A nil gate lets everything through, as it always has.
func TestNilGateAdmitsEverything(t *testing.T) {
	var g *ClientGate
	if err := g.AdmitIP("10.1.2.3"); err != nil {
		t.Fatalf("admit: %v", err)
	}
	if err := g.CheckToken("10.1.2.3", "anything"); err != nil {
		t.Fatalf("check: %v", err)
	}
	g.ReleaseIP("10.1.2.3")
}
