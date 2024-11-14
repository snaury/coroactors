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
        if (n > 0) {
            await FooN(n - 1);
        } else {
            await Foo0();
        }
    }

    public static async Task Bar(int count) {
        for (int i = 0; i < count; ++i) {
            await Foo5();
        }
    }

    public static async Task Main(string[] args) {
        var count = 100_000_000;
        var sw = Stopwatch.StartNew();

        await Bar(count);

        var elapsed = sw.Elapsed;

        var nsPerCall = (int)(elapsed.TotalNanoseconds / count);

        Console.WriteLine($"Single call is {nsPerCall}ns");
    }
}
