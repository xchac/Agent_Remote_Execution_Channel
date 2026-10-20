package gateway

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"time"

	"arec/gateway/internal/audit"
	"arec/gateway/internal/auth"
	"arec/gateway/internal/protocol"
	"arec/gateway/internal/registry"
	"arec/gateway/internal/session"
	"arec/gateway/internal/state"
	"arec/gateway/internal/transport"
)

const maxRequestBody = 1 << 20

type Config struct {
	BootstrapKey string
	HMACSecret   string
	AuditPath    string
}

type Gateway struct {
	config   Config
	sessions *session.Manager
	registry *registry.Registry
	state    *state.Store
	audit    *audit.Logger
	data     *transport.Server
	logger   *log.Logger
}

type sessionRequest struct {
	AllowedTools []string `json:"allowed_tools"`
	TTLSeconds   int      `json:"ttl_seconds"`
	MaxCalls     int      `json:"max_calls"`
}

type sessionResponse struct {
	Token     string `json:"token"`
	TokenID   string `json:"token_id"`
	SessionID string `json:"session_id"`
	ExpiresAt string `json:"expires_at"`
}

type executeRequest struct {
	Tool           string         `json:"tool"`
	Params         map[string]any `json:"params"`
	Target         string         `json:"target"`
	IdempotencyKey string         `json:"idempotency_key"`
	DeadlineMS     int            `json:"deadline_ms"`
}

type executeResponse struct {
	RequestID string          `json:"request_id"`
	Status    string          `json:"status"`
	Result    json.RawMessage `json:"result,omitempty"`
	ErrorCode *string         `json:"error_code"`
	Retryable bool            `json:"retryable"`
	Message   string          `json:"message,omitempty"`
	Platform  string          `json:"platform,omitempty"`
}

type apiError struct {
	HTTPStatus int
	Code       string
	Retryable  bool
	Message    string
}

func New(config Config) (*Gateway, error) {
	if config.BootstrapKey == "" || config.HMACSecret == "" {
		return nil, errors.New("AREC_BOOTSTRAP_KEY and AREC_HMAC_SECRET are required")
	}
	if config.AuditPath == "" {
		return nil, errors.New("audit path is required")
	}
	logger, err := audit.New(config.AuditPath)
	if err != nil {
		return nil, err
	}
	reg := registry.New()
	return &Gateway{
		config: config,
		sessions: session.NewManager(session.Config{
			Secret:         []byte(config.HMACSecret),
			AllowedTools:   protocol.SupportedToolNames(),
			MaxTTL:         time.Duration(protocol.DefaultTTL) * time.Second,
			MaxCalls:       protocol.DefaultCalls,
			SessionRate:    10,
			SessionBurst:   20,
			ToolRate:       20,
			ToolBurst:      40,
			CleanupOnWrite: true,
		}),
		registry: reg,
		state:    state.New(time.Duration(protocol.Retention)*time.Millisecond, protocol.MaxCacheItems),
		audit:    logger,
		data:     transport.NewServer(reg),
		logger:   log.New(os.Stderr, "arec-gateway: ", log.LstdFlags),
	}, nil
}

func (g *Gateway) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /session", g.handleSession)
	mux.HandleFunc("GET /tools", g.handleTools)
	mux.HandleFunc("POST /execute", g.handleExecute)
	mux.HandleFunc("GET /query/{request_id}", g.handleQuery)
	mux.HandleFunc("GET /healthz", g.handleHealth)
	return mux
}

func (g *Gateway) StartData(address string) error {
	return g.data.Start(address)
}

func (g *Gateway) DataAddr() string {
	return g.data.Addr()
}

func (g *Gateway) Close() error {
	var errs []error
	if err := g.data.Close(); err != nil {
		errs = append(errs, err)
	}
	if err := g.audit.Close(); err != nil {
		errs = append(errs, err)
	}
	return errors.Join(errs...)
}

func (g *Gateway) handleHealth(writer http.ResponseWriter, _ *http.Request) {
	writeJSON(writer, http.StatusOK, map[string]string{"status": "ok"})
}

