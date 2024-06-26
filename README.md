# coroactors
Experimental actors with C++ coroutines

This is a header-only library with collection of utilities for writing thread-safe easy to use coroutines suitable for multi-threaded executors. Library classes mostly use lock-free and wait-free primites for synchronization.

## Preface

I've been working on [YDB](https://ydb.tech) (a recently open-sourced distributed SQL database) for many years, and got a bit of experience in programming with actors as a result. Actor model is great at isolating mutable state, communicating using messaging kinda matches the real world, and having messages serializable makes communicating across the network almost identical to a single process. In theory that makes systems scalable, you just spread your actors across different nodes.

In practice, however, there are many drawbacks. First is the mental model, you have to invert your (often serial) business logic into a state machine, and it's very easy to introduce subtle bugs. Second, communicating with messages is very expensive, compared to a normal method call it's one or two orders of magnitude more expensive. Those two points lead to horrible trade offs: to make state machines smaller and understandable you need to use more actors, but the more actors you have the more communucation you have. And just because your actors don't have shared mutable state doesn't mean the actor system doesn't. Registering short lived actors, dispatching events to a global run queue, all of those become a bottleneck.

C++ has coroutines, and just like regular functions its local state is not shared by default. When coroutine suspends it may resume on another thread, but its body doesn't run in parallel, and you don't need mutexes to work with local variables. They are great for matching the mental model, you don't need to create a state machine every time (compiler does that for you). But just like with threads and regular functions, shared state needs isolation, and mutexes with coroutines would be even more error-prone than with regular functions.

[Swift actors](https://docs.swift.org/swift-book/documentation/the-swift-programming-language/concurrency/#Actors) show us there's a way to have both: asynchronous, maintainable and easy to understand methods instead of state machines, and isolated mutable state without using locks. What's more important actors in Swift isolate state to actor methods by default, so making a long running async fetch in a caching service won't stop reads from running concurrently.

This repository takes ideas from the Swift language and standard library, and tries to mesh them with C++ coroutines hoping to achieve high-performance and low-latency actors that are easy to code.

## Actors with C++ coroutines

The primary class is `async<T>` and should be used as the return type for actor coroutines. This is an eagerly started coroutine that must either `co_return` immediately or `co_await` on `actor_context::operator()` call first, at which point it binds to the specified context and suspends until `co_await`ed. When bound to a context actor will never execute in parallel with other actor coroutines bound to the same context, effectively acting like a local mutex, guaranteeing exclusive access to shared state protected by this context.

```c++
class Counter {
public:
    // ...

    async<int> get() const {
        co_await context();
        co_return value_;
    }

    async<void> set(int value) {
        co_await context();
        value_ = value;
    }

    async<int> increment() {
        co_await context();
        co_return ++value_;
    }

private:
    actor_context context;
    int value_ = 0;
};
```

Multiple coroutines may be calling `Counter` methods concurrently, but all accesses to the `value_` member variables will be serialized automatically.

Unlike a mutex context is automatically released when actor `co_await`s an awaitable, and reacquired before that awaitable returns. For efficiency this release/reacquire only happens when coroutine actually suspends. For example:

```c++
// Note: not an actor coroutine
task<std::string> fetch_url(const std::string& url);

class CachingService {
public:
    // ...

    async<std::string> get(const std::string& url) {
        co_await context();
        if (!cache.contains(url)) {
            // Context automatically released on co_await
            std::string data = co_await fetch_url(url);
            // Context automatically reacquired here
            cache[url] = data;
        }
        co_return cache[url];
    }

private:
    actor_context context;
    std::unordered_map<std::string, std::string> cache;
}
```

Multiple coroutines may be calling `CachingService::get`, getting cached results most of the time. And a very long fetch of a particular url will not block other coroutines from getting results that are already cached.

## Fast context switching

In a system with many actors it should be very common for actors to often call other actors instead of arbitrary coroutines. The first common case would be for actor to call its own methods, and since they are on the same context it should not be released and reacquired everytime. Instead coroutine uses direct transfer between coroutine frames, which is similar to how normal functions transfer stack to a nested call and how it returns.

The second common case would be for calls to an uncontended service, and since it's uncontended it should also use direct frame transfer when possible, switching to a new available context, releasing the old context (maybe rescheduling it when there is more work), and doing the same on the way back. Actor mailboxes are wait-free and don't need to involve the scheduler or the operating system when there is no contention.

When there is contention between actors (target mailbox is locked by another thread, or the source mailbox is not empty) we have to involve the scheduler for one side or the other. Actors prefer switching to the new context when possible, rescheduling the old context when it is not empty, since caches (e.g. arguments or the result) are more likely to stay warm when not interleaved with other work, and especially when not switching threads or cores. This is probably key for C++ coroutine actors here, as opposed to classical actors, because unlike message passing we know when frames are switching and the parent frame is suspending.

## Coroutine handle destruction

Coroutine handle destruction in C++ is tricky. It seems to be common to have coroutine awaiters in the wild that unconditionally destroy handles in destructors (e.g folly, stdexec). This performs a bottom-up destruction, where the bottom of the stack is resumed and starts to unwind its stack, which destroys a coroutine awaiter, which destroys its handle, so the next stack level is resumed for unwinding, etc. The problem with this approach is that the top-most awaiter may be waiting on an async operation, and there's an inherent race between bottom-up destruction and an async operation resuming the top-most coroutine at the same time.

Initially actors tried to support what I'd call a top-down destruction, where a suspended handle is destroyed without resuming, awaiters and a promise detect that and destroy their continuation handles, which eventually passes it down to the bottom of the stack. To coroutines this looks like a hidden exception that cannot be caught, and starts unwinding at the top. The destruction order turns out to be wrong however. The parent coroutine's frame is destroyed before nested coroutines finish destruction, which necessitates coroutine frames to be allocated on the heap. The logic supporting that is also very complicated, so I finally dropped the idea. Besides, when async operations can destroy a handle, they can resume with an error instead, so supporting top-down destruction doesn't seem to be necessary in practice, even for cancellation.

In theory this also allows heap allocation elision when awaiting actors, however in practice that doesn't work very well, since their context-related logic is very complex.

## Cancellation support

For cancellation actors use c++20 standard `std::stop_token`. Unfortunately in 2023 major stable operating systems (Ubuntu 22.04 and MacOS 13.5) don't ship with compilers implementing `<stop_token>`, so there's a polyfill implementation. In any case classes like `coroactors::stop_token` either point to either `std::stop_token` or a polyfill implementation when standard classes are unavailable.

It's not clear what is the best way to pass stop tokens implicitly between coroutines. Libraries like folly opt into an ADL customization function, but it's very heavyweight, hard to use in custom wrapper awaiters, and the direction where awaiting entities call extra functions on awaiters feels wrong. Then std::executors propose a get_stop_token customization point via tag_invoke, which seems great when senders have the type of the receiver. But unfortunately it's not true for coroutines: you could maybe inspect a promise in `await_suspend`, but it bypasses (non-coroutine) wrappers, could be type erased (intentionally hidden in actors where it's wrapped for context switching) and by the time awaiting coroutine is suspending it may already be too late (a lot of logic may be happening in `await_ready`, and awaiter may not want to suspend when a token is already requested to stop).

