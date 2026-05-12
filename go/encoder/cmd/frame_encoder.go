package main

import (
	"flag"
	"fmt"
	"image/png"
	"os"

	fe "github.com/jurgen-kluft/cmui/docs/tools/FrameEncoder"
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

func loadImage(path string) (pixels []uint16, w int, h int) {
	f, err := os.Open(path)
	if err != nil {
		panic(err)
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		panic(err)
	}

	w = img.Bounds().Dx()
	h = img.Bounds().Dy()

	minY := img.Bounds().Min.Y
	maxY := img.Bounds().Max.Y
	minX := img.Bounds().Min.X
	maxX := img.Bounds().Max.X

	pixels = make([]uint16, w*h)
	for y := minY; y < maxY; y++ {
		pixelsLine := pixels[(y-minY)*w : (y-minY+1)*w]
		for x := minX; x < maxX; x++ {
			r, g, b, _ := img.At(x, y).RGBA()
			pixelsLine[x-minX] = rgb565(r>>8, g>>8, b>>8)
		}
	}

	return pixels, w, h
}

func getImages(curPath, prevPath string, shift bool, fill uint32) (curPixels []uint16, prevPixels []uint16, w int, h int) {

	curPixels, w, h = loadImage(curPath)
	pixelCount := int64(w * h)

	// ------------------------------------------------------------------
	// Prepare previous image, palette, histogram and other data
	// ------------------------------------------------------------------
	prevPixels = make([]uint16, w*h) // default to all black if no prev image
	if prevPath != "" {
		pw := 0
		ph := 0
		prevPixels, pw, ph = loadImage(prevPath)
		if pw != w || ph != h {
			fmt.Printf("ERROR: prev image dimensions (%dx%d) do not match next image dimensions (%dx%d)\n", pw, ph, w, h)
			os.Exit(1)
		}
	} else {
		prevPixels = make([]uint16, pixelCount)
		if shift {
			copy(prevPixels[w:], curPixels[:pixelCount-int64(w)])
		} else {
			// fill prevPixels with the specified color
			fillColor := rgba8888_to_rgb565(fill)
			for i := range prevPixels {
				prevPixels[i] = fillColor
			}
		}
	}

	return curPixels, prevPixels, w, h
}

func main() {
	var (
		nextPath  = flag.String("main", "", "main PNG image (required)")
		prevPath  = flag.String("prev", "", "prev PNG image (optional)")
		tileSize  = flag.Int("tile-size", 16, "tile size: 8, 16 or 32")
		prevShift = flag.Bool("prev-shift", false, "shift next image down by 1 line as prev")
		prevFill  = flag.Uint("prev-fill", 0x000000, "fill prev image with a color (black as default)")
	)
	flag.Parse()

	if *nextPath == "" {
		fmt.Println("ERROR: -main image required")
		os.Exit(1)
	}
	if *tileSize != 8 && *tileSize != 16 && *tileSize != 32 {
		fmt.Println("ERROR: -tile-size must be one of: 8, 16 or 32")
		os.Exit(1)
	}

	curPixels, prevPixels, w, h := getImages(*nextPath, *prevPath, *prevShift, uint32(*prevFill))

	fe.EncodeFrame(w, h, *tileSize, *tileSize, curPixels, prevPixels)

}