func (g *Gateway) handleSession(writer http.ResponseWriter, request *http.Request) {
	if !auth.KeyMatches(g.config.BootstrapKey, request.Header.Get("X-AREC-Bootstrap")) {
		g.writeAPIError(writer, apiError{HTTPStatus: http.StatusUnauthorized, Code: protocol.ErrUnauthorized, Message: "invalid bootstrap key"})
		return
	}
	var body sessionRequest
	if err := decodeJSON(request, &body); err != nil {
		g.writeAPIError(writer, apiError{HTTPStatus: http.StatusBadRequest, Code: protocol.ErrInvalidParams, Message: "invalid JSON body"})
		return
	}
	created, err := g.sessions.Create(body.AllowedTools, body.TTLSeconds, body.MaxCalls)
	if err != nil {
		g.writeAPIError(writer, apiError{HTTPStatus: http.StatusUnauthorized, Code: protocol.ErrUnauthorized, Message: "requested session scope is not allowed"})
		return
	}
	writeJSON(writer, http.StatusOK, sessionResponse{
		Token:     created.Token,
		TokenID:   created.TokenID,
		SessionID: created.SessionID,
		ExpiresAt: created.ExpiresAt.Format(time.RFC3339),
	})
}

func (g *Gateway) handleTools(writer http.ResponseWriter, request *http.Request) {
	info, ok := g.authorize(writer, request, "")
	if !ok {
		return
	}
	result := make([]protocol.ToolDefinition, 0, len(info.AllowedTools))
	for _, name := range info.AllowedTools {
		definition, found := protocol.ToolByName(name)
		if !found {
			continue
		}
		platforms := availablePlatforms(g.registry.Candidates(name, "auto", ""))
		if len(platforms) == 0 {
			continue
		}
		definition.Platform = platforms
		result = append(result, definition)
	}
	writeJSON(writer, http.StatusOK, result)
}

