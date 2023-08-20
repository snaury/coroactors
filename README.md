# coroactors
Experimental actors with C++ coroutines

## Preface

I've been working on [YDB](https://ydb.tech) (a recently open-sourced distributed SQL database) for many years, and got a bit of experience in programming with actors as a result. Actor model is great at isolating mutable state, communicating using messaging kinda matches the real world, and having messages serializable makes communicating across the network almost identical to a single process. In theory that makes systems scalable, you just spread your actors across different nodes.

In practice, however, there are many drawbacks. First is the mental model, you have to invert your (often serial) business logic into a state machine, and it's very easy to introduce subtle bugs. Second, communicating with messages is very expensive, compared to a normal method call it's one or two orders of magnitude more expensive. Those two points lead to horrible trade offs: to make state machines smaller and understandable you need to use more actors, but the more actors you have the more communucation you have. And just because your actors don't have shared mutable state doesn't mean the actor system doesn't. Registering short lived actors, dispatching events to a global run queue, all of those become a bottleneck.

C++ has coroutines, and just like regular functions its local state is not shared by default. When coroutine suspends it may resume on another thread, but its body doesn't run in parallel, and you don't need mutexes to work with local variables. They are great for matching the mental model, you don't need to create a state machine every time (compiler does that for you). But just like with threads and regular functions, shared state needs isolation, and mutexes with coroutines would be even more error-prone than with regular functions.

[Swift actors](https://docs.swift.org/swift-book/documentation/the-swift-programming-language/concurrency/#Actors) show us there's a way to have both: asynchronous, maintainable and easy to understand methods instead of state machines, and isolated mutable state without using locks. What's more important actors in Swift isolate state to actor methods by default, so making a long running async fetch in a caching service won't stop reads from running concurrently.

This repository takes ideas from the Swift language and standard library, and tries to mesh them with C++ coroutines hoping to achieve high-performance and low-latency actors that are easy to code.

## Actors with C++ coroutines

The primary class here is `actor<T>` that should be used as the return type for actor coroutines. This would be your standard initially suspended lazy coroutine that only starts on `co_await`. However, when actor coroutine `co_await`s on an `actor_context` object it binds to that context and will never execute code in parallel with other coroutines bound to the same context. Effectively actor context acts like a localized mutex, and allows sharing state between different coroutines, for example:

```c++
class Counter {
public:
    // ...

    actor<int> get() const {
        co_await context;
        co_return value_;
    }

    actor<void> set(int value) {
        co_await context;
        value_ = value;
    }

    actor<int> increment() {
        co_await context;
        co_return ++value_;
    }

private:
    actor_context context;
    int value_ = 0;
};
```

Multiple coroutines may be calling `Counter` methods concurrently, but all accesses to the `value_` member variables will be serialized automatically.

Unlike a mutex however, context is also automatically released when actor coroutine `co_await`s on various other async activities. For example:

```c++
// Note: not an actor coroutine
task<std::string> fetch_url(const std::string& url);

class CachingService {
public:
    // ...

    actor<std::string> get(const std::string& url) {
        co_await context;
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

Multiple coroutines may be calling `CachingService::get`, getting cached results most of the time. However a very long fetch of a particular url will not block other coroutines from getting results that are already cached.
