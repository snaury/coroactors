#include <coroactors/actor.h>
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

class RealTimeService {
public:
    RealTimeService(actor_scheduler& scheduler, std::shared_ptr<SlowService> service)
        : context(scheduler)
        , service(std::move(service))
    {}

    actor<SlowService::Response> make_request(const SlowService::Request& request) {
        co_await context();
        co_return co_await context.with_timeout(
            std::chrono::milliseconds(500),
            service->make_request(request));
    }

private:
    const actor_context context;
    const std::shared_ptr<SlowService> service;
};