func (g *Gateway) handleExecute(writer http.ResponseWriter, request *http.Request) {
	started := time.Now().UTC()
	info, ok := g.authorize(writer, request, "")
	if !ok {
		return
	}
	var body executeRequest
	if err := decodeJSON(request, &body); err != nil {
		g.reject(writer, info, "", body.Tool, body.Params, started, apiError{
			HTTPStatus: http.StatusBadRequest,
			Code:       protocol.ErrInvalidParams,
			Message:    "invalid JSON body",
		})
		return
	}
	_, found := protocol.ToolByName(body.Tool)
	if !found || !contains(info.AllowedTools, body.Tool) {
		code := protocol.ErrToolNotFound
		status := http.StatusNotFound
		if found {
			code = protocol.ErrUnauthorized
			status = http.StatusUnauthorized
		}
		g.reject(writer, info, "", body.Tool, body.Params, started, apiError{
			HTTPStatus: status,
			Code:       code,
			Message:    "tool is not available to this session",
		})
		return
	}
	if err := validateExecuteRequest(&body); err != nil {
		g.reject(writer, info, "", body.Tool, body.Params, started, apiError{
			HTTPStatus: http.StatusBadRequest,
			Code:       protocol.ErrInvalidParams,
			Message:    err.Error(),
		})
		return
	}
	if err := validateParams(body.Tool, body.Params); err != nil {
		g.reject(writer, info, "", body.Tool, body.Params, started, apiError{
			HTTPStatus: http.StatusBadRequest,
			Code:       protocol.ErrInvalidParams,
			Message:    err.Error(),
		})
		return
	}

	paramsHash, err := audit.ParamsHash(body.Params)
	if err != nil {
		g.reject(writer, info, "", body.Tool, body.Params, started, apiError{
			HTTPStatus: http.StatusBadRequest,
			Code:       protocol.ErrInvalidParams,
			Message:    "params cannot be encoded",
		})
		return
	}
	requestID := newID()
	begin := g.state.Begin(state.Record{
		RequestID:      requestID,
		SessionID:      info.SessionID,
		TokenID:        info.TokenID,
		Tool:           body.Tool,
		ParamsHash:     paramsHash,
		IdempotencyKey: body.IdempotencyKey,
		Target:         body.Target,
		Status:         protocol.StatusPending,
	})
	if begin.Conflict {
		g.reject(writer, info, begin.Record.RequestID, body.Tool, body.Params, started, apiError{
			HTTPStatus: http.StatusConflict,
			Code:       protocol.ErrIdempotencyConflict,
			Message:    "idempotency key was already used with different input",
		})
		return
	}
	if !begin.Created {
		writeJSON(writer, http.StatusOK, responseFromRecord(begin.Record))
		return
	}
	requestID = begin.Record.RequestID

	candidates := g.registry.Candidates(body.Tool, body.Target, g.sessions.LastPlatform(info.SessionID))
	if len(candidates) == 0 {
		g.state.Delete(requestID)
		g.reject(writer, info, requestID, body.Tool, body.Params, started, apiError{
			HTTPStatus: http.StatusNotFound,
			Code:       protocol.ErrToolNotFound,
			Message:    "no healthy execution endpoint supports the requested target",
		})
		return
	}
	endpoint := candidates[0]
	if err := g.sessions.Consume(info, body.Tool); err != nil {
		g.state.Delete(requestID)
		if errors.Is(err, session.ErrRateLimited) {
			g.reject(writer, info, requestID, body.Tool, body.Params, started, apiError{
				HTTPStatus: http.StatusTooManyRequests,
				Code:       protocol.ErrRateLimited,
				Retryable:  true,
				Message:    "rate limit exceeded",
			})
			return
		}
		g.reject(writer, info, requestID, body.Tool, body.Params, started, apiError{
			HTTPStatus: http.StatusUnauthorized,
			Code:       protocol.ErrUnauthorized,
			Message:    "session is no longer authorized",
		})
		return
	}

	paramsJSON, _ := json.Marshal(body.Params)
	record, _ := g.state.Update(requestID, func(record *state.Record) {
		record.AgentID = endpoint.AgentID
		record.Platform = endpoint.Platform
		record.Status = protocol.StatusPending
	})
	g.sessions.SetLastPlatform(info.SessionID, endpoint.Platform)

	timeout := time.Duration(body.DeadlineMS+1500) * time.Millisecond
	contextWithDeadline, cancel := context.WithTimeout(request.Context(), timeout)
	defer cancel()
	response, sendErr := g.data.SendRequest(contextWithDeadline, endpoint, transport.Message{
		RequestID:      requestID,
		SessionID:      info.SessionID,
		Tool:           body.Tool,
		Params:         paramsJSON,
		IdempotencyKey: body.IdempotencyKey,
		DeadlineMS:     body.DeadlineMS,
		Token:          bearerToken(request),
	})

	if sendErr != nil {
		record, _ = g.state.Update(requestID, func(record *state.Record) {
			record.Status = protocol.StatusUnknown
			record.ErrorCode = nil
			record.Retryable = false
			record.Message = "execution state is unknown; query first"
		})
		g.auditRecord(info, record, started, "call_unknown")
		writeJSON(writer, http.StatusOK, responseFromRecord(record))
		return
	}
	record = g.applyDataResponse(requestID, response)
	g.auditRecord(info, record, started, "call")
	writeJSON(writer, http.StatusOK, responseFromRecord(record))
}

func (g *Gateway) handleQuery(writer http.ResponseWriter, request *http.Request) {
	started := time.Now().UTC()
	info, ok := g.authorize(writer, request, "")
	if !ok {
		return
	}
	requestID := request.PathValue("request_id")
	record, found := g.state.Get(requestID)
	if !found || record.SessionID != info.SessionID {
		writeJSON(writer, http.StatusNotFound, executeResponse{
			RequestID: requestID,
			Status:    protocol.StatusUnknown,
			ErrorCode: nil,
			Retryable: false,
			Message:   "request not found",
		})
		return
	}
	if !state.IsFinal(record.Status) && record.AgentID != "" {
		queryContext, cancel := context.WithTimeout(request.Context(), 2*time.Second)
		response, err := g.data.SendQuery(queryContext, record.AgentID, requestID)
		cancel()
		if err == nil {
			updated := g.applyDataResponse(requestID, response)
			if updated.Status != record.Status || updated.Result != nil {
				g.auditRecord(info, updated, started, "query")
			}
			record = updated
		}
	}
	writeJSON(writer, http.StatusOK, responseFromRecord(record))
}

