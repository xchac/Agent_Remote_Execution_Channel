package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"strings"
	"time"

	"arec/gateway/internal/protocol"
)

var (
	ErrMalformedToken = errors.New("malformed token")
	ErrInvalidToken   = errors.New("invalid token")
	ErrExpiredToken   = errors.New("expired token")
)

type Payload struct {
	Version      int      `json:"version"`
	TokenID      string   `json:"token_id"`
	SessionID    string   `json:"session_id"`
	AllowedTools []string `json:"allowed_tools"`
	IssuedAt     int64    `json:"issued_at"`
	ExpiresAt    int64    `json:"expires_at"`
	MaxCalls     int      `json:"max_calls"`
}

func Sign(payload Payload, secret []byte) (string, error) {
	body, err := json.Marshal(payload)
	if err != nil {
		return "", err
	}
	encoded := base64.RawURLEncoding.EncodeToString(body)
	mac := hmac.New(sha256.New, secret)
	_, _ = mac.Write([]byte(encoded))
	signature := base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	return encoded + "." + signature, nil
}

func Verify(token string, secret []byte, now time.Time) (Payload, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 2 || parts[0] == "" || parts[1] == "" {
		return Payload{}, ErrMalformedToken
	}

	provided, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return Payload{}, ErrMalformedToken
	}
	mac := hmac.New(sha256.New, secret)
	_, _ = mac.Write([]byte(parts[0]))
	expected := mac.Sum(nil)
	if len(provided) != len(expected) || subtle.ConstantTimeCompare(provided, expected) != 1 {
		return Payload{}, ErrInvalidToken
	}

	body, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return Payload{}, ErrMalformedToken
	}
	var payload Payload
	if err := json.Unmarshal(body, &payload); err != nil {
		return Payload{}, ErrMalformedToken
	}
	if payload.Version != protocol.TokenVersion ||
		payload.TokenID == "" ||
		payload.SessionID == "" ||
		payload.ExpiresAt <= 0 ||
		payload.MaxCalls <= 0 {
		return Payload{}, ErrInvalidToken
	}
	if !now.Before(time.Unix(payload.ExpiresAt, 0)) {
		return Payload{}, ErrExpiredToken
	}
	return payload, nil
}

func KeyMatches(expected, provided string) bool {
	if expected == "" || provided == "" {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(expected), []byte(provided)) == 1
}
