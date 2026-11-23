package main

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"
)

const (
	testTeam = "photon-test"
	testAud  = "aud-test-tag"
	testIss  = "https://photon-test.cloudflareaccess.com"
)

// The test keys are made once, since generating RSA keys is slow.
var (
	testKey1 = sync.OnceValue(genTestKey)
	testKey2 = sync.OnceValue(genTestKey)
)

func genTestKey() *rsa.PrivateKey {
	k, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		panic(err)
	}
	return k
}

// testJWKS serves the RSA keys it holds as the team's certs, by key ID. It can
// change while the verifier is using it, to simulate a rotation.
type testJWKS struct {
	mu   sync.Mutex
	keys map[string]*rsa.PublicKey
	srv  *httptest.Server
}

func newTestJWKS(t *testing.T) *testJWKS {
	t.Helper()
	j := &testJWKS{keys: make(map[string]*rsa.PublicKey)}
	j.srv = httptest.NewServer(http.HandlerFunc(j.serve))
	t.Cleanup(j.srv.Close)
	return j
}

func (j *testJWKS) set(kid string, pub *rsa.PublicKey) {
	j.mu.Lock()
	defer j.mu.Unlock()
	j.keys[kid] = pub
}

func (j *testJWKS) serve(w http.ResponseWriter, r *http.Request) {
	j.mu.Lock()
	defer j.mu.Unlock()
	type jwk struct {
		Kid string `json:"kid"`
		Kty string `json:"kty"`
		N   string `json:"n"`
		E   string `json:"e"`
	}
	var doc struct {
		Keys []jwk `json:"keys"`
	}
	for kid, pub := range j.keys {
		doc.Keys = append(doc.Keys, jwk{
			Kid: kid,
			Kty: "RSA",
			N:   base64.RawURLEncoding.EncodeToString(pub.N.Bytes()),
			E:   base64.RawURLEncoding.EncodeToString(big.NewInt(int64(pub.E)).Bytes()),
		})
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(doc)
}

// newTestVerifier checks tokens for the test team and AUD, with certs from jwks.
func newTestVerifier(t *testing.T, jwks *testJWKS) *accessVerifier {
	t.Helper()
	v := newAccessVerifier(testTeam, testAud)
	v.certsURL = jwks.srv.URL
	return v
}

// signToken returns an RS256 token with the header {alg, kid} and claims, signed
// with key.
func signToken(t *testing.T, key *rsa.PrivateKey, kid string, claims map[string]any) string {
	t.Helper()
	h := b64JSON(t, map[string]any{"alg": "RS256", "kid": kid})
	p := b64JSON(t, claims)
	digest := sha256.Sum256([]byte(h + "." + p))
	sig, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, digest[:])
	if err != nil {
		t.Fatal(err)
	}
	return h + "." + p + "." + base64.RawURLEncoding.EncodeToString(sig)
}

func b64JSON(t *testing.T, v any) string {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	return base64.RawURLEncoding.EncodeToString(b)
}

// goodClaims are the claims of a valid token for testAud, issued at now.
func goodClaims(now time.Time, email string) map[string]any {
	return map[string]any{
		"iss":   testIss,
		"aud":   testAud,
		"iat":   now.Unix(),
		"exp":   now.Add(time.Hour).Unix(),
		"email": email,
	}
}

func TestAccessVerify(t *testing.T) {
	now := time.Now()
	jwks := newTestJWKS(t)
	jwks.set("kid-1", &testKey1().PublicKey)
	v := newTestVerifier(t, jwks)
	v.now = func() time.Time { return now }

	// claimsWith returns the good claims with one change.
	claimsWith := func(key string, value any) map[string]any {
		c := goodClaims(now, "dev@example.com")
		c[key] = value
		return c
	}
	// unsigned builds a token with the given header and no signature.
	unsigned := func(header map[string]any) string {
		return b64JSON(t, header) + "." + b64JSON(t, goodClaims(now, "dev@example.com")) + "."
	}

	cases := []struct {
		name  string
		token string
		want  string // the email to get back, or "" when the token must be rejected
	}{
		{"valid", signToken(t, testKey1(), "kid-1", goodClaims(now, "dev@example.com")), "dev@example.com"},
		{"expired", signToken(t, testKey1(), "kid-1", claimsWith("exp", now.Add(-5*time.Minute).Unix())), ""},
		{"expired within skew", signToken(t, testKey1(), "kid-1", claimsWith("exp", now.Add(-10*time.Second).Unix())), "dev@example.com"},
		{"wrong aud", signToken(t, testKey1(), "kid-1", claimsWith("aud", "someone-else")), ""},
		{"aud list with ours", signToken(t, testKey1(), "kid-1", claimsWith("aud", []string{"other", testAud})), "dev@example.com"},
		{"aud list without ours", signToken(t, testKey1(), "kid-1", claimsWith("aud", []string{"other"})), ""},
		{"wrong iss", signToken(t, testKey1(), "kid-1", claimsWith("iss", "https://evil.cloudflareaccess.com")), ""},
		{"not yet valid", signToken(t, testKey1(), "kid-1", claimsWith("nbf", now.Add(time.Hour).Unix())), ""},
		{"no email", signToken(t, testKey1(), "kid-1", claimsWith("email", "")), ""},
		{"bad signature", signToken(t, testKey2(), "kid-1", goodClaims(now, "dev@example.com")), ""},
		{"alg none", unsigned(map[string]any{"alg": "none", "kid": "kid-1"}), ""},
		{"alg HS256", unsigned(map[string]any{"alg": "HS256", "kid": "kid-1"}), ""},
		{"garbage", "not-a-token", ""},
		{"two parts", "a.b", ""},
		{"empty", "", ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			email, err := v.Verify(tc.token)
			if tc.want == "" {
				if err == nil {
					t.Fatalf("accepted as %q, want rejected", email)
				}
				return
			}
			if err != nil {
				t.Fatalf("rejected: %v", err)
			}
			if email != tc.want {
				t.Fatalf("email = %q, want %q", email, tc.want)
			}
		})
	}
}

// A key the certs do not have is fetched again after a rotation, but not more
// often than accessCertsMinGap.
func TestAccessVerifyRefetchesForNewKey(t *testing.T) {
	now := time.Now()
	jwks := newTestJWKS(t)
	jwks.set("kid-1", &testKey1().PublicKey)
	v := newTestVerifier(t, jwks)
	clock := now
	v.now = func() time.Time { return clock }

	tok := signToken(t, testKey2(), "kid-2", goodClaims(now, "dev@example.com"))
	if _, err := v.Verify(tok); err == nil {
		t.Fatal("token with a key the team does not publish was accepted")
	}

	// The team rotates to kid-2. A retry within a minute is held back.
	jwks.set("kid-2", &testKey2().PublicKey)
	if _, err := v.Verify(tok); err == nil {
		t.Fatal("certs refetched less than a minute after the last fetch")
	}

	clock = now.Add(accessCertsMinGap + time.Second)
	email, err := v.Verify(tok)
	if err != nil {
		t.Fatalf("after the rotation: %v", err)
	}
	if email != "dev@example.com" {
		t.Fatalf("email = %q, want dev@example.com", email)
	}
}
