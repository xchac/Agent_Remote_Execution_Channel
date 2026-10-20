package transport

import (
	"bytes"
	"errors"
	"io"
	"testing"

	"arec/gateway/internal/protocol"
)

type oneByteReader struct {
	reader io.Reader
}

func (r oneByteReader) Read(buffer []byte) (int, error) {
	if len(buffer) > 1 {
		buffer = buffer[:1]
	}
	return r.reader.Read(buffer)
}

func TestReadMultipleFramesAndSegmentedFrame(t *testing.T) {
	first, err := EncodeFrame([]byte(`{"type":"one"}`), protocol.MaxFrameBytes)
	if err != nil {
		t.Fatal(err)
	}
	second, err := EncodeFrame([]byte(`{"type":"two"}`), protocol.MaxFrameBytes)
	if err != nil {
		t.Fatal(err)
	}
	stream := oneByteReader{reader: bytes.NewReader(append(first, second...))}

	payload, err := ReadFrame(stream, protocol.MaxFrameBytes)
	if err != nil || string(payload) != `{"type":"one"}` {
		t.Fatalf("first frame: %q, %v", payload, err)
	}
	payload, err = ReadFrame(stream, protocol.MaxFrameBytes)
	if err != nil || string(payload) != `{"type":"two"}` {
		t.Fatalf("second frame: %q, %v", payload, err)
	}
}

func TestInvalidFrameLengths(t *testing.T) {
	if _, err := ReadFrame(bytes.NewReader([]byte{0, 0, 0, 0}), 1024); !errors.Is(err, ErrZeroFrame) {
		t.Fatalf("expected zero-frame error, got %v", err)
	}
	if _, err := ReadFrame(bytes.NewReader([]byte{0, 0, 1, 1}), 16); !errors.Is(err, ErrFrameTooLarge) {
		t.Fatalf("expected oversized-frame error, got %v", err)
	}
	if _, err := EncodeFrame(nil, 16); !errors.Is(err, ErrZeroFrame) {
		t.Fatalf("expected encode zero-frame error, got %v", err)
	}
}
