package transport

import (
	"encoding/binary"
	"errors"
	"io"

	"arec/gateway/internal/protocol"
)

var (
	ErrZeroFrame     = errors.New("zero-length frame")
	ErrFrameTooLarge = errors.New("frame exceeds maximum size")
)

func ReadFrame(reader io.Reader, maxBytes uint32) ([]byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint32(header[:])
	if length == 0 {
		return nil, ErrZeroFrame
	}
	if maxBytes == 0 || length > maxBytes {
		return nil, ErrFrameTooLarge
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(reader, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

func EncodeFrame(payload []byte, maxBytes uint32) ([]byte, error) {
	if len(payload) == 0 {
		return nil, ErrZeroFrame
	}
	if uint64(len(payload)) > uint64(maxBytes) {
		return nil, ErrFrameTooLarge
	}
	frame := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(payload)))
	copy(frame[4:], payload)
	return frame, nil
}

func WriteFrame(writer io.Writer, payload []byte) error {
	frame, err := EncodeFrame(payload, protocol.MaxFrameBytes)
	if err != nil {
		return err
	}
	for len(frame) > 0 {
		written, err := writer.Write(frame)
		if err != nil {
			return err
		}
		if written == 0 {
			return io.ErrShortWrite
		}
		frame = frame[written:]
	}
	return nil
}
