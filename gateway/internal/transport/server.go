package transport

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"

	"arec/gateway/internal/protocol"
	"arec/gateway/internal/registry"
)

var (
	ErrAgentOffline = errors.New("agent offline")
	ErrProtocol     = errors.New("protocol error")
)

type Server struct {
	registry *registry.Registry

	mu       sync.Mutex
	listener net.Listener
	conns    map[string]*connection
	waiters  map[string]*waiter
	closed   bool
}

type connection struct {
	server    *Server
	conn      net.Conn
	id        string
	endpoint  registry.Endpoint
	writeMu   sync.Mutex
	closeOnce sync.Once
	closed    chan struct{}
}

type waiter struct {
	agentID string
	channel chan Message
}

func NewServer(reg *registry.Registry) *Server {
	return &Server{
		registry: reg,
		conns:    make(map[string]*connection),
		waiters:  make(map[string]*waiter),
	}
}

func (s *Server) Start(address string) error {
	listener, err := net.Listen("tcp", address)
	if err != nil {
		return err
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		_ = listener.Close()
		return errors.New("transport server closed")
	}
	s.listener = listener
	s.mu.Unlock()
	go s.acceptLoop(listener)
	return nil
}

func (s *Server) Addr() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.listener == nil {
		return ""
	}
	return s.listener.Addr().String()
}

func (s *Server) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	listener := s.listener
	connections := make([]*connection, 0, len(s.conns))
	for _, conn := range s.conns {
		connections = append(connections, conn)
	}
	s.mu.Unlock()

	for _, conn := range connections {
		conn.close()
	}
	if listener != nil {
		return listener.Close()
	}
	return nil
}

func (s *Server) SendRequest(ctx context.Context, endpoint registry.Endpoint, message Message) (Message, error) {
	message.Type = "request"
	message.ProtocolVersion = protocol.Version
	return s.sendAndWait(ctx, endpoint.AgentID, endpoint.ConnectionID, "response", message)
}

func (s *Server) SendQuery(ctx context.Context, agentID, requestID string) (Message, error) {
	message := Message{
		Type:            "query",
		ProtocolVersion: protocol.Version,
		RequestID:       requestID,
	}
	return s.sendAndWait(ctx, agentID, "", "query_response", message)
}

func (s *Server) sendAndWait(ctx context.Context, agentID, connectionID, expectedType string, message Message) (Message, error) {
	if message.RequestID == "" {
		return Message{}, fmt.Errorf("%w: missing request_id", ErrProtocol)
	}
	key := waiterKey(expectedType, message.RequestID)
	entry := &waiter{agentID: agentID, channel: make(chan Message, 1)}

	s.mu.Lock()
	if existing, ok := s.waiters[key]; ok {
		s.mu.Unlock()
		return Message{}, fmt.Errorf("%w: duplicate request_id (existing agent %s)", ErrProtocol, existing.agentID)
	}
	conn := s.conns[agentID]
	if conn == nil || (connectionID != "" && conn.id != connectionID) {
		s.mu.Unlock()
		return Message{}, ErrAgentOffline
	}
	s.waiters[key] = entry
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		if s.waiters[key] == entry {
			delete(s.waiters, key)
		}
		s.mu.Unlock()
	}()

	if err := conn.writeMessage(message); err != nil {
		return Message{}, fmt.Errorf("%w: %v", ErrAgentOffline, err)
	}

	select {
	case response := <-entry.channel:
		if response.Status == "" {
			return Message{}, ErrProtocol
		}
		return response, nil
	case <-ctx.Done():
		return Message{}, ctx.Err()
	}
}

func (s *Server) acceptLoop(listener net.Listener) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			s.mu.Lock()
			closed := s.closed
			s.mu.Unlock()
			if !closed {
				time.Sleep(50 * time.Millisecond)
			}
			return
		}
		go s.handleConnection(conn)
	}
}

