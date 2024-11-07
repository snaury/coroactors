protocol Fooer : Sendable {
    func foo() async
}

enum PerTaskFooer {
    @TaskLocal
    static var fooer: Fooer? = Optional.none
}

func foo0() async {
    if let f = PerTaskFooer.fooer {
        await f.foo()
    }
}

func foo1() async {
    await foo0()
}

func foo2() async {
    await foo1()
}

func foo3() async {
    await foo2()
}

func foo4() async {
    await foo3()
}

func foo5() async {
    await foo4()
}

func bar(_ count: Int) async {
    for _ in 0..<count {
        await foo5()
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
