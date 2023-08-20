let clock = ContinuousClock.continuous

actor Pingable {
    private var counter: Int = 0

    func ping() -> Int {
        counter += 1
        return counter
    }

    func getCounter() -> Int {
        return counter
    }
}

actor Pinger {
    private var target: Pingable

    init(_ target: Pingable) {
        self.target = target
    }

    func run(_ n: Int, start: ContinuousClock.Instant, withLatencies: Bool = true) async -> Duration {
        var maxLatency: Duration = .seconds(0)
        if withLatencies {
            var end = clock.now
            maxLatency = end - start
            for _ in 0..<n {
                let callStart = end
                _ = await self.target.ping()
                let callEnd = clock.now
                let latency = callEnd - callStart
                if maxLatency < latency {
                    maxLatency = latency
                }
                end = callEnd
            }
        } else {
            for _ in 0..<n {
                _ = await self.target.ping()
            }
        }
        return maxLatency
    }
}

func main() async {
    var numPingers = 1
    var numPingables = 1
    var count = 10_000_000
    var withLatencies = true

    var i = 1
    let args = CommandLine.arguments
    while i < args.count {
        switch args[i] {
        case "-p":
            i += 1
            numPingers = Int(args[i])!
            numPingables = numPingers
        case "--pingers":
            i += 1
            numPingers = Int(args[i])!
        case "--pingables":
            i += 1
            numPingables = Int(args[i])!
        case "-c", "--count":
            i += 1
            count = Int(args[i])!
        case "--without-latencies":
            withLatencies = false
        default:
            precondition(false, "Unexpected argument \(args[i])")
        }
        i += 1
    }

    var pingables = [Pingable]()
    for _ in 0..<numPingables {
        pingables.append(Pingable())
    }
    var pingers = [Pinger]()
    for i in 0..<numPingers {
        pingers.append(Pinger(pingables[i % numPingables]))
    }

    print("Warming up...")
    var r = count / numPingers / 100
    _ = await pingers[0].run(r, start: clock.now, withLatencies: withLatencies)

    print("Starting...")
    let start = clock.now
    let maxLatency = await withTaskGroup(of: Duration.self) { group in
        let m = count % numPingers
        for i in 0..<numPingers {
            let n = (count / numPingers) + (i < m ? 1 : 0)
            let pinger = pingers[i]
            let wl = withLatencies
            group.addTask {
                return await pinger.run(n, start: start, withLatencies: wl)
            }
        }

        var maxLatency: Duration = .seconds(0)
        for await latency in group {
            if maxLatency < latency {
                maxLatency = latency
            }
        }
        return maxLatency
    }
    let end = clock.now

    for pingable in pingables {
        r += await pingable.getCounter()
    }
    assert(r == count)

    let elapsed = end - start
    let elapsedSeconds = elapsed / Duration.seconds(1)
    let elapsedMs = Int(elapsedSeconds * 1000)
    let rps = Int(Double(count) / elapsedSeconds)
    let maxLatencyUs = maxLatency / Duration.microseconds(1)
    print("Finished in \(elapsedMs)ms (\(rps)/s), max latency = \(maxLatencyUs)us")
}

await main()
