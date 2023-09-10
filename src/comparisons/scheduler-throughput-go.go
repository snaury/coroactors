package main

import (
	"log"
	"os"
	"runtime"
	"strconv"
	"sync/atomic"
	"time"

	_ "unsafe"
)

func mustParseInt(s string) int {
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		log.Fatalf("Failed to parse: %v", err)
	}
	return int(n)
}

//go:linkname goyield runtime.goyield
func goyield()

type state struct {
	counter uint64
	padding [128 - 8]byte
}

func (s *state) spinGoyield() {
	for {
		atomic.AddUint64(&s.counter, 1)
		goyield()
	}
}

func (s *state) spinGosched() {
	for {
		atomic.AddUint64(&s.counter, 1)
		runtime.Gosched()
	}
}

func (s *state) getCount() uint64 {
	return atomic.SwapUint64(&s.counter, 0)
}

func main() {
	threads := 1
	numTasks := 1
	useGosched := false

	// Note: this just matches other programs parameters
	for i := 1; i < len(os.Args); {
		switch os.Args[i] {
		case "-t":
			i += 1
			threads = mustParseInt(os.Args[i])
		case "-p":
			i += 1
			numTasks = mustParseInt(os.Args[i])
		case "--use-gosched":
			useGosched = true
		default:
			log.Fatalf("Unexpected argument: " + os.Args[i])
		}
		i += 1
	}

	runtime.GOMAXPROCS(threads)

	var states []*state
	for i := 0; i < numTasks; i++ {
		s := new(state)
		if useGosched {
			go s.spinGosched()
		} else {
			go s.spinGoyield()
		}
		states = append(states, s)
	}

	for {
		time.Sleep(time.Second)
		var sum, min, max uint64
		for i := 0; i < numTasks; i++ {
			count := states[i].getCount()
			if i == 0 {
				sum = count
				min = count
				max = count
			} else {
				sum += count
				if min > count {
					min = count
				}
				if max < count {
					max = count
				}
			}
		}
		log.Printf("... %d/s (min=%d/s, max=%d/s)", sum, min, max)
	}
}