Actors control their frame activation and deactivation, and this is used as a basis for arbitrary `coroutine_local` values, with a special slot for current stop token. Coroutine local values are inherited in actor coroutines when awaited by callers (technically in their `await_ready`), removed from thread locals whenever actor coroutine suspends and restored into thread locals whenever actor coroutine resumes. This makes these coroutine local values available in normal functions calls before async operations are started.

To start some activity with a stop token you'd use `coroactors::with_stop_token` to wrap an awaitable (which must support stop token propagation by inheriting it in `await_ready`) into a special awaiter that propagates the specified stop token. The caller's stop token will be ignored when this wrapped awaiter is `co_await`ed, so it can also be used to protect against unwanted cancellation.

## Structured concurrency

Having transparent cancellation enables structured concurrency, where actor may start multiple concurrent activities using a `coroactors::task_group<T>`, wait for all or some results, and have those activities cancelled when they no longer have any chance of being awaited. Instead of using task group directly however, it is recommented to use `coroactors::with_task_group` (again heavily inspired by Swift), which awaits on a lambda result (which is supposed to add tasks to the group and await one or all of them), then cancels the task group, but also awaits all leftover tasks in the group, ignoring their results. This guarantees all unwanted tasks are actually cancelled and there are no runaway resource leaks. The advantage of `with_task_group` is also inheritance of coroutine local values by child tasks.

Example:

