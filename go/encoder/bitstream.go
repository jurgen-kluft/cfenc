package frameencoder

import "slices"

type BitStreamWriter struct {
	buf          []uint8 // buffer for bits being written
	numBits      int64   // total number of bits written
	pos          int64   // write byte position
	finalized    bool    // whether Finalize has been called
	accuNumBits  int32   // number of bits currently in the accumulator
	accuRegister uint64  // accumulator for bits being written
}

func NewBitStreamWriter(sizeInBits int64) *BitStreamWriter {
	bs := &BitStreamWriter{}
	bs.SetCapacity(sizeInBits)
	return bs
}

func (bs *BitStreamWriter) SetCapacity(sizeInBits int64) {
	if len(bs.buf) == 0 {
		bs.buf = make([]uint8, (sizeInBits+7)>>3)
	} else {
		needed := (sizeInBits + 7) >> 3
		bs.buf = slices.Grow(bs.buf, int(needed))
		// After grow, length of bs.buf may be less than needed, so we need to reslice to the new length.
		bs.buf = bs.buf[:needed]
	}
}

func (bs *BitStreamWriter) TestBit(bitPos int32) bool {
	bytePos := bitPos >> 3
	bitOffset := uint8(bitPos & 7)
	if bytePos >= int32(len(bs.buf)) {
		return false
	}
	return (bs.buf[bytePos] & (1 << bitOffset)) != 0
}

func (bs *BitStreamWriter) WriteBits(v uint32, n uint8) {
	if n == 0 || bs.finalized {
		return
	}
	// We are accumulating bits in the accuRegister until we have more than 32 bits,
	// at which point we flush to the buffer.
	// Note: This means that we cannot write more than 32 bits at a time!
	bs.accuRegister |= uint64(v) << bs.accuNumBits
	bs.accuNumBits += int32(n)
	if bs.accuNumBits >= 32 {
		bs.buf[bs.pos] = uint8(bs.accuRegister & 0xFF)
		bs.buf[bs.pos+1] = uint8((bs.accuRegister >> 8) & 0xFF)
		bs.buf[bs.pos+2] = uint8((bs.accuRegister >> 16) & 0xFF)
		bs.buf[bs.pos+3] = uint8((bs.accuRegister >> 24) & 0xFF)
		bs.pos += 4
		bs.accuRegister >>= 32
		bs.accuNumBits -= 32
	}
	bs.numBits += int64(n)
}

func (bs *BitStreamWriter) Finalize() (bitsWritten int64, bytesStored int64) {

	// Flush remaining bits in the accumulator to the buffer
	for bs.accuNumBits > 0 {
		bs.buf[bs.pos] = uint8(bs.accuRegister & 0xFF)
		bs.accuRegister >>= 8
		bs.accuNumBits -= 8
		bs.pos++
	}

	bs.accuNumBits = 0
	bs.accuRegister = 0
	bs.finalized = true

	return bs.numBits, bs.pos
}

func (bs *BitStreamWriter) Reader() *BitStreamReader {
	return NewBitStreamReader(bs.buf, int32(bs.numBits))
}

// -------------------------------------------------------------
// BitStreamReader
// ------------------------------------------------------------

type BitStreamReader struct {
	buf          []uint8
	numBits      int32
	readBits     int32
	pos          int32  // byte position in buf
	accuNumBits  int16  // number of bits currently in the accumulator
	accuRegister uint64 // accumulator for bits being read
}

func NewBitStreamReader(buf []uint8, numBits int32) *BitStreamReader {
	return &BitStreamReader{
		buf:     buf,
		numBits: numBits,
	}
}

func (bs *BitStreamReader) ResetRead() {
	bs.readBits = 0
	bs.pos = 0
	bs.accuNumBits = 0
	bs.accuRegister = 0
}

func (bs *BitStreamReader) CurrentBytePos() int32 {
	return bs.pos
}

func (bs *BitStreamReader) checkAccumulator(n uint8) {
	// Ensure we have enough bits in the accumulator to satisfy the current read.
	for bs.accuNumBits < int16(n) && bs.pos < int32(len(bs.buf)) {
		bs.accuRegister |= uint64(bs.buf[bs.pos]) << bs.accuNumBits
		bs.accuNumBits += 8
		bs.pos++
	}
}

func (bs *BitStreamReader) ReadBits(n uint8) int32 {
	if n == 0 || (bs.readBits+int32(n)) > bs.numBits {
		return -1
	}

	bs.checkAccumulator(n)
	v := uint32(bs.accuRegister & ((uint64(1) << n) - 1))
	bs.accuRegister >>= n
	bs.accuNumBits -= int16(n)

	bs.readBits += int32(n)
	return int32(v)
}

func (bs *BitStreamReader) PeekBits(n uint8) int32 {
	if n == 0 || (bs.readBits+int32(n)) > bs.numBits {
		return -1
	}
	bs.checkAccumulator(n)
	return int32(bs.accuRegister & ((uint64(1) << n) - 1))
}

func (bs *BitStreamReader) SkipBits(n uint8) {
	if n == 0 || (bs.readBits+int32(n)) > bs.numBits {
		return
	}

	bs.checkAccumulator(n)
	bs.accuRegister >>= n
	bs.accuNumBits -= int16(n)
	bs.readBits += int32(n)
}

func (bs *BitStreamReader) IsReadEnd(sizeofSymbolInBits uint8) bool {
	return bs.readBits >= bs.numBits || (bs.numBits-bs.readBits) < int32(sizeofSymbolInBits)
}