func (g *Gateway) applyDataResponse(requestID string, response transport.Message) state.Record {
	status := response.Status
	if !validStatus(status) {
		status = protocol.StatusUnknown
		response.Message = "invalid execution response"
	}
	record, ok := g.state.Update(requestID, func(record *state.Record) {
		record.Status = status
		record.Result = append(json.RawMessage(nil), response.Result...)
		record.ErrorCode = response.ErrorCode
		record.Retryable = response.Retryable
		record.Message = response.Message
		if response.ExecutedAt != "" {
			value := response.ExecutedAt
			record.ExecutedAt = &value
		}
		if response.Platform != "" {
			record.Platform = response.Platform
		}
	})
	if !ok {
		return state.Record{RequestID: requestID, Status: protocol.StatusUnknown}
	}
	return record
}

func (g *Gateway) authorize(writer http.ResponseWriter, request *http.Request, tool string) (session.Info, bool) {
	token := bearerToken(request)
	if token == "" {
		g.writeAPIError(writer, apiError{HTTPStatus: http.StatusUnauthorized, Code: protocol.ErrUnauthorized, Message: "missing bearer token"})
		return session.Info{}, false
	}
	info, err := g.sessions.Authorize(token, tool)
	if err != nil {
		g.writeAPIError(writer, apiError{HTTPStatus: http.StatusUnauthorized, Code: protocol.ErrUnauthorized, Message: "invalid or expired capability token"})
		return session.Info{}, false
	}
	return info, true
}

func (g *Gateway) reject(writer http.ResponseWriter, info session.Info, requestID, tool string, params any, started time.Time, apiErr apiError) {
	hash, _ := audit.ParamsHash(params)
	errorCode := apiErr.Code
	g.auditRecord(info, state.Record{
		RequestID:  requestID,
		Tool:       tool,
		ParamsHash: hash,
		Status:     protocol.StatusFailed,
		ErrorCode:  &errorCode,
		Retryable:  apiErr.Retryable,
	}, started, "rejected")
	g.writeAPIError(writer, apiErr)
}

func (g *Gateway) auditRecord(info session.Info, record state.Record, started time.Time, event string) {
	if err := g.audit.Write(audit.Record{
		SessionID:      info.SessionID,
		TokenID:        info.TokenID,
		RequestID:      record.RequestID,
		Tool:           record.Tool,
		ParamsHash:     record.ParamsHash,
		TargetPlatform: record.Platform,
		Status:         record.Status,
		ErrorCode:      record.ErrorCode,
		DurationMS:     time.Since(started).Milliseconds(),
		ExecutedAt:     record.ExecutedAt,
		Event:          event,
	}); err != nil {
		g.logger.Printf("audit write failed for request %s: %v", record.RequestID, err)
	}
}

func (g *Gateway) writeAPIError(writer http.ResponseWriter, apiErr apiError) {
	errorCode := apiErr.Code
	writeJSON(writer, apiErr.HTTPStatus, executeResponse{
		Status:    protocol.StatusFailed,
		ErrorCode: &errorCode,
		Retryable: apiErr.Retryable,
		Message:   apiErr.Message,
	})
}

func responseFromRecord(record state.Record) executeResponse {
	return executeResponse{
		RequestID: record.RequestID,
		Status:    record.Status,
		Result:    record.Result,
		ErrorCode: record.ErrorCode,
		Retryable: record.Retryable,
		Message:   record.Message,
		Platform:  record.Platform,
	}
}

