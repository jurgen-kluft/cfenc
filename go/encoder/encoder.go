package frameencoder

import (
	"fmt"
	"math/bits"
	"slices"
	"time"
)

const (
	SELECTOR_P2  = 0
	SELECTOR_P4  = 1
	SELECTOR_RAW = 2
	SELECTOR_P8  = 3
)

func rgb565(r, g, b uint32) uint16 {
	return uint16(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
}

func rgba8888_to_rgb565(c uint32) uint16 {
	r := uint32((c >> 16) & 0xFF)
	g := uint32((c >> 8) & 0xFF)
	b := uint32(c & 0xFF)
	return rgb565(r, g, b)
}

type histocolor struct {
	color      uint16 // the RGB565 color value
	colorCount int32  // occurrence count of the color in an image
}

// ------------------------------------------------------------------
// Pixel count for each pixel stream
// ------------------------------------------------------------------
func CalculatePixelCounts(hist []histocolor) (p2NumPixels, p4NumPixels, p8NumPixels, p16NumPixels int64) {
	p2NumPixels = int64(0)
	p4NumPixels = int64(0)
	p8NumPixels = int64(0)
	p16NumPixels = int64(0)
	for i, hc := range hist {
		if i < 4 {
			p2NumPixels += int64(hc.colorCount)
		} else if i < 20 {
			p4NumPixels += int64(hc.colorCount)
		} else if i < 276 {
			p8NumPixels += int64(hc.colorCount)
		} else {
			p16NumPixels += int64(hc.colorCount)
		}
	}
	return p2NumPixels, p4NumPixels, p8NumPixels, p16NumPixels
}

// ------------------------------------------------------------------
// Correct histogram color index mapping
// ------------------------------------------------------------------
func MakeColorIndex(hist []histocolor) []int32 {
	// The histogram is sorted, so color lookup will be incorrect.
	colorMapping := make([]int32, 65536)
	for i := range hist {
		c := hist[i].color
		colorMapping[c] = int32(i)
	}
	return colorMapping
}

func BuildHistogram(pixels []uint16, w, h int) (histogram []histocolor) {

	// Note: avoid the use of map for histogram to ensure deterministic palette generation
	// across different runs and platforms. The histogram is implemented as a fixed-size array
	// indexed by RGB565 color values, which guarantees consistent ordering of colors based on
	// their occurrence counts.
	histogram = make([]histocolor, 65536) // RGB565 histogram
	for i := range histogram {
		histogram[i].color = uint16(i)
		histogram[i].colorCount = 0
	}

	for y := range h {
		for x := range w {
			v := pixels[y*w+x]
			histogram[v].colorCount++
		}
	}

	slices.SortFunc(histogram, func(a, b histocolor) int {
		if a.colorCount < b.colorCount {
			return 1
		} else if a.colorCount > b.colorCount {
			return -1
		} else {
			return 0
		}
	})

	return histogram
}

func PrintImageInfo(histogram []histocolor, w int, h int) {
	// Print image info:
	// - image name
	// - dimensions
	// - total unique colors
	// - pixel count of P0, P1, P2, and raw pixels

	colorCount := 0
	p2Count := int32(0)
	p4Count := int32(0)
	p8Count := int32(0)
	p16Count := int32(0)

	for i, hc := range histogram {
		if hc.colorCount > 0 {
			colorCount++
			if i < 4 {
				p2Count += hc.colorCount
			} else if i < 20 {
				p4Count += hc.colorCount
			} else if i < 276 {
				p8Count += hc.colorCount
			} else {
				p16Count += hc.colorCount
			}
		}
	}

	fmt.Printf("Dimensions: %dx%d\n", w, h)
	fmt.Printf("Raw Size: %d bytes\n", w*h*2) // RGB565 format uses 2 bytes per pixel
	fmt.Printf("Unique color count: %d\n", colorCount)
	fmt.Printf("P2 pixel count: %d\n", p2Count)
	fmt.Printf("P4 pixel count: %d\n", p4Count)
	fmt.Printf("P8 pixel count: %d\n", p8Count)
	fmt.Printf("P16 pixel count: %d\n", p16Count)
}

type LineInfo struct {
	Active                bool
	LineIndex             uint16
	SpanStream            *BitStreamWriter
	SelectorStream        *BitStreamWriter
	P2Stream              *BitStreamWriter
	P4Stream              *BitStreamWriter
	P8Stream              *BitStreamWriter
	P16Stream             []uint16
	SpanStreamEncoded     *BitStreamWriter
	SelectorStreamEncoded *BitStreamWriter
	P2StreamEncoded       *BitStreamWriter
	P4StreamEncoded       *BitStreamWriter
	P8StreamEncoded       *BitStreamWriter
}

func (l *LineInfo) Initialize(y uint16, w int) {
	l.Active = true
	l.LineIndex = uint16(y)
	l.SpanStream = NewBitStreamWriter(int64(w))
	l.SelectorStream = NewBitStreamWriter(int64(w * 2))
	l.P2Stream = NewBitStreamWriter(int64(w * 2))
	l.P4Stream = NewBitStreamWriter(int64(w * 4))
	l.P8Stream = NewBitStreamWriter(int64(w * 8))
	l.P16Stream = make([]uint16, 0, w)
	l.SpanStreamEncoded = NewBitStreamWriter(int64(w))
	l.SelectorStreamEncoded = NewBitStreamWriter(int64(w * (2 + 5)))
	l.P2StreamEncoded = NewBitStreamWriter(int64(w * (2 + 5)))
	l.P4StreamEncoded = NewBitStreamWriter(int64(w * (4 + 5)))
	l.P8StreamEncoded = NewBitStreamWriter(int64(w * (8 + 5)))
}

func EncodeFrame(imageWidth, imageHeight, tileWidth, tileHeight int, curImage []uint16, prevImage []uint16) {

	startTime := time.Now()

	w := imageWidth
	h := imageHeight
	pixelCount := w * h

	hist := BuildHistogram(curImage, imageWidth, imageHeight)
	p2NumPixels, p4NumPixels, p8NumPixels, p16NumPixels := CalculatePixelCounts(hist)
	colorMapping := MakeColorIndex(hist)

	tileWidthShift := bits.TrailingZeros(uint(tileWidth))
	numTilesX := (w + tileWidth - 1) / tileWidth
	numTilesY := (h + tileHeight - 1) / tileHeight
	numTilesTotal := numTilesX * numTilesY

	// ------------------------------------------------------------------
	// Encoding preparation
	// ------------------------------------------------------------------

	// We are going to encode the image line by line.
	// But before we encode, we are first going to build:
	// - tile-change bits (for the ESP32 to identify dirty tiles to upload to the DISPLAY)
	// - line-change bits
	// - run-change bits
	lineChanged := make([]uint8, h)   // indicates whether each line has any changes compared to the prev image
	spanChanged := make([][]uint8, h) // indicates whether each run of tiles in a line has any changes compared to the prev image, only for lines that have changes

	numChangedLines := 0
	numChangedSpans := 0
	for y := 0; y < h; y++ {
		lineHasChanges := uint8(0)
		curLineSpans := make([]uint8, numTilesX)
		spanChanged[y] = curLineSpans
		curLinePixels := curImage[y*w : y*w+w]
		prevLinePixels := prevImage[y*w : y*w+w]
		for x := 0; x < w; {
			if curLinePixels[x] != prevLinePixels[x] {
				lineHasChanges = 1
				tx := x >> tileWidthShift
				curLineSpans[tx] = 1
				numChangedSpans += 1
				// skip x up to the end of the current tile
				x = (tx + 1) << tileWidthShift
			} else {
				x++
			}
		}
		lineChanged[y] = lineHasChanges
		numChangedLines += int(lineHasChanges)
	}

	// ------------------------------------------------------------------
	// Build the line, run and tile streams
	// ------------------------------------------------------------------

	lineStream := NewBitStreamWriter(int64(h))
	lineStreamEncoded := NewBitStreamWriter(int64(h))
	for _, lineHasChanged := range lineChanged {
		lineStream.WriteBits(uint32(lineHasChanged), 1)
	}

	// Build the span and tile streams
	tileStream := NewBitStreamWriter(int64(numTilesTotal))
	tileStreamEncoded := NewBitStreamWriter(int64(numTilesTotal))
	tilesChangedTotal := 0
	for ty := 0; ty < numTilesY; ty++ {
		ys := ty * tileHeight
		ye := min((ty+1)*tileHeight, h)
		for tx := 0; tx < numTilesX; tx++ {
			tileHasChanged := uint8(0)
			for y := ys; y < ye; y++ {
				if spanChanged[y][tx] == 1 {
					tileHasChanged = 1
					break
				}
			}
			tileStream.WriteBits(uint32(tileHasChanged), 1)
			tilesChangedTotal += int(tileHasChanged)
		}
	}

	// Finalize these streams since we are done writing to them
	lineStreamNumBits, lineStreamNumBytes := lineStream.Finalize()
	tileStreamNumBits, tileStreamNumBytes := tileStream.Finalize()

	// ------------------------------------------------------------------
	// encoding using SRLEN + BitStream
	// Compress the line, run and tile streams
	// ------------------------------------------------------------------
	lineStreamRb, _ := Encode(lineStream.Reader(), 1, nil, lineStreamEncoded)
	tileStreamRb, _ := Encode(tileStream.Reader(), 1, nil, tileStreamEncoded)

	_, lineStreamEncodedNumBytes := lineStreamEncoded.Finalize()
	_, tileStreamEncodedNumBytes := tileStreamEncoded.Finalize()

	// ------------------------------------------------------------------
	// Encoding into selector and pixel streams
	// ------------------------------------------------------------------

	lineInfos := make([]LineInfo, h)

	globalSpanStream := NewBitStreamWriter(int64(numTilesX * h))
	globalSelectorStream := NewBitStreamWriter(int64(pixelCount * 2))
	globalP2Stream := NewBitStreamWriter(int64(p2NumPixels * 2))
	globalP4Stream := NewBitStreamWriter(int64(p4NumPixels * 4))
	globalP8Stream := NewBitStreamWriter(int64(p8NumPixels * 8))

	for ty := 0; ty < h; ty += 1 {
		if lineChanged[ty] == 1 {
			lineInfos[ty].Initialize(uint16(ty), w)

			curLinePixels := curImage[ty*w : ty*w+w]
			curSpanChanged := spanChanged[ty]

			for tx := 0; tx < w; tx += tileWidth {
				spanHasChanged := curSpanChanged[tx>>tileWidthShift]
				globalSpanStream.WriteBits(uint32(spanHasChanged), 1)
				lineInfos[ty].SpanStream.WriteBits(uint32(spanHasChanged), 1)

				// The span has changes, so we need to write pixels
				if spanHasChanged == 1 {
					pxe := min(tx+tileWidth, w)
					for pxi := tx; pxi < pxe; pxi += 1 {
						v := curLinePixels[pxi]
						ci := colorMapping[v]
						if ci >= 0 && ci < 4 {
							globalSelectorStream.WriteBits(SELECTOR_P2, 2)
							globalP2Stream.WriteBits(uint32(ci), 2)
							lineInfos[ty].SelectorStream.WriteBits(SELECTOR_P2, 2)
							lineInfos[ty].P2Stream.WriteBits(uint32(ci), 2)
						} else if ci >= 4 && ci < 20 {
							globalSelectorStream.WriteBits(SELECTOR_P4, 2)
							globalP4Stream.WriteBits(uint32(ci-4), 4)
							lineInfos[ty].SelectorStream.WriteBits(SELECTOR_P4, 2)
							lineInfos[ty].P4Stream.WriteBits(uint32(ci-4), 4)
						} else if ci >= 20 && ci < 276 {
							globalSelectorStream.WriteBits(SELECTOR_P8, 2)
							globalP8Stream.WriteBits(uint32(ci-20), 8)
							lineInfos[ty].SelectorStream.WriteBits(SELECTOR_P8, 2)
							lineInfos[ty].P8Stream.WriteBits(uint32(ci-20), 8)
						} else {
							globalSelectorStream.WriteBits(SELECTOR_RAW, 2)
							lineInfos[ty].SelectorStream.WriteBits(SELECTOR_RAW, 2)
							lineInfos[ty].P16Stream = append(lineInfos[ty].P16Stream, v)
						}
					}
				}
			}
		}
	}

	_, globalSpanStreamNumBytes := globalSpanStream.Finalize()
	_, globalSelectorStreamNumBytes := globalSelectorStream.Finalize()
	_, globalP2StreamNumBytes := globalP2Stream.Finalize()
	_, globalP4StreamNumBytes := globalP4Stream.Finalize()
	_, globalP8StreamNumBytes := globalP8Stream.Finalize()

	// ------------------------------------------------------------------
	// encoding of the global streams using SRLEN + BitStream
	// this is done to get the global rb values that we will use for encoding
	// these streams per line.
	// ------------------------------------------------------------------
	globalSpanStreamEncoded := NewBitStreamWriter(int64(numTilesX * h))
	globalSelectorStreamEncoded := NewBitStreamWriter(int64(pixelCount * 2))
	globalP2StreamEncoded := NewBitStreamWriter(int64(p2NumPixels * 2))
	globalP4StreamEncoded := NewBitStreamWriter(int64(p4NumPixels * 4))
	globalP8StreamEncoded := NewBitStreamWriter(int64(p8NumPixels * 8))

	globalSpanStreamRb, _ := Encode(globalSpanStream.Reader(), 1, nil, globalSpanStreamEncoded)
	globalSelectorStreamRb, _ := Encode(globalSelectorStream.Reader(), 2, nil, globalSelectorStreamEncoded)
	globalP2StreamRb, _ := Encode(globalP2Stream.Reader(), 2, nil, globalP2StreamEncoded)
	globalP4StreamRb, _ := Encode(globalP4Stream.Reader(), 4, nil, globalP4StreamEncoded)
	globalP8StreamRb, _ := Encode(globalP8Stream.Reader(), 8, nil, globalP8StreamEncoded)

	_, globalSpanStreamEncodedNumBytes := globalSpanStreamEncoded.Finalize()
	_, globalSelectorStreamEncodedNumBytes := globalSelectorStreamEncoded.Finalize()
	_, globalP2StreamEncodedNumBytes := globalP2StreamEncoded.Finalize()
	_, globalP4StreamEncodedNumBytes := globalP4StreamEncoded.Finalize()
	_, globalP8StreamEncodedNumBytes := globalP8StreamEncoded.Finalize()

	// ------------------------------------------------------------------
	// Now for each line that has changes, we compress the necessary streams
	// ------------------------------------------------------------------
	lineBasedSpanStreamNumBytes := 0
	lineBasedSelectorStreamNumBytes := 0
	lineBasedP2StreamNumBytes := 0
	lineBasedP4StreamNumBytes := 0
	lineBasedP8StreamNumBytes := 0

	lineBasedSpanStreamEncodedNumBytes := 0
	lineBasedSelectorStreamEncodedNumBytes := 0
	lineBasedP2StreamEncodedNumBytes := 0
	lineBasedP4StreamEncodedNumBytes := 0
	lineBasedP8StreamEncodedNumBytes := 0

	for ty := 0; ty < h; ty += 1 {
		if lineInfos[ty].Active {
			_, spanStreamNumBytes := lineInfos[ty].SpanStream.Finalize()
			_, selectorStreamNumBytes := lineInfos[ty].SelectorStream.Finalize()
			_, p2StreamNumBytes := lineInfos[ty].P2Stream.Finalize()
			_, p4StreamNumBytes := lineInfos[ty].P4Stream.Finalize()
			_, p8StreamNumBytes := lineInfos[ty].P8Stream.Finalize()

			lineBasedSpanStreamNumBytes += int(spanStreamNumBytes)
			lineBasedSelectorStreamNumBytes += int(selectorStreamNumBytes)
			lineBasedP2StreamNumBytes += int(p2StreamNumBytes)
			lineBasedP4StreamNumBytes += int(p4StreamNumBytes)
			lineBasedP8StreamNumBytes += int(p8StreamNumBytes)

			_, err := Encode(lineInfos[ty].SpanStream.Reader(), 1, globalSpanStreamRb, lineInfos[ty].SpanStreamEncoded)
			if err != nil {
				fmt.Printf("ERROR: failed to encode span stream for line %d: %v\n", ty, err)
				return
			}
			_, err = Encode(lineInfos[ty].SelectorStream.Reader(), 2, globalSelectorStreamRb, lineInfos[ty].SelectorStreamEncoded)
			if err != nil {
				fmt.Printf("ERROR: failed to encode selector stream for line %d: %v\n", ty, err)
				return
			}
			_, err = Encode(lineInfos[ty].P2Stream.Reader(), 2, globalP2StreamRb, lineInfos[ty].P2StreamEncoded)
			if err != nil {
				fmt.Printf("ERROR: failed to encode P2 stream for line %d: %v\n", ty, err)
				return
			}
			_, err = Encode(lineInfos[ty].P4Stream.Reader(), 4, globalP4StreamRb, lineInfos[ty].P4StreamEncoded)
			if err != nil {
				fmt.Printf("ERROR: failed to encode P4 stream for line %d: %v\n", ty, err)
				return
			}
			_, err = Encode(lineInfos[ty].P8Stream.Reader(), 8, globalP8StreamRb, lineInfos[ty].P8StreamEncoded)
			if err != nil {
				fmt.Printf("ERROR: failed to encode P8 stream for line %d: %v\n", ty, err)
				return
			}

			// Finalize the encoded streams for this line to get the final byte counts for the report
			_, spanStreamEncodedNumBytes := lineInfos[ty].SpanStreamEncoded.Finalize()
			_, selectorStreamEncodedNumBytes := lineInfos[ty].SelectorStreamEncoded.Finalize()
			_, p2StreamEncodedNumBytes := lineInfos[ty].P2StreamEncoded.Finalize()
			_, p4StreamEncodedNumBytes := lineInfos[ty].P4StreamEncoded.Finalize()
			_, p8StreamEncodedNumBytes := lineInfos[ty].P8StreamEncoded.Finalize()

			lineBasedSpanStreamEncodedNumBytes += int(spanStreamEncodedNumBytes)
			lineBasedSelectorStreamEncodedNumBytes += int(selectorStreamEncodedNumBytes)
			lineBasedP2StreamEncodedNumBytes += int(p2StreamEncodedNumBytes)
			lineBasedP4StreamEncodedNumBytes += int(p4StreamEncodedNumBytes)
			lineBasedP8StreamEncodedNumBytes += int(p8StreamEncodedNumBytes)
		}
	}

	// ------------------------------------------------------------------
	// Setup the frame header
	// ------------------------------------------------------------------
	header := NewFrameHeader()
	header.SetImgDimensions(uint16(w), uint16(h))
	header.SetTileDimensions(uint8(tileWidth), uint8(tileHeight))

	header.SetTileChange(tileStreamRb, uint32(tileStreamEncodedNumBytes), uint32(tileStreamNumBits/1))
	header.SetLineChange(lineStreamRb, uint32(lineStreamEncodedNumBytes), uint32(lineStreamNumBits/1))

	// ------------------------------------------------------------------
	// Using the LineInfo for each active line we can now build the full
	// frame data that can be send over TCP to the ESP32.
	// - frame header
	//   - image width, height
	//   - tile width, tile height
	//   - global selector rb array
	//   - global span rb array
	//   - global P2 rb array
	//   - global P4 rb array
	//   - global P8 rb array
	//   - line change array
	//   - tile change array
	// - per active line:
	//   - length of msg (u16)
	//   - line index (u16)
	//   - active streams (u16), as bitfield where:
	//     - bit 0 = span stream
	//     - bit 1 = selector stream
	//     - bit 2 = P2 stream
	//     - bit 3 = P4 stream
	//     - bit 4 = P8 stream
	//   - P16 stream; length(u16), data (u16*)
	//   - P8 stream; length(u16), data (u8*)
	//   - P4 stream; length(u16), data (u8*)
	//   - P2 stream; length(u16), data (u8*)
	//   - selector; length(u16), data (u8*)
	//   - span change stream; length(u16), data (u8*)
	//   - alignment to 4 bytes (if necessary)
	// - frame end marker

	// ------------------------------------------------------------------

	endTime := time.Now()
	elapsed := endTime.Sub(startTime)

	// Some global stats of the selector and pixel streams
	fmt.Println("----------------------------------------")
	fmt.Printf("Global span stream: %d bytes\n", globalSpanStreamNumBytes)
	fmt.Printf("Global selector stream: %d bytes\n", globalSelectorStreamNumBytes)
	fmt.Printf("Global P2 stream: %d bytes\n", globalP2StreamNumBytes)
	fmt.Printf("Global P4 stream: %d bytes\n", globalP4StreamNumBytes)
	fmt.Printf("Global P8 stream: %d bytes\n", globalP8StreamNumBytes)
	fmt.Println()
	fmt.Printf("Line-based span stream bytes: %d\n", lineBasedSpanStreamNumBytes)
	fmt.Printf("Line-based selector stream bytes: %d\n", lineBasedSelectorStreamNumBytes)
	fmt.Printf("Line-based P2 stream bytes: %d\n", lineBasedP2StreamNumBytes)
	fmt.Printf("Line-based P4 stream bytes: %d\n", lineBasedP4StreamNumBytes)
	fmt.Printf("Line-based P8 stream bytes: %d\n", lineBasedP8StreamNumBytes)
	fmt.Println("---- encoded -----")
	fmt.Printf("Global span stream: %d bytes\n", globalSpanStreamEncodedNumBytes)
	fmt.Printf("Global selector stream: %d bytes\n", globalSelectorStreamEncodedNumBytes)
	fmt.Printf("Global P2 stream: %d bytes\n", globalP2StreamEncodedNumBytes)
	fmt.Printf("Global P4 stream: %d bytes\n", globalP4StreamEncodedNumBytes)
	fmt.Printf("Global P8 stream: %d bytes\n", globalP8StreamEncodedNumBytes)
	fmt.Println()
	fmt.Printf("Line-based span stream bytes: %d\n", lineBasedSpanStreamEncodedNumBytes)
	fmt.Printf("Line-based selector stream bytes: %d\n", lineBasedSelectorStreamEncodedNumBytes)
	fmt.Printf("Line-based P2 stream bytes: %d\n", lineBasedP2StreamEncodedNumBytes)
	fmt.Printf("Line-based P4 stream bytes: %d\n", lineBasedP4StreamEncodedNumBytes)
	fmt.Printf("Line-based P8 stream bytes: %d\n", lineBasedP8StreamEncodedNumBytes)
	fmt.Println("----------------------------------------")

	// ------------------------------------------------------------------
	// Print current and previous image info
	// ------------------------------------------------------------------
	fmt.Println("----------------------------------------")
	fmt.Println("Current Image")
	PrintImageInfo(hist, w, h)
	fmt.Println("----------------------------------------")
	prevHist := BuildHistogram(prevImage, w, h)
	fmt.Println("Previous Image")
	PrintImageInfo(prevHist, w, h)
	fmt.Println("----------------------------------------")

	// ------------------------------------------------------------------
	// Tile report
	// ------------------------------------------------------------------
	fmt.Println("Diff Information:")
	fmt.Printf("   Tile Size: %dx%d\n", tileWidth, tileHeight)
	fmt.Printf("   Tile Count: %dx%d = %d\n", numTilesX, numTilesY, numTilesTotal)
	fmt.Printf("   Tiles Changed: %d (%.2f%%)\n", tilesChangedTotal, float64(tilesChangedTotal)/float64(numTilesTotal)*100)
	fmt.Printf("   Lines Changed: %d\n", numChangedLines)
	fmt.Printf("   Spans Changed: %d\n", numChangedSpans)
	fmt.Println("----------------------------------------")

	// ------------------------------------------------------------------
	// Report
	// ------------------------------------------------------------------
	rawBytes := pixelCount * 2
	fmt.Printf("Streams:\n")
	fmt.Printf("P16      : %6d bytes\n", p16NumPixels*2)
	fmt.Printf("P8       : %6d bytes -> %6d bytes (SRLEN, ratio %.2fx)\n", globalP8StreamNumBytes, globalP8StreamEncodedNumBytes, float64(globalP8StreamNumBytes)/float64(globalP8StreamEncodedNumBytes))
	fmt.Printf("P4       : %6d bytes -> %6d bytes (SRLEN, ratio %.2fx)\n", globalP4StreamNumBytes, globalP4StreamEncodedNumBytes, float64(globalP4StreamNumBytes)/float64(globalP4StreamEncodedNumBytes))
	fmt.Printf("P2       : %6d bytes -> %6d bytes (SRLEN, ratio %.2fx)\n", globalP2StreamNumBytes, globalP2StreamEncodedNumBytes, float64(globalP2StreamNumBytes)/float64(globalP2StreamEncodedNumBytes))
	fmt.Printf("Selector : %6d bytes -> %6d bytes (SRLEN, ratio %.2fx)\n", globalSelectorStreamNumBytes, globalSelectorStreamEncodedNumBytes, float64(globalSelectorStreamNumBytes)/float64(globalSelectorStreamEncodedNumBytes))
	fmt.Printf("Span     : %6d bytes -> %6d bytes (SRLEN, ratio %.2fx)\n", globalSpanStreamNumBytes, globalSpanStreamEncodedNumBytes, float64(globalSpanStreamNumBytes)/float64(globalSpanStreamEncodedNumBytes))
	fmt.Printf("Line     : %6d bytes -> %6d bytes (SRLEN, ratio %.2fx)\n", lineStreamNumBytes, lineStreamEncodedNumBytes, float64(lineStreamNumBytes)/float64(lineStreamEncodedNumBytes))
	fmt.Printf("Tile     : %6d bytes -> %6d bytes (SRLEN, ratio %.2fx)\n", tileStreamNumBytes, tileStreamEncodedNumBytes, float64(tileStreamNumBytes)/float64(tileStreamEncodedNumBytes))
	fmt.Println("----------------------------------------")

	total := int64(tileStreamEncodedNumBytes)
	total += int64(lineStreamEncodedNumBytes)
	total += int64(globalSpanStreamEncodedNumBytes)
	total += int64(globalSelectorStreamEncodedNumBytes)
	total += int64(globalP2StreamEncodedNumBytes)
	total += int64(globalP4StreamEncodedNumBytes)
	total += int64(globalP8StreamEncodedNumBytes)
	total += int64(p16NumPixels * 2)
	fmt.Printf("Total encoded: %d bytes (%.2fx)\n", total, float64(rawBytes)/float64(total))
	fmt.Printf("Total encoding time: %s\n", elapsed)
}
