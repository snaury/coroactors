using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;

public interface Pingable {
    Task<int> Ping();
    Task<int> GetCounter();
}

public interface Pinger {
    Task<TimeSpan> Run(int count);
}

public class PingableImpl : Pingable {
    private SemaphoreSlim semaphore = new SemaphoreSlim(1, 1);
    private int counter = 0;

    public async Task<int> Ping() {
        await semaphore.WaitAsync();
        try {
            return ++counter;
        } finally {
            semaphore.Release();
        }
    }

    public async Task<int> GetCounter() {
        await semaphore.WaitAsync();
        try {
            return counter;
        } finally {
            semaphore.Release();
        }
    }
}

public class PingerImpl : Pinger {
    private Pingable pingable;
    private long preemptTicks;

    public PingerImpl(Pingable pingable, int preemptUs = 10) {
        this.pingable = pingable;
        this.preemptTicks = TimeSpan.FromMicroseconds(preemptUs).Ticks;
    }

    public async Task<TimeSpan> Run(int count) {
        var sw = Stopwatch.StartNew();
        var start = new TimeSpan(0);
        var maxLatency = new TimeSpan(0);
        await Task.Yield();
        long lastPreempted = sw.Elapsed.Ticks;
        for (int i = 0; i < count; ++i) {
            await pingable.Ping();
            var end = sw.Elapsed;
            var elapsed = end - start;
            if (maxLatency < elapsed) {
                maxLatency = elapsed;
            }
            if (end.Ticks - lastPreempted >= preemptTicks) {
                await Task.Yield();
                lastPreempted = sw.Elapsed.Ticks;
            }
            start = end;
        }
        return maxLatency;
    }
}

public class Program {
    public static async Task Main(string[] args) {
        var count = 10_000_000;
        var numPingers = 1;
        var numPingables = 1;
        var preemptUs = 10;
        for (int i = 0; i < args.Length; ++i) {
            switch (args[i]) {
            case "-c":
            case "--count":
                count = int.Parse(args[++i]);
                break;
            case "-p":
                numPingers = int.Parse(args[++i]);
                numPingables = numPingers;
                break;
            case "--pingers":
                numPingers = int.Parse(args[++i]);
                break;
            case "--pingables":
                numPingables = int.Parse(args[++i]);
                break;
            case "--preempt-us":
                preemptUs = int.Parse(args[++i]);
                break;
            default:
                Console.WriteLine($"ERROR: unsupported argument {args[i]}");
                Environment.Exit(1);
                break;
            }
        }

        var pingables = new List<Pingable>();
        for (int i = 0; i < numPingables; ++i) {
            pingables.Add(new PingableImpl());
        }

        var pingers = new List<Pinger>();
        for (int i = 0; i < numPingers; ++i) {
            pingers.Add(new PingerImpl(pingables[i % pingables.Count], preemptUs));
        }

        await pingers[0].Run(count / pingers.Count / 100);

        var sw = Stopwatch.StartNew();
        var tasks = new List<Task<TimeSpan>>();
        for (int i = 0; i < pingers.Count; ++i) {
            int m = count % pingers.Count;
            tasks.Add(pingers[i].Run(count / pingers.Count + (i < m ? 1 : 0)));
        }
        var maxLatency = await Task.WhenAll(tasks).ContinueWith((r) => r.Result.Max());
        var elapsed = sw.Elapsed;

        var elapsedMs = (int)(elapsed.TotalMilliseconds);
        var rps = (int)(count / elapsed.TotalSeconds);
        var maxLatencyUs = (int)(maxLatency.TotalMicroseconds);

        Console.WriteLine($"Finished in {elapsedMs}ms ({rps}/s), max latency = {maxLatencyUs}us");
    }
}
