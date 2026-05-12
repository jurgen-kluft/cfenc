package frameencoder

import "testing"

func TestBitStreamWriterReader_RoundTripMixedWidths(t *testing.T) {
	type entry struct {
		v int32
		n uint8
	}

	seq := []entry{
		{0b1, 1},
		{0b01, 2},
		{0b1011, 4},
		{0x5A, 8},
		{0xBEEF, 16},
		{0x3FFFFFFF, 30},
	}

	totalNumbits := 0
	for _, e := range seq {
		totalNumbits += int(e.n)
	}

	// BitStream needs to be initialized with a capacity that can hold all bits; otherwise it will panic on writes.

	// BitStream needs to be initialized with a capacity that can hold all bits; otherwise it will panic on writes.
	w := NewBitStreamWriter(int64(totalNumbits))
	for _, e := range seq {
		w.WriteBits(uint32(e.v), e.n)
	}
	bits, _ := w.Finalize()

	var expectedBits int64
	for _, e := range seq {
		expectedBits += int64(e.n)
	}
	if bits != expectedBits {
		t.Fatalf("Finalize bits mismatch: got %d want %d", bits, expectedBits)
	}

	r := w.Reader()
	for i, e := range seq {
		got := r.ReadBits(e.n)
		if got != e.v {
			t.Fatalf("read[%d] mismatch: got 0x%X want 0x%X (%d bits)", i, got, e.v, e.n)
		}
	}

	if !r.IsReadEnd(1) {
		t.Fatalf("expected read end after consuming all bits")
	}
}

func TestBitStreamWriterReader_PeekSkipAndReset(t *testing.T) {
	w := NewBitStreamWriter(64)
	w.WriteBits(0b110, 3)
	w.WriteBits(0b10, 2)
	w.WriteBits(0b1111, 4)
	w.Finalize()

	r := w.Reader()

	if got := r.PeekBits(3); got != 0b110 {
		t.Fatalf("peek mismatch: got %b want %b", got, 0b110)
	}
	if got := r.PeekBits(3); got != 0b110 {
		t.Fatalf("second peek should be identical: got %b want %b", got, 0b110)
	}

	r.SkipBits(3)
	if got := r.ReadBits(2); got != 0b10 {
		t.Fatalf("read after skip mismatch: got %b want %b", got, 0b10)
	}
	if got := r.ReadBits(4); got != 0b1111 {
		t.Fatalf("tail read mismatch: got %b want %b", got, 0b1111)
	}

	r.ResetRead()
	if got := r.ReadBits(3); got != 0b110 {
		t.Fatalf("reset should rewind stream: got %b want %b", got, 0b110)
	}
}

func TestBitStreamWriterReader_ReadBounds(t *testing.T) {
	w := NewBitStreamWriter(16)
	w.WriteBits(0b101010, 6)
	w.Finalize()

	r := w.Reader()

	if got := r.ReadBits(0); got != -1 {
		t.Fatalf("ReadBits(0) should return -1, got %d", got)
	}
	if got := r.ReadBits(7); got != -1 {
		t.Fatalf("out-of-bounds read should return -1, got %d", got)
	}

	if got := r.ReadBits(6); got != 0b101010 {
		t.Fatalf("valid read mismatch: got %b want %b", got, 0b101010)
	}
	if got := r.ReadBits(1); got != -1 {
		t.Fatalf("read beyond end should return -1, got %d", got)
	}
}

func TestBitStreamWriter_FinalizeLocksWriter(t *testing.T) {
	w := NewBitStreamWriter(16)
	w.WriteBits(0b11, 2)
	before, _ := w.Finalize()

	tail := w.buf[0]
	w.WriteBits(0xFF, 8)
	after, _ := w.Finalize()

	if after != before {
		t.Fatalf("Finalize should be stable after lock: got %d want %d", after, before)
	}
	if w.buf[0] != tail {
		t.Fatalf("buffer changed after writes to finalized writer")
	}
}

func TestBitStreamWriter_32BitBoundaryRoundTrip(t *testing.T) {
	w := NewBitStreamWriter(128)

	w.WriteBits((1<<31)-1, 31)
	w.WriteBits(^uint32(0), 32)
	w.WriteBits(1, 1)
	w.Finalize()

	r := w.Reader()

	if got := r.ReadBits(31); got != (1<<31)-1 {
		t.Fatalf("first chunk mismatch: got 0x%X", got)
	}
	if got := r.ReadBits(32); got != ^int32(0) {
		t.Fatalf("second chunk mismatch: got 0x%X", got)
	}
	if got := r.ReadBits(1); got != 1 {
		t.Fatalf("boundary bit mismatch: got %d", got)
	}
}

func TestBitStreamWriter_SetCapacityGrowthSafe(t *testing.T) {
	w := NewBitStreamWriter(8)
	w.WriteBits(0xAB, 8)

	// Grow after initial write; this should allow subsequent writes without panic.
	w.SetCapacity(256)

	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("WriteBits panicked after SetCapacity growth: %v", r)
		}
	}()

	for i := 0; i < 8; i++ {
		w.WriteBits(uint32(i), 4)
	}
	w.Finalize()
}

// TestBitStreamWriter_DeterministicStress writes 500 entries whose widths
// sweep 1..32 bits in round-robin order and whose values are produced by a
// simple LCG.  After Finalize it verifies:
//   - the returned bit count equals the sum of all widths
//   - every value round-trips exactly through the reader
//   - IsReadEnd triggers precisely at the end
//   - a second ResetRead + full re-read produces identical results
func TestBitStreamWriter_DeterministicStress(t *testing.T) {
	const N = 500

	type entry struct {
		v int32
		n uint8 // 1..32
	}

	// LCG parameters (Knuth Vol.2)
	const (
		lcgA = 1664525
		lcgC = 1013904223
	)
	lcg := uint32(0xDEADBEEF)
	next := func() uint32 {
		lcg = lcgA*lcg + lcgC
		return lcg
	}

	entries := make([]entry, N)
	totalBits := int64(0)
	for i := range entries {
		n := uint8((i % 32) + 1) // widths 1,2,...,32,1,2,...
		raw := next()
		mask := uint32((uint64(1) << n) - 1)
		entries[i] = entry{v: int32(raw & mask), n: n}
		totalBits += int64(n)
	}

	w := NewBitStreamWriter(totalBits)
	for _, e := range entries {
		w.WriteBits(uint32(e.v), e.n)
	}
	got, _ := w.Finalize()
	if got != totalBits {
		t.Fatalf("Finalize: got %d bits, want %d", got, totalBits)
	}

	// --- first read-through ---
	verify := func(pass string) {
		t.Helper()
		r := w.Reader()
		for i, e := range entries {
			if r.IsReadEnd(e.n) {
				t.Fatalf("%s: IsReadEnd too early at entry %d", pass, i)
			}
			got := r.ReadBits(e.n)
			if got != e.v {
				t.Fatalf("%s: entry[%d] width=%d: got 0x%08X want 0x%08X", pass, i, e.n, got, e.v)
			}
		}
		if !r.IsReadEnd(1) {
			t.Fatalf("%s: expected IsReadEnd after consuming all bits", pass)
		}
	}

	verify("first pass")

	// --- verify ResetRead reproduces the same sequence ---
	r := w.Reader()
	// consume a few entries, then reset and re-read from the start
	for i := 0; i < 10; i++ {
		r.ReadBits(entries[i].n)
	}
	r.ResetRead()
	verify("after ResetRead")
}
