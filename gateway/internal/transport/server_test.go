package transport

import (
	"context"
	"encoding/json"
	"net"
	"testing"
	"time"

	"arec/gateway/internal/protocol"
	"arec/gateway/internal/registry"
)

func startTestServer(t *testing.T) (*Server, *registry.Registry) {
	t.Helper()
	reg := registry.New()
	server := NewServer(reg)
	if err := server.Start("127.0.0.1:0"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = server.Close() })
	return server, reg
}

func registerFakeAgent(t *testing.T, address, agentID, platform string) net.Conn {
	t.Helper()
	conn, err := net.Dial("tcp", address)
	if err != nil {
		t.Fatal(err)
	}
	tool, _ := protocol.ToolByName("system_info")
	if err := writeTestMessage(conn, Message{
		Type:            "register",
		ProtocolVersion: protocol.Version,
		AgentID:         agentID,
		Platform:        platform,
		Tools:           []protocol.ToolDefinition{tool},
	}); err != nil {
		t.Fatal(err)
	}
	ack, err := readMessage(conn)
	if err != nil {
		t.Fatal(err)
	}
	if ack.Type != "register_ack" || ack.Accepted == nil || !*ack.Accepted {
		t.Fatalf("unexpected register ack: %+v", ack)
	}
	return conn
}

func writeTestMessage(conn net.Conn, message Message) error {
	payload, err := jsonMarshal(message)
	if err != nil {
		return err
	}
	return WriteFrame(conn, payload)
}

func jsonMarshal(value any) ([]byte, error) {
	return json.Marshal(value)
}

func TestRequestResponseAndQuery(t *testing.T) {
	server, reg := startTestServer(t)
	conn := registerFakeAgent(t, server.Addr(), "mac-test", "darwin")
	defer conn.Close()

	endpoint, ok := reg.Endpoint("mac-test")
	if !ok {
		t.Fatal("agent was not registered")
	}

	result := make(chan Message, 1)
	go func() {
		response, err := server.SendRequest(context.Background(), endpoint, Message{
			RequestID: "request-1",
		})
		if err != nil {
			t.Error(err)
			return
		}
		result <- response
	}()

	request, err := readMessage(conn)
	if err != nil {
		t.Fatal(err)
	}
	if request.Type != "request" || request.RequestID != "request-1" {
		t.Fatalf("unexpected request: %+v", request)
	}
	if err := writeTestMessage(conn, Message{
		Type:            "response",
		ProtocolVersion: protocol.Version,
		RequestID:       "request-1",
		Status:          protocol.StatusOK,
	}); err != nil {
		t.Fatal(err)
	}
	if response := <-result; response.Status != protocol.StatusOK {
		t.Fatalf("unexpected response: %+v", response)
	}

	queryResult := make(chan Message, 1)
	go func() {
		response, err := server.SendQuery(context.Background(), "mac-test", "request-1")
		if err != nil {
			t.Error(err)
			return
		}
		queryResult <- response
	}()
	query, err := readMessage(conn)
	if err != nil {
		t.Fatal(err)
	}
	if query.Type != "query" {
		t.Fatalf("unexpected query: %+v", query)
	}
	if err := writeTestMessage(conn, Message{
		Type:            "query_response",
		ProtocolVersion: protocol.Version,
		RequestID:       "request-1",
		Status:          protocol.StatusOK,
	}); err != nil {
		t.Fatal(err)
	}
	if response := <-queryResult; response.Status != protocol.StatusOK {
		t.Fatalf("unexpected query response: %+v", response)
	}
}

func TestDisconnectProducesUnknown(t *testing.T) {
	server, reg := startTestServer(t)
	conn := registerFakeAgent(t, server.Addr(), "mac-test", "darwin")
	endpoint, _ := reg.Endpoint("mac-test")

	result := make(chan Message, 1)
	go func() {
		response, err := server.SendRequest(context.Background(), endpoint, Message{RequestID: "request-disconnect"})
		if err != nil {
			t.Error(err)
			return
		}
		result <- response
	}()
	if _, err := readMessage(conn); err != nil {
		t.Fatal(err)
	}
	_ = conn.Close()

	select {
	case response := <-result:
		if response.Status != protocol.StatusUnknown {
			t.Fatalf("expected unknown, got %+v", response)
		}
	case <-time.After(time.Second):
		t.Fatal("timed out waiting for disconnect state")
	}
}
