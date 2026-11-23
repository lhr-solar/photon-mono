package main

import (
	"crypto"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	// accessCertsMinGap is the least time between two fetches of the team's certs,
	// so tokens with unknown key IDs cannot make the server fetch on every request.
	accessCertsMinGap = time.Minute
	// accessCertsMaxAge is how long fetched certs are trusted before a refetch.
	accessCertsMaxAge = time.Hour
	// accessClockSkew is the slack allowed on exp and nbf.
	accessClockSkew = 30 * time.Second
)

// accessVerifier checks the JWT that Cloudflare Access adds to each request, in
// the Cf-Access-Jwt-Assertion header. The keys are the team's signing certs,
// fetched from the team and cached.
type accessVerifier struct {
	issuer   string
	aud      string
	certsURL string
	client   *http.Client
	now      func() time.Time

	mu        sync.Mutex
	keys      map[string]*rsa.PublicKey
	fetched   time.Time // when keys were last fetched
	attempted time.Time // when a fetch was last tried, whether it worked or not
}

// newAccessVerifier checks tokens for the Access application aud on team.
func newAccessVerifier(team, aud string) *accessVerifier {
	issuer := "https://" + team + ".cloudflareaccess.com"
	return &accessVerifier{
		issuer:   issuer,
		aud:      aud,
		certsURL: issuer + "/cdn-cgi/access/certs",
		client:   &http.Client{Timeout: 5 * time.Second},
		now:      time.Now,
	}
}

// Verify checks an RS256 token and returns the email it names. The errors say
// which check failed; the token itself is never put in one.
func (v *accessVerifier) Verify(token string) (string, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return "", errors.New("jwt: want three parts")
	}
	header, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return "", errors.New("jwt: bad header encoding")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return "", errors.New("jwt: bad payload encoding")
	}
	sig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return "", errors.New("jwt: bad signature encoding")
	}

	var hdr struct {
		Alg string `json:"alg"`
		Kid string `json:"kid"`
	}
	if err := json.Unmarshal(header, &hdr); err != nil {
		return "", errors.New("jwt: bad header json")
	}
	if hdr.Alg != "RS256" {
		return "", fmt.Errorf("jwt: alg %q is not RS256", hdr.Alg)
	}
	key, err := v.key(hdr.Kid)
	if err != nil {
		return "", err
	}
	digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	if err := rsa.VerifyPKCS1v15(key, crypto.SHA256, digest[:], sig); err != nil {
		return "", errors.New("jwt: bad signature")
	}

	var claims struct {
		Iss   string   `json:"iss"`
		Aud   audience `json:"aud"`
		Exp   float64  `json:"exp"`
		Nbf   *float64 `json:"nbf"`
		Email string   `json:"email"`
	}
	if err := json.Unmarshal(payload, &claims); err != nil {
		return "", errors.New("jwt: bad claims json")
	}
	now := v.now()
	if claims.Iss != v.issuer {
		return "", errors.New("jwt: wrong issuer")
	}
	if !claims.Aud.has(v.aud) {
		return "", errors.New("jwt: wrong audience")
	}
	if claims.Exp <= float64(now.Add(-accessClockSkew).Unix()) {
		return "", errors.New("jwt: expired")
	}
	if claims.Nbf != nil && *claims.Nbf > float64(now.Add(accessClockSkew).Unix()) {
		return "", errors.New("jwt: not valid yet")
	}
	if claims.Email == "" {
		return "", errors.New("jwt: no email")
	}
	return claims.Email, nil
}

// key returns the signing key with ID kid. The certs are fetched when they are
// older than accessCertsMaxAge or the ID is unknown, but never more often than
// accessCertsMinGap. A failed fetch keeps the keys already held.
func (v *accessVerifier) key(kid string) (*rsa.PublicKey, error) {
	v.mu.Lock()
	defer v.mu.Unlock()
	now := v.now()
	fresh := v.keys != nil && now.Sub(v.fetched) < accessCertsMaxAge
	if k, ok := v.keys[kid]; ok && fresh {
		return k, nil
	}
	if !v.attempted.IsZero() && now.Sub(v.attempted) < accessCertsMinGap {
		if k, ok := v.keys[kid]; ok {
			return k, nil
		}
		return nil, errors.New("jwt: unknown key id")
	}
	v.attempted = now
	keys, err := v.fetchKeys()
	if err != nil {
		if k, ok := v.keys[kid]; ok {
			return k, nil
		}
		return nil, fmt.Errorf("jwt: fetching certs: %w", err)
	}
	v.keys, v.fetched = keys, now
	if k, ok := keys[kid]; ok {
		return k, nil
	}
	return nil, errors.New("jwt: unknown key id")
}

// fetchKeys downloads the team's certs and returns the RSA keys among them.
func (v *accessVerifier) fetchKeys() (map[string]*rsa.PublicKey, error) {
	resp, err := v.client.Get(v.certsURL)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("status %d", resp.StatusCode)
	}
	var doc struct {
		Keys []struct {
			Kid string `json:"kid"`
			Kty string `json:"kty"`
			N   string `json:"n"`
			E   string `json:"e"`
		} `json:"keys"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&doc); err != nil {
		return nil, err
	}
	keys := make(map[string]*rsa.PublicKey, len(doc.Keys))
	for _, k := range doc.Keys {
		if k.Kty != "RSA" {
			continue
		}
		n, errN := base64.RawURLEncoding.DecodeString(strings.TrimRight(k.N, "="))
		e, errE := base64.RawURLEncoding.DecodeString(strings.TrimRight(k.E, "="))
		if errN != nil || errE != nil || len(n) == 0 || len(e) == 0 {
			continue
		}
		exp := new(big.Int).SetBytes(e)
		if !exp.IsInt64() || exp.Int64() < 2 || exp.Int64() > 1<<31-1 {
			continue
		}
		keys[k.Kid] = &rsa.PublicKey{N: new(big.Int).SetBytes(n), E: int(exp.Int64())}
	}
	return keys, nil
}

// audience is the aud claim: one string, or a list of strings.
type audience []string

func (a *audience) UnmarshalJSON(b []byte) error {
	var one string
	if err := json.Unmarshal(b, &one); err == nil {
		*a = audience{one}
		return nil
	}
	var many []string
	if err := json.Unmarshal(b, &many); err != nil {
		return err
	}
	*a = many
	return nil
}

func (a audience) has(want string) bool {
	for _, s := range a {
		if s == want {
			return true
		}
	}
	return false
}