```c++
struct Request;
struct Response;
async<Response> make_shard_request(int shard, const Request& request);

class FastService {
public:
    // ...

    struct FastResponse {
        int shard;
        Response response;
    };

    async<FastResponse> make_request(const Request& request) {
        co_await context();

        // The template parameter specifies type of a single task result
        // The co_await will return whatever result the lambda returns
        co_return co_await with_task_group<Response>(
            [&](task_group<Response>& group) -> async<FastResponse> {
                // We are guaranteed to run in the same context as the caller
                // All actor functions must co_await context, so we do just that
                co_await actor_context::caller_context();

                // Spawn one task for each shard. Note: because of structured
                // concurrency we are guaranteed not to return until all tasks
                // are complete, so passing a const reference to the same
                // object is safe.
                for (int shard : shards) {
                    group.add(make_shard_request(shard, request));
                }

                // Wait for the first packaged result, error handling omitted
                auto result = co_await group.next_result();
                assert(!result.has_exception());

                // Every task gets a sequential index starting from zero
                // We can use it to find which shard replied first
                // When we return all other tasks are cancelled
                co_return FastResponse{
                    shards.at(result.index),
                    result.take_value(),
                };
            });
    }

private:
    const actor_context context;
    const std::vector<int> shards;
}
```

## Sleeping and timeouts

Practical cancellation often arises because asynchronous code needs to timeout, e.g. so outbound requests doesn't take an unbounded time. Many forms of retry loops also need to sleep to implement exponential backoffs. While it is possible to do manually using stop tokens it would be way too cumbersome, but sleeping is impossible without a scheduler, and timer wrappers on an `actor_context` instance may be when scheduler implements a timed `schedule` call. Wrapper methods `sleep_until` and `sleep_for` return at the specified deadline, or when current stop token is cancelled, whatever comes first (these methods return true on deadline and false on cancellation when `co_await`ed). Wrapper methods `with_deadline` and `with_timeout` wrap another awaitable (which must support cancellation propagation), and cancel a forked stop token on deadline. Besides additional cancellation there are no other effects on the awaitable, it is awaited when the wrapper is awaited.

## Benchmarks

Benchmarks show the performance of actors is not exactly bad, but unfortunately not great either. The `benchmark_actor_latency` benchmark starts a number of `Pinger` actors, each calling a `ping` method in a loop on a `Pingable` actor, which increments a counter in its shared state and returns the next value. The number of actors, number of threads, and the total number of calls are configurable using the command line. Benchmark tracks the overall throughput, as well as the maximum latency of each call, which mostly depends on how well the scheduler is able to preempt running actors and prevent them from monopilizing the thread for a long time.

A single pair of actors with a single thread scheduler gets me around 10 million rps with ~1-10ms max latency on my macbook m1 air (the latency figure is not stable between runs and most likely depends on other activity on my laptop at the same time). When 8 threads run 2048 pairs of actors I get ~25 million rps with my custom scheduler and up to ~35 million rps using `asio::thread_pool` (mostly due to asio implementing `defer` without using a global shared queue). However multiple pairs of actors is not a very interesting test case, because those actors have absolutely no contention, and it becomes mostly about actor context switching and scheduling overhead.

A more interesting test case is when multiple actors are calling `ping` on the same `Pingable` instance, using multiple threads per scheduler, because then multiple actors try to execute at the same time in different threads, and conflict over the same shared actor which can only run in a single thread at a time. With 8 threads and my custom scheduler I get ~3 million rps for 2 `Pinger`s, then ~2.3 million for 3 `Pinger`s, then ~2.8 million for 4 `Pinger`s and it stays mostly the same afterwards. Since scheduling overhead here is very high, best performance is when using a single global queue protected with `absl::Mutex`, ~4.8 million rps mostly independent of the number of `Pinger`s.

### Comparison to Swift

The same benchmark is also implemented in Swift, which shows interesting behavior. First of all Swift actors show throughput of ~3 million rps even when single threaded (using `LIBDISPATCH_COOPERATIVE_POOL_STRICT=1` environment variable). There's no way to control the number of threads (it appears to always use up to 8 threads, which is the number of cores). When running many non-conflicting actors throughput goes up to ~16 million rps with a large number of pairs and basically stays there. However, max latency also goes way up: ~8ms for 8 pairs, then ~350ms for 16 pairs and 10 million requests, then ~640ms for 16 pairs and 20 million requests, and so on. This shows that either actors are not preempted in any way, or the preemption interval is very large, and a single actor calling non-blocking async methods in a loop can monopolize threads basically forever.

Conflicting on the same `Pingable` instance is much worse however. While 2 `Pinger`s still get ~3 million rps, 4 `Pinger`s go down to ~1 million rps, then ~750k rps for 128 `Pinger`s, ~670k rps for 256, ~630k for 512, ~320k for 1024, ~110k for 2048, etc. I'm not sure what's going on, but it doesn't look good. Turns out swift actors have a linear preprocessing time based on queue length, see [swift#68299](https://github.com/apple/swift/issues/68299).

### Comparison to Go

