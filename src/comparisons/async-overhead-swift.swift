protocol Fooer : Sendable {
    func foo() async
}

enum PerTaskFooer {
    @TaskLocal
    static var fooer: Fooer? = Optional.none
}

@inline(never)
func foo() async {
    if let f = PerTaskFooer.fooer {
        await f.foo()
    }
}

@inline(never)
func bar(_ count: Int) async {
    for _ in 0..<count {
        await foo()
    }
}

func main() async {
    let clock = ContinuousClock.continuous
    let count = 100_000_000
    let start = clock.now
    await bar(count)
    let end = clock.now

    let elapsedNs = (end - start) / Duration.nanoseconds(1)
    let callNs = elapsedNs / Double(count)
    print("Single call is \(callNs)ns")
}

await main()