func validateExecuteRequest(request *executeRequest) error {
	if request.IdempotencyKey == "" || len(request.IdempotencyKey) > 256 {
		return errors.New("idempotency_key must contain 1 to 256 characters")
	}
	if request.DeadlineMS == 0 {
		request.DeadlineMS = protocol.DefaultTimeout
	}
	if request.DeadlineMS < 1 || request.DeadlineMS > protocol.MaxTimeout {
		return fmt.Errorf("deadline_ms must be between 1 and %d", protocol.MaxTimeout)
	}
	if request.Target == "" {
		request.Target = "auto"
	}
	if request.Target != "darwin" && request.Target != "windows" && request.Target != "auto" {
		return errors.New("target must be darwin, windows, or auto")
	}
	return nil
}

func validateParams(tool string, params map[string]any) error {
	if params == nil {
		params = map[string]any{}
	}
	switch tool {
	case "system_info":
		if len(params) != 0 {
			return errors.New("system_info does not accept parameters")
		}
	case "exec_command":
		if err := onlyKeys(params, "command", "cwd"); err != nil {
			return err
		}
		command, ok := params["command"].(string)
		if !ok || strings.TrimSpace(command) == "" || len(command) > 65536 {
			return errors.New("command must be a non-empty string up to 65536 bytes")
		}
		if cwd, exists := params["cwd"]; exists {
			value, ok := cwd.(string)
			if !ok || len(value) == 0 || len(value) > 4096 {
				return errors.New("cwd must be a non-empty string up to 4096 bytes")
			}
		}
	case "read_file":
		if err := onlyKeys(params, "path", "max_bytes"); err != nil {
			return err
		}
		path, ok := params["path"].(string)
		if !ok || strings.TrimSpace(path) == "" || len(path) > 4096 {
			return errors.New("path must be a non-empty string up to 4096 bytes")
		}
		if raw, exists := params["max_bytes"]; exists {
			value, ok := integerValue(raw)
			if !ok || value < 1 || value > 262144 {
				return errors.New("max_bytes must be an integer between 1 and 262144")
			}
		}
	default:
		return errors.New("unsupported tool")
	}
	return nil
}

func onlyKeys(params map[string]any, allowed ...string) error {
	set := make(map[string]struct{}, len(allowed))
	for _, key := range allowed {
		set[key] = struct{}{}
	}
	for key := range params {
		if _, ok := set[key]; !ok {
			return fmt.Errorf("unknown parameter %q", key)
		}
	}
	return nil
}

func integerValue(value any) (int64, bool) {
	switch typed := value.(type) {
	case json.Number:
		parsed, err := typed.Int64()
		return parsed, err == nil
	case float64:
		if typed != float64(int64(typed)) {
			return 0, false
		}
		return int64(typed), true
	case int:
		return int64(typed), true
	case int64:
		return typed, true
	default:
		return 0, false
	}
}

func decodeJSON(request *http.Request, destination any) error {
	decoder := json.NewDecoder(io.LimitReader(request.Body, maxRequestBody))
	decoder.DisallowUnknownFields()
	decoder.UseNumber()
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		return errors.New("request body must contain one JSON object")
	}
	return nil
}

func writeJSON(writer http.ResponseWriter, status int, value any) {
	writer.Header().Set("Content-Type", "application/json")
	writer.WriteHeader(status)
	_ = json.NewEncoder(writer).Encode(value)
}

func bearerToken(request *http.Request) string {
	header := request.Header.Get("Authorization")
	const prefix = "Bearer "
	if !strings.HasPrefix(header, prefix) {
		return ""
	}
	return strings.TrimSpace(strings.TrimPrefix(header, prefix))
}

func availablePlatforms(endpoints []registry.Endpoint) []string {
	seen := make(map[string]bool, 2)
	result := make([]string, 0, 2)
	for _, platform := range []string{"darwin", "windows"} {
		for _, endpoint := range endpoints {
			if endpoint.Platform == platform {
				if !seen[platform] {
					seen[platform] = true
					result = append(result, platform)
				}
				break
			}
		}
	}
	return result
}

func contains(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}

func validStatus(status string) bool {
	switch status {
	case protocol.StatusPending,
		protocol.StatusRunning,
		protocol.StatusOK,
		protocol.StatusTimeout,
		protocol.StatusFailed,
		protocol.StatusUnknown:
		return true
	default:
		return false
	}
}

func newID() string {
	return transportNewID()
}
