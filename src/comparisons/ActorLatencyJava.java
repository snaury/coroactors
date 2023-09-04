import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

public class ActorLatencyJava {

    public interface Pingable {
        long ping();
        long getCounter();
    }

    public static class ThreadSafePingable implements Pingable {
        private Lock lock = new ReentrantLock();
        private long counter = 0;

        @Override
        public long ping() {
            lock.lock();
            try {
                return ++counter;
            } finally {
                lock.unlock();
            }
        }

        @Override
        public long getCounter() {
            lock.lock();
            try {
                return counter;
            } finally {
                lock.unlock();
            }
        }
    }

    public static class Pinger {
        private final Pingable pingable;
        private final boolean withPreemption;

        public Pinger(Pingable pingable, boolean withPreemption) {
            this.pingable = pingable;
            this.withPreemption = withPreemption;
        }

        public static class Result {
            public final long maxLatency;

            public Result(long maxLatency) {
                this.maxLatency = maxLatency;
            }
        }

        public Result run(long count, long start) {
            long end = System.nanoTime();
            long maxLatency = end - start;
            for (long i = 0; i < count; ++i) {
                long callStart = end;
                pingable.ping();
                long callEnd = System.nanoTime();
                if (maxLatency < callEnd - callStart) {
                    maxLatency = callEnd - callStart;
                }
                end = callEnd;
                if (withPreemption && end - start >= 10000) {
                    Thread.yield();
                    start = end;
                }
            }
            return new Result(maxLatency);
        }
    }

    public static void main(String[] args) throws InterruptedException {
        int numPingers = 1;
        int numPingables = 1;
        long count = 10_000_000;
        boolean withPreemption = false;
        for (int i = 0; i < args.length; ++i) {
            switch (args[i]) {
                case "-p":
                    numPingers = Integer.parseInt(args[++i]);
                    numPingables = numPingers;
                    break;
                case "--pingers":
                    numPingers = Integer.parseInt(args[++i]);
                    break;
                case "--pingables":
                    numPingables = Integer.parseInt(args[++i]);
                    break;
                case "-c":
                case "--count":
                    count = Long.parseLong(args[++i]);
                    break;
                case "--with-preemption":
                    withPreemption = true;
                    break;
                default:
                    System.err.printf("ERROR: unsupported argument: %s\n", args[i]);
                    System.exit(1);
            }
        }

        Pingable[] pingables = new Pingable[numPingables];
        for (int i = 0; i < numPingables; ++i) {
            pingables[i] = new ThreadSafePingable();
        }

        Pinger[] pingers = new Pinger[numPingers];
        for (int i = 0; i < numPingers; ++i) {
            pingers[i] = new Pinger(pingables[i % numPingables], withPreemption);
        }

        System.out.printf("Warming up...\n");
        long r = count / numPingers / 100;
        pingers[0].run(r, System.nanoTime());

        System.out.printf("Starting...\n");
        final long start = System.nanoTime();
        final Thread[] threads = new Thread[numPingers];
        final Pinger.Result[] results = new Pinger.Result[numPingers];
        final long m = count % numPingers;
        for (int i = 0; i < numPingers; ++i) {
            final int index = i;
            final long n = count / numPingers + (i < m ? 1 : 0);
            threads[i] = Thread.ofVirtual().factory().newThread(() -> {
                results[index] = pingers[index].run(n, start);
            });
        }
        for (var thread : threads) {
            thread.start();
        }
        for (var thread : threads) {
            thread.join();
        }
        final long end = System.nanoTime();

        long maxLatency = 0;
        for (var result : results) {
            if (maxLatency < result.maxLatency) {
                maxLatency = result.maxLatency;
            }
        }

        long sum = 0;
        for (var pingable : pingables) {
            sum += pingable.getCounter();
        }
        if (sum != r + count) {
            System.err.printf("Final counter sum doesn't match: expected %d + %d == %d, found %d\n",
                r, count, r + count, sum);
            System.exit(1);
        }

        long elapsedNs = end - start;
        long elapsedMs = elapsedNs / 1_000_000;
        double elapsedSeconds = elapsedNs / 1_000_000_000.0;
        long rps = (long) (count / elapsedSeconds);
        long maxLatencyUs = maxLatency / 1_000;
        System.out.printf("Finished in %dms (%d/s), max latency = %dus\n",
            elapsedMs, rps, maxLatencyUs);
    }

}
