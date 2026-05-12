package frameencoder

import (
	"strings"
	"testing"
)

// generateRunLengthBiasedData produces n symbols from an alphabet of the given
// size using a simple LCG seeded deterministically.  The bias parameter (0..1)
// controls how likely consecutive symbols are to repeat, which creates the long
// runs that SRLEN is designed to compress.
func generateRunLengthBiasedData(n int, alphabetSize uint32, seed uint32, runBias float32) []uint32 {
	lcg := seed
	next := func() uint32 {
		lcg = 1664525*lcg + 1013904223
		return lcg
	}

	out := make([]uint32, n)
	cur := next() % alphabetSize
	for i := range out {
		// advance symbol when a fresh random draw exceeds the bias threshold
		if float32(next()>>1)/float32(1<<31) > runBias {
			cur = next() % alphabetSize
		}
		out[i] = cur
	}
	return out
}

func TestEncodeDecodeRoundTrip(t *testing.T) {
	const symbolBits = uint8(2)
	const numberOfSymbols = int32(4)

	inputSymbols := []uint32{1, 1, 1, 2, 2, 3, 3, 3, 3, 0}

	input := NewBitStreamWriter(int64(len(inputSymbols) * int(symbolBits)))
	for _, s := range inputSymbols {
		input.WriteBits(s, symbolBits)
	}
	input.Finalize()

	encoded := NewBitStreamWriter(int64(len(inputSymbols) * int(symbolBits) * 2))
	rb, err := Encode(input.Reader(), symbolBits, nil, encoded)
	if err != nil {
		t.Fatalf("Encode failed: %v", err)
	}
	encoded.Finalize()

	decoded := NewBitStreamWriter(int64(len(inputSymbols) * int(symbolBits)))
	err = Decode(encoded.Reader(), symbolBits, rb, decoded)
	if err != nil {
		t.Fatalf("Decode failed: %v", err)
	}
	decoded.Finalize()

	r := decoded.Reader()
	for i, want := range inputSymbols {
		got := r.ReadBits(symbolBits)
		if got == -1 {
			t.Fatalf("decoded stream ended early at symbol %d", i)
		}
		if uint32(got) != want {
			t.Fatalf("decoded symbol[%d] mismatch: got %d want %d", i, got, want)
		}
	}

	if !r.IsReadEnd(1) {
		t.Fatalf("decoded stream has unexpected trailing bits")
	}
}

func TestDecodeReturnsErrorOnTruncatedRunLength(t *testing.T) {
	const symbolBits = uint8(2)

	// Encoded stream contains only the symbol "1" and omits its run-length bits (rb=2 requires 2 more bits).
	encoded := NewBitStreamWriter(int64(symbolBits))
	encoded.WriteBits(1, symbolBits)
	encoded.Finalize()

	rb := []uint8{0, 2, 0, 0}
	decoded := NewBitStreamWriter(16)
	err := Decode(encoded.Reader(), symbolBits, rb, decoded)
	if err == nil {
		t.Fatalf("expected decode error for truncated run length")
	}
	if !strings.Contains(err.Error(), "invalid run length") {
		t.Fatalf("unexpected decode error: %v", err)
	}
}

// TestEncodeCompressesAndRoundTrips verifies that Encode produces a bit stream
// that is smaller than the unencoded input for highly repetitive data, and that
// Decode reconstructs the original sequence exactly.
//
// The test uses 8-bit symbols (256-symbol alphabet) and generates 4 000
// symbols with a heavy run-length bias so that SRLEN should compress well.
func TestEncodeCompressesAndRoundTrips(t *testing.T) {
	const (
		symbolBits   = uint8(8)
		alphabetSize = uint32(1) << symbolBits
		numSymbols   = 4000
		numberOfSyms = int32(alphabetSize)
	)

	symbols := generateRunLengthBiasedData(numSymbols, alphabetSize, 0xDEADBEEF, 0.92)

	// Build unencoded bit stream.
	rawBits := numSymbols * int(symbolBits)
	input := NewBitStreamWriter(int64(rawBits))
	for _, s := range symbols {
		input.WriteBits(s, symbolBits)
	}
	input.Finalize()

	// Encode – allocate a generous output buffer; we'll measure actual bits used.
	encoded := NewBitStreamWriter(int64(rawBits * 2))
	rb, err := Encode(input.Reader(), symbolBits, nil, encoded)
	if err != nil {
		t.Fatalf("Encode failed: %v", err)
	}
	encodedBits, _ := encoded.Finalize()

	ratio := float64(encodedBits) / float64(rawBits)
	t.Logf("raw=%d bits  encoded=%d bits  ratio=%.3f", rawBits, encodedBits, ratio)

	// With a geometric run distribution (mean ≈ 12.5, bias=0.92) and an 8-bit
	// alphabet, SRLEN should encode each run in ~13 bits (symbol + rb=5 field),
	// yielding ~320 runs × 13 bits ≈ 4160 bits out of 32000 raw bits → ratio ≈ 0.13.
	// We allow up to 0.20 as a regression guard: any serious breakage in
	// recordRun or bestRb would push the ratio well above this threshold.
	const maxAllowedRatio = 0.20
	if ratio > maxAllowedRatio {
		t.Errorf("compression ratio %.3f is worse than expected threshold %.2f — possible regression in rb selection", ratio, maxAllowedRatio)
	}

	// Decode and verify round-trip fidelity.
	decoded := NewBitStreamWriter(int64(rawBits))
	if err := Decode(encoded.Reader(), symbolBits, rb, decoded); err != nil {
		t.Fatalf("Decode failed: %v", err)
	}
	decoded.Finalize()

	r := decoded.Reader()
	for i, want := range symbols {
		got := r.ReadBits(symbolBits)
		if got == -1 {
			t.Fatalf("decoded stream ended early at symbol %d", i)
		}
		if uint32(got) != want {
			t.Fatalf("decoded symbol[%d]: got %d want %d", i, got, want)
		}
	}
	if !r.IsReadEnd(1) {
		t.Fatalf("decoded stream has unexpected trailing bits")
	}
}
