package main

import (
	"log"
	"os"
	"runtime"
	"strconv"
	"sync"
	"time"
)

type Pingable interface {
	Ping() int
	GetCounter() int
}

type Pinger interface {
	Run(n int, start time.Time, withLatencies bool, withReschedule bool) time.Duration
}

type pingable struct {
	lock    sync.Mutex
	counter int
}

func newPingable() Pingable {
	return &pingable{}
}

func (p *pingable) Ping() int {
	p.lock.Lock()
	defer p.lock.Unlock()
	p.counter += 1
	return p.counter
}

func (p *pingable) GetCounter() int {
	p.lock.Lock()
	defer p.lock.Unlock()
	return p.counter
}

type pinger struct {
	lock     sync.Mutex
	pingable Pingable
}

func newPinger(pingable Pingable) Pinger {
	return &pinger{
		pingable: pingable,
	}
}

func (p *pinger) Run(n int, start time.Time, withLatencies bool, withReschedule bool) time.Duration {
	var maxLatency time.Duration
	if withLatencies {
		end := time.Now()
		latency := end.Sub(start)
		if maxLatency < latency {
			maxLatency = latency
		}
		for i := 0; i < n; i++ {
			if withReschedule {
				runtime.Gosched()
			}
			callStart := end
			p.pingable.Ping()
			callEnd := time.Now()
			latency = callEnd.Sub(callStart)
			if maxLatency < latency {
				maxLatency = latency
			}
			end = callEnd
		}
	} else {
		for i := 0; i < n; i++ {
			p.pingable.Ping()
		}
	}
	return maxLatency
}

func mustParseInt(s string) int {
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		log.Fatalf("Failed to parse: %v", err)
	}
	return int(n)
}

func main() {
	threads := 1
	numPingers := 1
	numPingables := 1
	count := 10_000_000
	withLatencies := true
	withReschedule := false

	// Note: this just matches other programs parameters
	for i := 1; i < len(os.Args); {
		switch os.Args[i] {
		case "-t":
			i += 1
			threads = mustParseInt(os.Args[i])
		case "-p":
			i += 1
			numPingers = mustParseInt(os.Args[i])
			numPingables = numPingers
		case "--pingers":
			i += 1
			numPingers = mustParseInt(os.Args[i])
		case "--pingables":
			i += 1
			numPingables = mustParseInt(os.Args[i])
		case "-c", "--count":
			i += 1
			count = mustParseInt(os.Args[i])
		case "--without-latencies":
			withLatencies = false
		case "--with-reschedule":
			withReschedule = true
		default:
			log.Fatalf("Unexpected argument: " + os.Args[i])
		}
		i += 1
	}

	runtime.GOMAXPROCS(threads)

	var pingables []Pingable
	for i := 0; i < numPingables; i++ {
		pingables = append(pingables, newPingable())
	}
	var pingers []Pinger
	for i := 0; i < numPingers; i++ {
		pingers = append(pingers, newPinger(pingables[i%numPingables]))
	}

	log.Printf("Warming up...")
	r := count / numPingers / 100
	pingers[0].Run(r, time.Now(), withLatencies, withReschedule)

	log.Printf("Starting...")
	start := time.Now()
	results := make([]time.Duration, numPingers)
	var wg sync.WaitGroup
	for i := range pingers {
		n := count / numPingers
		if i < (count % numPingers) {
			n += 1
		}
		wg.Add(1)
		go func(i, n int) {
			defer wg.Done()
			results[i] = pingers[i].Run(n, start, withLatencies, withReschedule)
		}(i, n)
	}
	wg.Wait()
	end := time.Now()

	var maxLatency time.Duration
	for _, result := range results {
		if maxLatency < result {
			maxLatency = result
		}
	}

	sum := 0
	for _, pingable := range pingables {
		sum += pingable.GetCounter()
	}
	if sum != r+count {
		log.Fatalf("Counters didn't match: expected %v + %v = %v, got %v", r, count, r+count, sum)
	}

	elapsed := end.Sub(start).Seconds()
	elapsedMs := end.Sub(start) / time.Millisecond
	rps := int(float64(count) / elapsed)

	log.Printf("Finished in %dms (%d/s), max latency = %dus", elapsedMs, rps, maxLatency.Microseconds())
}
