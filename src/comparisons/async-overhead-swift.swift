protocol Fooer {
    func foo() async
}

let g_fooer = TaskLocal<Fooer?>(wrappedValue: Optional.none)

@inline(never)
func foo() async {
    if let fooer = g_fooer.get() {
        await fooer.foo()
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
