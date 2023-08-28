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

The primary class is `actor<T>` and should be used as the return type for actor coroutines. This is an eagerly started coroutine that must either return immediately or `co_await` on `actor_context::operator()` call, at which point it binds to the specified context and suspends until `co_await`ed. When bound to a context actor will never execute in parallel with other actor coroutines bound to the same context, effectively acting like a local mutex, guaranteeing exclusive access to shared state protected by this context.

```c++
class Counter {
public:
    // ...

    actor<int> get() const {
        co_await context();
        co_return value_;
    }

    actor<void> set(int value) {
        co_await context();
        value_ = value;
    }

    actor<int> increment() {
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

    actor<std::string> get(const std::string& url) {
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

The second common case would be for calls to an uncontended service, and since it's uncontended it should also use direct frame transfer when possible, switching to a new available context, releasing the old context (maybe rescheduling it when there is more work), and doing the same on the same on the way back. There is some synchronization with actor mailboxes, but otherwise scheduler is not involved.

When there is contention between actors (target mailbox is locked by another thread, or the source mailbox is not empty) we have to involve scheduler for one side or the other. Actors prefer switching to the new context when possible, rescheduling old context when it is not empty, since caches (e.g. argument or the result) are more likely to stay warm without interleaving with other work and especially switching threads. This is probably key for C++ coroutines here, as opposed to classical actors, because unlike message sending we know that frames are changing and parent frame is suspending.

## Coroutine handle destruction

Coroutine handle destruction in C++ is tricky. It seems to be common to have coroutine awaiters in the wild that unconditionally destroy handles in destructor (e.g folly, stdexec). This works for bottom-up destruction, where a scheduler holds on to "root" coroutine handles, destroys them on exit, and they in turn destroy nested frames via unfinished awaiters. There is a problem with this approach however, you never know what innermost coroutine is awaiting on, how well does it support unexpected cancellation, and this makes races between destroy and resume possible (and even impossible to avoid), violating an invariant that there is only one way to resume a suspended coroutine (which would be via their continuation from await_suspend).

Actors support top-down destruction instead. When some service is holding on to a continuation (coroutine handle), but cannot resume it for some reason (e.g. there is no way to generate an error), it can be destroyed instead. When that happens the stack of the innermost coroutine is unwound, and its promise type (e.g. `actor_promise<T>`) is destroyed. Promise detects when destructor is called with an active continuation (before coroutine finished), and also destroy it, which recursively unwinds the stack level above. Actor awaiter detects its destruction before resuming, and instead of destroying the awaited handle will try to unset its continuation instead (effectively detaching from the nested coroutine), which kinda supports both top-down and bottom-up frame cleanup. Eventually this will reach the bottom frame, destroying the full chain of awaiting frames.

Unfortunate downside of this support is compilers having a hard time figuring out if nested coroutine frame is guaranteed to be destroyed before the parent, and turn off coroutine allocation elision as a result. This doesn't seem to really be a problem for actors though, since their context-related logic is already complex.

## Cancellation support

For cancellation actors use c++20 standard `std::stop_token`. Unfortunately in 2023 major stable operating systems (Ubuntu 22.04 and MacOS 13.5) don't ship with compilers implementing `<stop_token>`, so there's a polyfill implementation. In any case classes like `coroactors::stop_token` either point to either `std::stop_token` or a polyfill implementation when standard classes are unavailable.

It's not clear what is the best way to pass stop tokens implicitly between coroutines. Libraries like folly opt into an ADL customization function, but it's very heavyweight, hard to use in custom wrapper awaiters, and the direction where awaiting entities call extra functions on awaiters feels wrong. Then std::executors propose a get_stop_token customization point via tag_invoke, which seems great when senders have the type of the receiver. But unfortunately it's not true for coroutines: you could maybe inspect a promise in `await_suspend`, but it bypasses (non-coroutine) wrappers, could be type erased (intentionally hidden in actors where it's wrapped for context switching) and by the time awaiting coroutine is suspending it may already be too late (a lot of logic may be happening in `await_ready`).

For the time being I opted for a non-standard extension to awaiters where `await_ready` method optionally accepts a `const coroactors::stop_token&` argument, which is used for transparent stop token passing in all coroactors awaiters and coroutines. Ideally this should maybe be an abstract object (similar to a receiver in stdexec) which signifies an awaiting entity and may be transparently queried for various properties, including a stop token. Having the token available in `await_ready` is also more efficient, as you don't have to save it using some separate function/method and don't have to deal with the possibility of multiple conflicting calls.

Finally to start some activity with a stop token you'd use `coroactors::with_stop_token` to wrap an awaitable (which must support stop token propagation) into a special awaiter that propagates the specified stop token. The caller's stop token will be ignored when this wrapped awaiter is `co_await`ed, so it can also be used to protect against unwanted cancellation.

## Structured concurrency

Having transparent cancellation enables structured concurrency, where actor may start multiple concurrent activities using a `coroactors::task_group<T>`, wait for all or some results, and have those activities cancelled when they no longer have any chance of being awaited. Instead of using task group directly however, it is recommented to use `coroactors::with_task_group` (again heavily inspired by Swift), which awaits on a lambda result, then cancels the task group, but also awaits all leftover tasks in the group, ignoring their results. This guarantees all unwanted tasks are actually cancelled and there are no runaway resource leaks.

Example:

```c++
struct Request;
struct Response;
actor<Response> make_shard_request(int shard, const Request& request);

class FastService {
public:
    // ...

    struct FastResponse {
        int shard;
        Response response;
    };

    actor<FastResponse> make_request(const Request& request) {
        co_await context();

        // The template parameter specifies type of a single task result
        // The co_await will return whatever result the lambda returns
        co_return co_await with_task_group<Response>(
            [&](const task_group<Response>& group) -> actor<FastResponse> {
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
