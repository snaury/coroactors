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

func spinGoyield(counter *atomic.Uint64) {
	for {
		counter.Add(1)
		goyield()
	}
}

func spinGosched(counter *atomic.Uint64) {
	for {
		counter.Add(1)
		runtime.Gosched()
	}
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

	var counters []*atomic.Uint64
	for i := 0; i < numTasks; i++ {
		counter := new(atomic.Uint64)
		if useGosched {
			go spinGosched(counter)
		} else {
			go spinGoyield(counter)
		}
		counters = append(counters, counter)
	}

	for {
		time.Sleep(time.Second)
		var sum, min, max uint64
		for i := 0; i < numTasks; i++ {
			count := counters[i].Swap(0)
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
