#include <coroactors/actor.h>
#include <coroactors/with_task_group.h>
#include <memory>

using namespace coroactors;

class SlowService {
public:
    struct Request {
        // ...
    };

    struct Response {
        // ...
    };

    virtual actor<Response> make_request(const Request& request) = 0;
};

class FastService {
public:
    using Request = SlowService::Request;

    struct Response {
        // Indexes in the services list
        size_t index;
        // Response from the given service
        SlowService::Response response;
    };

    FastService(actor_scheduler& scheduler, std::vector<std::shared_ptr<SlowService>> services)
        : context(scheduler)
        , services(std::move(services))
    {}

    actor<Response> make_request(const Request& request) {
        co_await context();

        co_return co_await with_task_group<SlowService::Response>(
            [&](task_group<SlowService::Response>& group) -> actor<Response> {
                co_await actor_context::caller_context();

                for (const auto& service : services) {
                    group.add(service->make_request(request));
                }

                auto result = co_await group.next_result();
                assert(!result.has_exception());

                co_return Response{
                    result.index,
                    result.take_value(),
                };
            });
    }

private:
    const actor_context context;
    const std::vector<std::shared_ptr<SlowService>> services;
};
