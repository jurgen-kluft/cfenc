package frameencoder

import "fmt"

// ============================================================
// SRLEN core
// ============================================================

type runHist struct {
	sizeInBitsPerRb  [6]int // resulting compression size for each rb (0..5)
	symbolSizeInBits int    // bits per symbol
}

func newRunHist(symSizeInBits uint8) runHist {
	h := runHist{}
	h.symbolSizeInBits = int(symSizeInBits)
	return h
}

// recordRun updates the run histogram for a given run length.
// It calculates the contribution of the run to the sizeInBits for each possible run bit length (rb).
func (h *runHist) recordRun(run int) {
	for i := range h.sizeInBitsPerRb {
		r := run
		if i == 0 {
			h.sizeInBitsPerRb[i] += r * h.symbolSizeInBits
		} else {
			b := 1 << i
			e := r / b
			r -= e * b
			if r > 0 {
				e++
			}
			h.sizeInBitsPerRb[i] += e * (h.symbolSizeInBits + i)
		}
	}
}

// Choose best rb in {0..5}
// Which rb gives the smallest total size in bits?
// Returns the index of the best rb.
func (h *runHist) bestRb() uint8 {
	best := uint8(0)
	sizeInBits := h.sizeInBitsPerRb[0]
	for i := 1; i < len(h.sizeInBitsPerRb); i++ {
		s := h.sizeInBitsPerRb[i]
		if s < sizeInBits {
			sizeInBits = s
			best = uint8(i)
		}
	}
	return best
}

// ============================================================
// Encoder
// ============================================================

// Encode takes a stream of symbols and encodes it using the SRLEN algorithm.
// User needs to provide the symbol size in bits and the alphabet size (number of distinct symbols), as
// well as an output BitStreamWriter with enough capacity to hold the encoded data.
func EncodePrepare(data *BitStreamReader, sizeofSymbolInBits uint8) (rb []uint8, err error) {
	numberOfSymbols := int32(1 << sizeofSymbolInBits)
	hists := make([]runHist, numberOfSymbols)
	for i := range numberOfSymbols {
		hists[i] = newRunHist(sizeofSymbolInBits)
	}

	if (1 << sizeofSymbolInBits) < numberOfSymbols {
		return nil, fmt.Errorf("number of symbols %d exceeds the maximum representable with %d bits", numberOfSymbols, sizeofSymbolInBits)
	}

	// Analysis pass
	data.ResetRead()
	for data.IsReadEnd(sizeofSymbolInBits) == false {
		s := data.ReadBits(sizeofSymbolInBits)
		if s == -1 {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}
		run := 1
		for data.IsReadEnd(sizeofSymbolInBits) == false && data.PeekBits(sizeofSymbolInBits) == s {
			data.SkipBits(sizeofSymbolInBits)
			run++
		}
		// Assert when symbol is out of range
		if s >= numberOfSymbols {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}
		hists[s].recordRun(run)
	}

	// Determine the best run bit length (rb) for each symbol based on the recorded run lengths
	// and their contributions to the total size.
	rb = make([]uint8, numberOfSymbols)
	for s := range numberOfSymbols {
		rb[s] = hists[s].bestRb()
	}

	return rb, nil
}

func Encode(data *BitStreamReader, sizeofSymbolInBits uint8, givenRb []uint8, out *BitStreamWriter) (rb []uint8, err error) {
	if givenRb == nil {
		var err error
		givenRb, err = EncodePrepare(data, sizeofSymbolInBits)
		if err != nil {
			return nil, err
		}
	}

	if len(givenRb) != (1 << sizeofSymbolInBits) {
		return nil, fmt.Errorf("givenRb length %d does not match expected number of symbols %d", len(givenRb), 1<<sizeofSymbolInBits)
	}

	data.ResetRead()
	for data.IsReadEnd(sizeofSymbolInBits) == false {
		s := data.ReadBits(sizeofSymbolInBits)
		run := 1
		for data.IsReadEnd(sizeofSymbolInBits) == false && data.PeekBits(sizeofSymbolInBits) == s {
			data.SkipBits(sizeofSymbolInBits)
			run++
		}

		if s == -1 || s >= int32(len(givenRb)) {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}

		rb := givenRb[s]
		if rb == 0 {
			for k := 0; k < run; k++ {
				out.WriteBits(uint32(s), sizeofSymbolInBits)
			}
		} else {
			maxChunk := (1 << rb)
			remain := run
			for remain > 0 {
				chunk := min(remain, maxChunk)
				out.WriteBits(uint32(s), sizeofSymbolInBits)
				out.WriteBits(uint32(chunk-1), rb)
				remain -= chunk
			}
		}
	}

	return givenRb, nil
}

