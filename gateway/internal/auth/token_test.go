package auth

import (
	"errors"
	"testing"
	"time"

	"arec/gateway/internal/protocol"
)

func testPayload(now time.Time) Payload {
	return Payload{
		Version:      protocol.TokenVersion,
		TokenID:      "token-1",
		SessionID:    "session-1",
		AllowedTools: []string{"system_info"},
		IssuedAt:     now.Unix(),
		ExpiresAt:    now.Add(time.Minute).Unix(),
		MaxCalls:     3,
	}
}

func TestSignAndVerify(t *testing.T) {
	now := time.Now().UTC()
	token, err := Sign(testPayload(now), []byte("test-secret"))
	if err != nil {
		t.Fatal(err)
	}
	got, err := Verify(token, []byte("test-secret"), now)
	if err != nil {
		t.Fatal(err)
	}
	if got.SessionID != "session-1" || got.TokenID != "token-1" {
		t.Fatalf("unexpected payload: %+v", got)
	}
}

func TestVerifyRejectsTamperingAndExpiry(t *testing.T) {
	now := time.Now().UTC()
	token, err := Sign(testPayload(now), []byte("test-secret"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := Verify(token+"x", []byte("test-secret"), now); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("expected invalid token, got %v", err)
	}
	if _, err := Verify(token, []byte("other-secret"), now); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("expected invalid token, got %v", err)
	}
	if _, err := Verify(token, []byte("test-secret"), now.Add(2*time.Minute)); !errors.Is(err, ErrExpiredToken) {
		t.Fatalf("expected expired token, got %v", err)
	}
}

func TestKeyMatches(t *testing.T) {
	if !KeyMatches("a", "a") {
		t.Fatal("matching keys should pass")
	}
	if KeyMatches("a", "b") || KeyMatches("", "") {
		t.Fatal("mismatched keys should fail")
	}
}
