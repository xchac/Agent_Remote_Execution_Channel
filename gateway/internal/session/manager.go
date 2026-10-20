package session

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"sync"
	"time"

	"golang.org/x/time/rate"

	"arec/gateway/internal/auth"
	"arec/gateway/internal/protocol"
)

var (
	ErrUnauthorized = errors.New("unauthorized")
	ErrRateLimited  = errors.New("rate limited")
)

type Config struct {
	Secret         []byte
	AllowedTools   []string
	MaxTTL         time.Duration
	MaxCalls       int
	SessionRate    rate.Limit
	SessionBurst   int
	ToolRate       rate.Limit
	ToolBurst      int
	CleanupOnWrite bool
}

type Info struct {
	SessionID    string
	TokenID      string
	AllowedTools []string
	ExpiresAt    time.Time
}

type Created struct {
	Token     string
	TokenID   string
	SessionID string
	ExpiresAt time.Time
}

type sessionState struct {
	id           string
	tokenID      string
	allowed      map[string]struct{}
	allowedList  []string
	expiresAt    time.Time
	maxCalls     int
	calls        int
	limiter      *rate.Limiter
	lastPlatform string
}

type Manager struct {
	mu           sync.Mutex
	cfg          Config
	allowed      map[string]struct{}
	sessions     map[string]*sessionState
	toolLimiters map[string]*rate.Limiter
}

func NewManager(cfg Config) *Manager {
	if cfg.MaxTTL <= 0 {
		cfg.MaxTTL = time.Duration(protocol.DefaultTTL) * time.Second
	}
	if cfg.MaxCalls <= 0 {
		cfg.MaxCalls = protocol.DefaultCalls
	}
	if cfg.SessionRate <= 0 {
		cfg.SessionRate = 10
	}
	if cfg.SessionBurst <= 0 {
		cfg.SessionBurst = 20
	}
	if cfg.ToolRate <= 0 {
		cfg.ToolRate = 20
	}
	if cfg.ToolBurst <= 0 {
		cfg.ToolBurst = 40
	}
	allowed := make(map[string]struct{}, len(cfg.AllowedTools))
	for _, name := range cfg.AllowedTools {
		allowed[name] = struct{}{}
	}
	return &Manager{
		cfg:          cfg,
		allowed:      allowed,
		sessions:     make(map[string]*sessionState),
		toolLimiters: make(map[string]*rate.Limiter),
	}
}

func (m *Manager) Create(allowedTools []string, ttlSeconds, maxCalls int) (Created, error) {
	if len(allowedTools) == 0 {
		allowedTools = append([]string(nil), m.cfg.AllowedTools...)
	}
	if ttlSeconds <= 0 {
		ttlSeconds = protocol.DefaultTTL
	}
	if maxCalls <= 0 {
		maxCalls = protocol.DefaultCalls
	}
	ttl := time.Duration(ttlSeconds) * time.Second
	if ttl > m.cfg.MaxTTL || maxCalls > m.cfg.MaxCalls {
		return Created{}, ErrUnauthorized
	}

	allowed := make([]string, 0, len(allowedTools))
	for _, name := range allowedTools {
		if _, ok := m.allowed[name]; !ok {
			return Created{}, ErrUnauthorized
		}
		allowed = append(allowed, name)
	}

	now := time.Now().UTC()
	tokenID := newID()
	sessionID := newID()
	expiresAt := now.Add(ttl)
	payload := auth.Payload{
		Version:      protocol.TokenVersion,
		TokenID:      tokenID,
		SessionID:    sessionID,
		AllowedTools: allowed,
		IssuedAt:     now.Unix(),
		ExpiresAt:    expiresAt.Unix(),
		MaxCalls:     maxCalls,
	}
	token, err := auth.Sign(payload, m.cfg.Secret)
	if err != nil {
		return Created{}, err
	}

	state := &sessionState{
		id:          sessionID,
		tokenID:     tokenID,
		allowed:     make(map[string]struct{}, len(allowed)),
		allowedList: allowed,
		expiresAt:   expiresAt,
		maxCalls:    maxCalls,
		limiter:     rate.NewLimiter(m.cfg.SessionRate, m.cfg.SessionBurst),
	}
	for _, name := range allowed {
		state.allowed[name] = struct{}{}
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	if m.cfg.CleanupOnWrite {
		m.cleanupLocked(now)
	}
	m.sessions[sessionID] = state
	return Created{
		Token:     token,
		TokenID:   tokenID,
		SessionID: sessionID,
		ExpiresAt: expiresAt,
	}, nil
}

func (m *Manager) Authorize(token string, requiredTool string) (Info, error) {
	payload, err := auth.Verify(token, m.cfg.Secret, time.Now().UTC())
	if err != nil {
		return Info{}, ErrUnauthorized
	}

	m.mu.Lock()
	defer m.mu.Unlock()
	state, ok := m.sessions[payload.SessionID]
	if !ok || state.tokenID != payload.TokenID || !time.Now().UTC().Before(state.expiresAt) {
		return Info{}, ErrUnauthorized
	}
	if requiredTool != "" {
		if _, ok := state.allowed[requiredTool]; !ok {
			return Info{}, ErrUnauthorized
		}
	}
	return state.info(), nil
}

func (m *Manager) Consume(info Info, tool string) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	state, ok := m.sessions[info.SessionID]
	if !ok || state.tokenID != info.TokenID || !time.Now().UTC().Before(state.expiresAt) {
		return ErrUnauthorized
	}
	if _, ok := state.allowed[tool]; !ok {
		return ErrUnauthorized
	}
	if state.calls >= state.maxCalls {
		return ErrUnauthorized
	}
	toolLimiter, ok := m.toolLimiters[tool]
	if !ok {
		toolLimiter = rate.NewLimiter(m.cfg.ToolRate, m.cfg.ToolBurst)
		m.toolLimiters[tool] = toolLimiter
	}
	if !state.limiter.Allow() || !toolLimiter.Allow() {
		return ErrRateLimited
	}
	state.calls++
	return nil
}

func (m *Manager) SetLastPlatform(sessionID, platform string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if state, ok := m.sessions[sessionID]; ok {
		state.lastPlatform = platform
	}
}

func (m *Manager) LastPlatform(sessionID string) string {
	m.mu.Lock()
	defer m.mu.Unlock()
	if state, ok := m.sessions[sessionID]; ok {
		return state.lastPlatform
	}
	return ""
}

func (m *Manager) cleanupLocked(now time.Time) {
	for id, state := range m.sessions {
		if !now.Before(state.expiresAt) {
			delete(m.sessions, id)
		}
	}
}

func (s *sessionState) info() Info {
	return Info{
		SessionID:    s.id,
		TokenID:      s.tokenID,
		AllowedTools: append([]string(nil), s.allowedList...),
		ExpiresAt:    s.expiresAt,
	}
}

func newID() string {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		panic(err)
	}
	raw[6] = (raw[6] & 0x0f) | 0x40
	raw[8] = (raw[8] & 0x3f) | 0x80
	encoded := hex.EncodeToString(raw[:])
	return encoded[0:8] + "-" + encoded[8:12] + "-" + encoded[12:16] + "-" + encoded[16:20] + "-" + encoded[20:32]
}
