package gateway

import (
	"bytes"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"arec/gateway/internal/protocol"
	"arec/gateway/internal/transport"
)

const (
	testBootstrap = "test-bootstrap-only"
	testSecret    = "test-hmac-secret-only"
)

type testApplication struct {
	app      *Gateway
	http     *httptest.Server
	audit    string
	dataAddr string
}

func newTestApplication(t *testing.T) *testApplication {
	t.Helper()
	auditPath := filepath.Join(t.TempDir(), "audit.jsonl")
	app, err := New(Config{
		BootstrapKey: testBootstrap,
		HMACSecret:   testSecret,
		AuditPath:    auditPath,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := app.StartData("127.0.0.1:0"); err != nil {
		t.Fatal(err)
	}
	httpServer := httptest.NewServer(app.Handler())
	system := &testApplication{
		app:      app,
		http:     httpServer,
		audit:    auditPath,
		dataAddr: app.DataAddr(),
	}
	t.Cleanup(func() {
		httpServer.Close()
		_ = app.Close()
	})
	return system
}

func (a *testApplication) createSession(t *testing.T, allowed []string) sessionResponse {
	t.Helper()
	status, response := a.request(t, http.MethodPost, "/session", "", map[string]any{
		"allowed_tools": allowed,
		"ttl_seconds":   900,
		"max_calls":     20,
	})
	if status != http.StatusOK {
		t.Fatalf("create session failed: %d %+v", status, response)
	}
	body, _ := json.Marshal(response)
	var created sessionResponse
	if err := json.Unmarshal(body, &created); err != nil {
		t.Fatal(err)
	}
	return created
}

func (a *testApplication) request(t *testing.T, method, path, token string, body any) (int, map[string]any) {
	t.Helper()
	var payload bytes.Buffer
	if body != nil {
		if err := json.NewEncoder(&payload).Encode(body); err != nil {
			t.Fatal(err)
		}
	}
	request, err := http.NewRequest(method, a.http.URL+path, &payload)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Content-Type", "application/json")
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	if path == "/session" {
		request.Header.Set("X-AREC-Bootstrap", testBootstrap)
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var decoded map[string]any
	if response.StatusCode != http.StatusNoContent {
		_ = json.NewDecoder(response.Body).Decode(&decoded)
	}
	return response.StatusCode, decoded
}

func (a *testApplication) tools(t *testing.T, token string) (int, []protocol.ToolDefinition) {
	t.Helper()
	request, err := http.NewRequest(http.MethodGet, a.http.URL+"/tools", nil)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Authorization", "Bearer "+token)
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var tools []protocol.ToolDefinition
	if err := json.NewDecoder(response.Body).Decode(&tools); err != nil {
		t.Fatal(err)
	}
	return response.StatusCode, tools
}

func registerTestAgent(t *testing.T, address, agentID, platform string) net.Conn {
	t.Helper()
	conn, err := net.Dial("tcp", address)
	if err != nil {
		t.Fatal(err)
	}
	tools := []protocol.ToolDefinition{
		mustTool("system_info"),
		mustTool("exec_command"),
		mustTool("read_file"),
	}
	if err := writeAgentMessage(conn, transport.Message{
		Type:            "register",
		ProtocolVersion: protocol.Version,
		AgentID:         agentID,
		Platform:        platform,
		Tools:           tools,
	}); err != nil {
		t.Fatal(err)
	}
	ack, err := readAgentMessage(conn)
	if err != nil {
		t.Fatal(err)
	}
	if ack.Type != "register_ack" || ack.Accepted == nil || !*ack.Accepted {
		t.Fatalf("registration failed: %+v", ack)
	}
	return conn
}

func mustTool(name string) protocol.ToolDefinition {
	tool, _ := protocol.ToolByName(name)
	return tool
}

func writeAgentMessage(conn net.Conn, message transport.Message) error {
	payload, err := json.Marshal(message)
	if err != nil {
		return err
	}
	return transport.WriteFrame(conn, payload)
}

func readAgentMessage(conn net.Conn) (transport.Message, error) {
	payload, err := transport.ReadFrame(conn, protocol.MaxFrameBytes)
	if err != nil {
		return transport.Message{}, err
	}
	var message transport.Message
	err = json.Unmarshal(payload, &message)
	return message, err
}

func TestBootstrapToolVisibilityAndScope(t *testing.T) {
	system := newTestApplication(t)

	request, _ := http.NewRequest(http.MethodPost, system.http.URL+"/session", strings.NewReader(`{}`))
	request.Header.Set("X-AREC-Bootstrap", "wrong")
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("wrong bootstrap returned %d", response.StatusCode)
	}

	created := system.createSession(t, []string{"system_info"})
	status, tools := system.tools(t, created.Token)
	if status != http.StatusOK {
		t.Fatalf("tools request failed: %d %+v", status, tools)
	}
	if len(tools) != 0 {
		t.Fatalf("expected no registered tools, got %+v", tools)
	}

	conn := registerTestAgent(t, system.dataAddr, "mac-test", "darwin")
	defer conn.Close()
	status, tools = system.tools(t, created.Token)
	if status != http.StatusOK || len(tools) != 1 {
		t.Fatalf("unexpected tools response: %d %+v", status, tools)
	}

	status, payload := system.request(t, http.MethodPost, "/execute", created.Token, map[string]any{
		"tool":            "exec_command",
		"params":          map[string]any{"command": "uname -s"},
		"target":          "darwin",
		"idempotency_key": "denied-key",
		"deadline_ms":     1000,
	})
	if status != http.StatusUnauthorized || payload["error_code"] != protocol.ErrUnauthorized {
		t.Fatalf("expected tool-scope rejection: %d %+v", status, payload)
	}
}

func TestExecuteRoutingIdempotencyAndAuditRedaction(t *testing.T) {
	system := newTestApplication(t)
	created := system.createSession(t, protocol.SupportedToolNames())
	conn := registerTestAgent(t, system.dataAddr, "windows-test", "windows")
	defer conn.Close()

	var dispatchCount atomic.Int32
	agentDone := make(chan error, 1)
	go func() {
		message, err := readAgentMessage(conn)
		if err != nil {
			agentDone <- err
			return
		}
		dispatchCount.Add(1)
		result, _ := json.Marshal(map[string]any{"stdout": "secret-command-output", "stderr": "", "exit_code": 0})
		agentDone <- writeAgentMessage(conn, transport.Message{
			Type:            "response",
			ProtocolVersion: protocol.Version,
			RequestID:       message.RequestID,
			Status:          protocol.StatusOK,
			Result:          result,
			Platform:        "windows",
			ExecutedAt:      time.Now().UTC().Format(time.RFC3339),
		})
	}()

	body := map[string]any{
		"tool":            "exec_command",
		"params":          map[string]any{"command": "echo secret-command-output"},
		"target":          "windows",
		"idempotency_key": "same-key",
		"deadline_ms":     2000,
	}
	status, payload := system.request(t, http.MethodPost, "/execute", created.Token, body)
	if status != http.StatusOK || payload["status"] != protocol.StatusOK || payload["platform"] != "windows" {
		t.Fatalf("unexpected execute response: %d %+v", status, payload)
	}
	if err := <-agentDone; err != nil {
		t.Fatal(err)
	}
	firstRequestID := payload["request_id"]

	status, duplicate := system.request(t, http.MethodPost, "/execute", created.Token, body)
	if status != http.StatusOK || duplicate["request_id"] != firstRequestID {
		t.Fatalf("duplicate was not deduplicated: %d %+v", status, duplicate)
	}
	if dispatchCount.Load() != 1 {
		t.Fatalf("expected one dispatch, got %d", dispatchCount.Load())
	}

	conflict := map[string]any{
		"tool":            "exec_command",
		"params":          map[string]any{"command": "echo different"},
		"target":          "windows",
		"idempotency_key": "same-key",
		"deadline_ms":     2000,
	}
	status, payload = system.request(t, http.MethodPost, "/execute", created.Token, conflict)
	if status != http.StatusConflict || payload["error_code"] != protocol.ErrIdempotencyConflict {
		t.Fatalf("expected idempotency conflict: %d %+v", status, payload)
	}

	autoAgentDone := make(chan error, 1)
	go func() {
		message, err := readAgentMessage(conn)
		if err != nil {
			autoAgentDone <- err
			return
		}
		dispatchCount.Add(1)
		result, _ := json.Marshal(map[string]any{"hostname": "windows-auto"})
		autoAgentDone <- writeAgentMessage(conn, transport.Message{
			Type:            "response",
			ProtocolVersion: protocol.Version,
			RequestID:       message.RequestID,
			Status:          protocol.StatusOK,
			Result:          result,
			Platform:        "windows",
			ExecutedAt:      time.Now().UTC().Format(time.RFC3339),
		})
	}()
	status, autoResponse := system.request(t, http.MethodPost, "/execute", created.Token, map[string]any{
		"tool":            "system_info",
		"params":          map[string]any{},
		"target":          "auto",
		"idempotency_key": "auto-key",
		"deadline_ms":     2000,
	})
	if status != http.StatusOK || autoResponse["platform"] != "windows" {
		t.Fatalf("auto route did not prefer the session's recent windows agent: %d %+v", status, autoResponse)
	}
	if err := <-autoAgentDone; err != nil {
		t.Fatal(err)
	}

	auditBody, err := os.ReadFile(system.audit)
	if err != nil {
		t.Fatal(err)
	}
	text := string(auditBody)
	for _, prohibited := range []string{created.Token, testBootstrap, testSecret, "echo secret-command-output", "secret-command-output"} {
		if strings.Contains(text, prohibited) {
			t.Fatalf("audit leaked %q: %s", prohibited, text)
		}
	}
	if !strings.Contains(text, `"params_hash"`) || !strings.Contains(text, `"target_platform":"windows"`) {
		t.Fatalf("audit is missing required fields: %s", text)
	}
}

func TestTimeoutAndUnknownQueryRecovery(t *testing.T) {
	system := newTestApplication(t)
	created := system.createSession(t, []string{"exec_command"})

	conn := registerTestAgent(t, system.dataAddr, "mac-test", "darwin")
	executeDone := make(chan struct{})
	var requestID string
	go func() {
		defer close(executeDone)
		status, payload := system.request(t, http.MethodPost, "/execute", created.Token, map[string]any{
			"tool":            "exec_command",
			"params":          map[string]any{"command": "sleep 10"},
			"target":          "darwin",
			"idempotency_key": "timeout-key",
			"deadline_ms":     1000,
		})
		if status != http.StatusOK || payload["status"] != protocol.StatusUnknown {
			t.Errorf("unexpected unknown response: %d %+v", status, payload)
		}
		requestID, _ = payload["request_id"].(string)
	}()
	message, err := readAgentMessage(conn)
	if err != nil {
		t.Fatal(err)
	}
	_ = conn.Close()
	<-executeDone

	reconnected := registerTestAgent(t, system.dataAddr, "mac-test", "darwin")
	defer reconnected.Close()
	queryDone := make(chan map[string]any, 1)
	go func() {
		_, payload := system.request(t, http.MethodGet, "/query/"+requestID, created.Token, nil)
		queryDone <- payload
	}()
	query, err := readAgentMessage(reconnected)
	if err != nil {
		t.Fatal(err)
	}
	if query.Type != "query" || query.RequestID != message.RequestID {
		t.Fatalf("unexpected query: %+v", query)
	}
	errorCode := protocol.ErrExecutionTimeout
	if err := writeAgentMessage(reconnected, transport.Message{
		Type:            "query_response",
		ProtocolVersion: protocol.Version,
		RequestID:       query.RequestID,
		Status:          protocol.StatusTimeout,
		ErrorCode:       &errorCode,
		Retryable:       false,
		Message:         "deadline exceeded; execution state unknown",
		Platform:        "darwin",
		ExecutedAt:      time.Now().UTC().Format(time.RFC3339),
	}); err != nil {
		t.Fatal(err)
	}
	payload := <-queryDone
	if payload["status"] != protocol.StatusTimeout || payload["error_code"] != protocol.ErrExecutionTimeout {
		t.Fatalf("unexpected recovered query: %+v", payload)
	}
}
