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

func fooN(_ n: Int) async {
    if n > 1 {
        await fooN(n - 1)
    } else {
        await foo0()
    }
}

func bar(_ count: Int, _ n: Int, _ dyn: Bool) async {
    if dyn {
        for _ in 0..<count {
            //await foo5()
            await fooN(n)
        }
    } else {
        switch n {
        case 5:
            for _ in 0..<count {
                await foo5()
            }
        case 4:
            for _ in 0..<count {
                await foo4()
            }
        case 3:
            for _ in 0..<count {
                await foo3()
            }
        case 2:
            for _ in 0..<count {
                await foo2()
            }
        case 1:
            for _ in 0..<count {
                await foo1()
            }
        default:
            break
        }
    }
}

func main() async {
    let clock = ContinuousClock.continuous
    let count = 100_000_000

    for n in 1...5 {
        for dyn in 0...1 {
            let start = clock.now
            await bar(count, n, dyn != 0)
            let end = clock.now

            let elapsedNs = (end - start) / Duration.nanoseconds(1)
            let callNs = elapsedNs / Double(count)
            let suffix = dyn != 0 ? " dynamic" : ""
            print("Single call is \(callNs)ns for n=\(n)\(suffix)")
        }
    }
}

await main()