// ============================================================
// Encoder for 4 bit symbols
// ============================================================

// Encode takes a stream of symbols and encodes it using the SRLEN algorithm.
// User needs to provide the symbol size in bits and the alphabet size (number of distinct symbols), as
// well as an output BitStreamWriter with enough capacity to hold the encoded data.
func Encode4Prepare(data []uint8) (rb []uint8, err error) {
	sizeofSymbolInBits := uint8(4)
	numberOfSymbols := int32(16) // for 4 bit symbols, we have 16 possible symbols (0..15)

	hists := make([]runHist, numberOfSymbols)
	for i := range numberOfSymbols {
		hists[i] = newRunHist(sizeofSymbolInBits)
	}

	if (1 << sizeofSymbolInBits) < numberOfSymbols {
		return nil, fmt.Errorf("number of symbols %d exceeds the maximum representable with %d bits", numberOfSymbols, sizeofSymbolInBits)
	}

	// Analysis pass, read nibble by nibble
	reader := NewBitStreamReader(data, 4)
	reader.ResetRead()
	for reader.IsReadEnd(sizeofSymbolInBits) == false {
		s := reader.ReadBits(sizeofSymbolInBits)
		if s == -1 {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}
		run := 1
		for reader.IsReadEnd(sizeofSymbolInBits) == false && reader.PeekBits(sizeofSymbolInBits) == s {
			reader.SkipBits(sizeofSymbolInBits)
			run++
		}
		// Assert when symbol is out of range
		if s >= numberOfSymbols {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}
		hists[s].recordRun(run)
	}

	// Determine the best run bit length (rb) for each symbol based on the recorded run lengths
	// and their contributions to the total size.
	rb = make([]uint8, numberOfSymbols)
	for s := range numberOfSymbols {
		rb[s] = hists[s].bestRb()
	}

	return rb, nil
}

func Encode4(data []uint8, givenRb []uint8, out *BitStreamWriter) (rb []uint8, err error) {
	if givenRb == nil {
		var err error
		givenRb, err = Encode4Prepare(data)
		if err != nil {
			return nil, err
		}
	}

	sizeofSymbolInBits := uint8(4)

	if len(givenRb) != (1 << sizeofSymbolInBits) {
		return nil, fmt.Errorf("givenRb length %d does not match expected number of symbols %d", len(givenRb), 1<<sizeofSymbolInBits)
	}

	reader := NewBitStreamReader(data, 4)
	reader.ResetRead()
	for reader.IsReadEnd(sizeofSymbolInBits) == false {
		s := reader.ReadBits(sizeofSymbolInBits)
		run := 1
		for reader.IsReadEnd(sizeofSymbolInBits) == false && reader.PeekBits(sizeofSymbolInBits) == s {
			reader.SkipBits(sizeofSymbolInBits)
			run++
		}

		if s == -1 || s >= int32(len(givenRb)) {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}

		rb := givenRb[s]
		if rb == 0 {
			for k := 0; k < run; k++ {
				out.WriteBits(uint32(s), sizeofSymbolInBits)
			}
		} else {
			maxChunk := (1 << rb)
			remain := run
			for remain > 0 {
				chunk := min(remain, maxChunk)
				out.WriteBits(uint32(s), sizeofSymbolInBits)
				out.WriteBits(uint32(chunk-1), rb)
				remain -= chunk
			}
		}
	}

	return givenRb, nil
}

// ============================================================
// Encoder for 8 bit symbols
// ============================================================

// Encode takes a stream of symbols and encodes it using the SRLEN algorithm.
// User needs to provide the symbol size in bits and the alphabet size (number of distinct symbols), as
// well as an output BitStreamWriter with enough capacity to hold the encoded data.
func Encode8Prepare(data []uint8) (rb []uint8, err error) {
	sizeofSymbolInBits := uint8(8)
	numberOfSymbols := int32(256) // for 8 bit symbols, we have 256 possible symbols (0..255)

	hists := make([]runHist, numberOfSymbols)
	for i := range numberOfSymbols {
		hists[i] = newRunHist(sizeofSymbolInBits)
	}

	if (1 << sizeofSymbolInBits) < numberOfSymbols {
		return nil, fmt.Errorf("number of symbols %d exceeds the maximum representable with %d bits", numberOfSymbols, sizeofSymbolInBits)
	}

	// Analysis pass, read nibble by nibble
	reader := NewBitStreamReader(data, 4)
	reader.ResetRead()
	for reader.IsReadEnd(sizeofSymbolInBits) == false {
		s := reader.ReadBits(sizeofSymbolInBits)
		if s == -1 {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}
		run := 1
		for reader.IsReadEnd(sizeofSymbolInBits) == false && reader.PeekBits(sizeofSymbolInBits) == s {
			reader.SkipBits(sizeofSymbolInBits)
			run++
		}
		// Assert when symbol is out of range
		if s >= numberOfSymbols {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}
		hists[s].recordRun(run)
	}

	// Determine the best run bit length (rb) for each symbol based on the recorded run lengths
	// and their contributions to the total size.
	rb = make([]uint8, numberOfSymbols)
	for s := range numberOfSymbols {
		rb[s] = hists[s].bestRb()
	}

	return rb, nil
}