func (s *Server) handleConnection(netConn net.Conn) {
	conn := &connection{
		server: s,
		conn:   netConn,
		id:     newID(),
		closed: make(chan struct{}),
	}
	defer conn.close()

	message, err := readMessage(netConn)
	if err != nil {
		return
	}
	if message.Type != "register" || !s.validateRegister(message) {
		_ = conn.writeMessage(registerAck(false))
		return
	}

	conn.endpoint = registry.Endpoint{
		AgentID:      message.AgentID,
		ConnectionID: conn.id,
		Platform:     message.Platform,
		Tools:        message.Tools,
		ConnectedAt:  time.Now().UTC(),
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	previous := s.conns[message.AgentID]
	s.conns[message.AgentID] = conn
	s.mu.Unlock()
	if previous != nil {
		previous.close()
	}
	s.registry.Register(conn.endpoint)
	if err := conn.writeMessage(registerAck(true)); err != nil {
		return
	}

	for {
		message, err := readMessage(netConn)
		if err != nil {
			return
		}
		if message.ProtocolVersion != protocol.Version {
			return
		}
		switch message.Type {
		case "response", "query_response":
			s.deliver(message)
		default:
			return
		}
	}
}

func (s *Server) validateRegister(message Message) bool {
	if message.ProtocolVersion != protocol.Version || message.AgentID == "" {
		return false
	}
	if message.Platform != "darwin" && message.Platform != "windows" {
		return false
	}
	if len(message.Tools) == 0 {
		return false
	}
	for _, definition := range message.Tools {
		canonical, ok := protocol.ToolByName(definition.Name)
		if !ok || !platformIncluded(canonical.Platform, message.Platform) {
			return false
		}
	}
	return true
}

func (s *Server) deliver(message Message) {
	key := waiterKey(message.Type, message.RequestID)
	s.mu.Lock()
	entry := s.waiters[key]
	s.mu.Unlock()
	if entry == nil {
		return
	}
	select {
	case entry.channel <- message:
	default:
	}
}

func (c *connection) writeMessage(message Message) error {
	payload, err := json.Marshal(message)
	if err != nil {
		return err
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return WriteFrame(c.conn, payload)
}

func (c *connection) close() {
	c.closeOnce.Do(func() {
		close(c.closed)
		_ = c.conn.Close()
		c.server.removeConnection(c)
	})
}

func (s *Server) removeConnection(conn *connection) {
	s.registry.Unregister(conn.endpoint.AgentID, conn.id)
	s.mu.Lock()
	if s.conns[conn.endpoint.AgentID] == conn {
		delete(s.conns, conn.endpoint.AgentID)
	}
	affected := make([]*waiter, 0)
	for key, entry := range s.waiters {
		if entry.agentID == conn.endpoint.AgentID {
			affected = append(affected, entry)
			delete(s.waiters, key)
		}
	}
	s.mu.Unlock()

	for _, entry := range affected {
		select {
		case entry.channel <- Message{
			Type:            "response",
			ProtocolVersion: protocol.Version,
			Status:          protocol.StatusUnknown,
			Message:         "agent connection lost",
			Retryable:       false,
		}:
		default:
		}
	}
}

func readMessage(reader io.Reader) (Message, error) {
	payload, err := ReadFrame(reader, protocol.MaxFrameBytes)
	if err != nil {
		return Message{}, err
	}
	var message Message
	if err := json.Unmarshal(payload, &message); err != nil {
		return Message{}, err
	}
	if message.Type == "" {
		return Message{}, ErrProtocol
	}
	return message, nil
}

func registerAck(accepted bool) Message {
	value := accepted
	return Message{
		Type:            "register_ack",
		ProtocolVersion: protocol.Version,
		Accepted:        &value,
	}
}

func platformIncluded(platforms []string, platform string) bool {
	for _, value := range platforms {
		if value == platform {
			return true
		}
	}
	return false
}

func waiterKey(messageType, requestID string) string {
	return messageType + "\x00" + requestID
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
