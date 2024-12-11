using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;

public interface Fooer {
    Task Foo();
}

public class Program {
    public static Fooer? fooer = null;

    public static async Task Foo0() {
        if (fooer != null) {
            await fooer.Foo();
        }
    }

    public static async Task Foo1() {
        await Foo0();
    }

    public static async Task Foo2() {
        await Foo1();
    }

    public static async Task Foo3() {
        await Foo2();
    }

    public static async Task Foo4() {
        await Foo3();
    }

    public static async Task Foo5() {
        await Foo4();
    }

    public static async Task FooN(int n) {
        if (n > 1) {
            await FooN(n - 1);
        } else {
            await Foo0();
        }
    }

    public static async Task Bar(int count, int n, bool dyn) {
        if (dyn) {
            for (int i = 0; i < count; ++i) {
                await FooN(n);
            }
        } else {
            switch (n) {
            case 5:
                for (int i = 0; i < count; ++i) {
                    await Foo5();
                }
                break;
            case 4:
                for (int i = 0; i < count; ++i) {
                    await Foo4();
                }
                break;
            case 3:
                for (int i = 0; i < count; ++i) {
                    await Foo3();
                }
                break;
            case 2:
                for (int i = 0; i < count; ++i) {
                    await Foo2();
                }
                break;
            case 1:
                for (int i = 0; i < count; ++i) {
                    await Foo1();
                }
                break;
            }
        }
    }

    public static async Task Main(string[] args) {
        var count = 100_000_000;

        for (int n = 1; n <= 5; ++n) {
            for (int dyn = 0; dyn <= 1; ++dyn) {
                var sw = Stopwatch.StartNew();

                await Bar(count, n, dyn != 0);

                var elapsed = sw.Elapsed;
                var nsPerCall = (int)(elapsed.TotalNanoseconds / count);
                var suffix = dyn != 0 ? " dynamic" : "";
                Console.WriteLine($"Single call is {nsPerCall}ns for n={n}{suffix}");
            }
        }
    }
}