func Encode8(data []uint8, givenRb []uint8, out *BitStreamWriter) (rb []uint8, err error) {
	if givenRb == nil {
		var err error
		givenRb, err = Encode8Prepare(data)
		if err != nil {
			return nil, err
		}
	}

	sizeofSymbolInBits := uint8(8)

	if len(givenRb) != (1 << sizeofSymbolInBits) {
		return nil, fmt.Errorf("givenRb length %d does not match expected number of symbols %d", len(givenRb), 1<<sizeofSymbolInBits)
	}

	reader := NewBitStreamReader(data, 8)
	reader.ResetRead()
	for reader.IsReadEnd(sizeofSymbolInBits) == false {
		s := reader.ReadBits(sizeofSymbolInBits)
		run := 1
		for reader.IsReadEnd(sizeofSymbolInBits) == false && reader.PeekBits(sizeofSymbolInBits) == s {
			reader.SkipBits(sizeofSymbolInBits)
			run++
		}

		if s == -1 || s >= int32(len(givenRb)) {
			return nil, fmt.Errorf("invalid symbol %d", s)
		}

		rb := givenRb[s]
		if rb == 0 {
			for k := 0; k < run; k++ {
				out.WriteBits(uint32(s), sizeofSymbolInBits)
			}
		} else {
			maxChunk := (1 << rb)
			remain := run
			for remain > 0 {
				chunk := min(remain, maxChunk)
				out.WriteBits(uint32(s), sizeofSymbolInBits)
				out.WriteBits(uint32(chunk-1), rb)
				remain -= chunk
			}
		}
	}

	return givenRb, nil
}

// ============================================================
// Decoder
// ============================================================
type SrlenBitStreamReader struct {
	bs               *BitStreamReader
	symbolSizeInBits uint8
	rb               []uint8
	symbol           int32
	run              int32
}

func NewSrlenBitStreamReader(bs *BitStreamReader, symbolSizeInBits uint8, rb []uint8) *SrlenBitStreamReader {
	return &SrlenBitStreamReader{
		bs:               bs,
		symbolSizeInBits: symbolSizeInBits,
		rb:               rb,
	}
}

func (s *SrlenBitStreamReader) BytePos() int32 {
	return s.bs.CurrentBytePos()
}

func (s *SrlenBitStreamReader) ReadSymbol() (int32, error) {
	if s.run > 0 {
		s.run--
		return s.symbol, nil
	}

	if s.bs.IsReadEnd(s.symbolSizeInBits) {
		return -1, fmt.Errorf("end of stream")
	}
	sym := s.bs.ReadBits(s.symbolSizeInBits)
	if sym == -1 || sym >= int32(len(s.rb)) {
		return -1, fmt.Errorf("invalid symbol %d", sym)
	}
	s.symbol = sym
	if s.rb[sym] > 0 {
		s.run = s.bs.ReadBits(s.rb[sym])
	}
	return sym, nil
}

// Decode takes an encoded bit stream and decodes it using the SRLEN algorithm.
// User needs to provide the symbol size in bits, the run bit lengths (rb) for each symbol,
// and an output BitStreamWriter with enough capacity to hold the decoded data.
func Decode(bs *BitStreamReader, sizeofSymbolInBits uint8, rb []uint8, out *BitStreamWriter) error {
	bs.ResetRead()

	for bs.IsReadEnd(sizeofSymbolInBits) == false {
		s := bs.ReadBits(sizeofSymbolInBits)
		if s == -1 || s >= int32(len(rb)) {
			return fmt.Errorf("invalid symbol %d", s)
		} else if rb[s] == 0 {
			out.WriteBits(uint32(s), sizeofSymbolInBits)
		} else {
			run := bs.ReadBits(rb[s])
			if run == -1 {
				return fmt.Errorf("invalid run length for symbol %d", s)
			}

			run++ // runs are encoded as (run-1) to allow representing runs of length 1..(2^rb)

			for range run {
				out.WriteBits(uint32(s), sizeofSymbolInBits)
			}
		}
	}
	return nil
}