Now in the Go benchmark it's only logical to isolate state with a mutex (remember, it's not about message passing for message passing sake, it's about isolating shared state), and single-threaded (forced by setting `runtime.GOMAXPROCS`, the `-t` option defaults to `1` on the command line, same as for other versions) single pair gives ~17 million rps (and it's actually the same for a single pair with 8 threads, or thousands of pairs with 1 thread). For the case of 8 threads and thousands of pairs the throughput is almost **100 million rps**, seems to scale almost linearly here. The latency can Go quite high though, probably because Go scheduler preempts every 10ms, and for thousands of running goroutines that adds up. When a single `Pingable` is contended Go is the winner here: ~18 million rps for 2 pingers, ~12 million for 4, ~10 million for 64, ~9.6 million for 128, ~9.5 million for 1024, ~9.2 million for 8192.

### Comparison to Java

Java benchmark uses virtual threads, and just like Go the shared state is protected with a lock (which integrate with virtual threads as far as I know). For a single pair it gives ~45 million rps on my m1 air, for 2 pairs it drop to ~19 million rps, and more pairs don't effect the resulting rps, only max latency. Interestingly with 4 pairs calling ping on a single instance it's 17 million rps and pretty much stays there up to many thousands of pingers. It looks like virtual threads are not preempted in any way, so to get decent latencies you need to call `Thread.yield()` periodically, which decreases throughput.

This however may not be a fair comparison, since the ping method takes lock for a very little time, and threads have very little chance to actually conflict (they spin a little when trying to acquire a lock). To test scheduling overhead I tried inserting a `Thread.yield()` inside the lock, and performance drops to ~1 million rps even for a single pair (because yield always reschedules current thread to a global queue), and drops with more pairs, which is not fair either.

### Benchmarks summary

Overall performance of coroutine actors is not too bad, but I would have expected a lot better from optimized C++ code. For reference there's a `benchmark_actor` program that benchmarks various function calls, and it shows ~15 million rps for an actor repeatedly calling another uncontended actor, but it's ~115 million rps for a method call isolated with `std::mutex` and ~120 million rps for a method call isolated with `absl::Mutex`, all single threaded, that's almost 8 times slower when replacing normal function calls. However when calling a method that is not protected by a mutex (most of the code people write, really), it's actually ~1 billion rps, and the same function using actors (returns a constant value without `co_await`ing a context) is now ~25 times slower!

I'm not sure if these overheads would matter in practice, e.g. when functions actually do useful work instead of just incrementing a counter, but in microbenchmarks it looks like Go has lower overheads and better scales, especially in uncontended cases.

## Practical concerns

Unfortunately using these actor coroutines is not very practical at the moment.

There are multiple known bugs in coroutine implementations, one glaring example is the need to mark `await_suspend` methods with `__attribute__((__noinline__))` to prevent local variables spilling into a suspended coroutine frame, which may concurrently resume in another thread. This hurts performance ever worse, because coroutines will not be optimized in many ways when anything is not inlinable on their execution path.

Symmetric transfer is absolutely critical for fast coroutine switching without unbounded stack growth, and it seems to work well with modern versions of clang. Unfortunately it causes unexpected stack explosions with gcc, just because you compiled your code a little differently (like without `-O2`, or with a sanitizer). It's not safe to use code that can randomly blow up, just because it's friday and your compiler didn't feel like optimizing.

For this reason (and also for Windows support) coroactors don't use compiler support for symmetric transfer by default, and instead emulate it using a TLS variable resumed in a loop. This may be prone to errors however, and causes a performance hit in at least some microbenchmarks.

And then every actor call has to allocate memory from the heap, because it cannot be elided due to pointers escaping to the scheduler, etc. There's a `task<T>` class that is a lot simpler and elides heap allocations, but it doesn't work in loops, and sometimes it's actually worse: suppose your `task<T>` coroutine calls 10 other `task<T>` methods that perfectly inline, etc. Unfortunately your initial `task<T>` will now allocate 10 times the heap space needed for a single call, because apparently clang cannot reuse memory between different invocations. :-/

Coroutines still feel very immature, and it all feels like trying to solve your normal function calls, before you could even start solving complex concurrency related problems.

## Windows support

Actors compile on Windows, but don't work without symmetric transfer emulation (tested with Visual Studio 2022 so far), because many wrappers in coroactors destroy coroutine handles in their `final_suspend` (one way or another). It appears when `await_suspend` returns `std::coroutine_handle<>` stack is already unwound when entering `final_suspend`, but when destroy is called it unwinds it again (often causing a double free), likely because coroutines are not fully suspended before the symmetric transfer variant of `await_suspend` is called. Symmetric transfer emulation appears to work well however.
