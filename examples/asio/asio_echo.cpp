#include <coroactors/actor.h>
#include <coroactors/asio_actor_scheduler.h>
#include <coroactors/asio_awaitable.h>
#include <coroactors/detach_awaitable.h>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <iostream>

using namespace coroactors;

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::any_io_executor;

actor<void> serve_client(actor_scheduler& scheduler, tcp::socket socket) {
    actor_context context(scheduler);
    co_await context();

    try {
        char data[1024];
        for (;;) {
            size_t n = co_await context.with_timeout(
                std::chrono::seconds(5),
                socket.async_read_some(asio::buffer(data), asio_awaitable));
            co_await asio::async_write(socket, asio::buffer(data, n), asio_awaitable);
        }
    } catch (std::exception& e) {
        std::cout << "ERROR: " << e.what() << std::endl;
    }
}

actor<void> listener(actor_scheduler& scheduler, any_io_executor executor) {
    actor_context context(scheduler);
    co_await context();

    tcp::acceptor acceptor(executor, {tcp::v4(), 54321});
    for (;;) {
        std::cout << "Waiting for a client..." << std::endl;
        tcp::socket socket = co_await acceptor.async_accept(asio_awaitable);
        detach_awaitable(serve_client(scheduler, std::move(socket)));
    }
}

int main() {
    boost::asio::thread_pool pool(4);
    asio_actor_scheduler scheduler(pool.executor());

    stop_source source;
    detach_awaitable(
        with_stop_token(source.get_token(),
            listener(scheduler, pool.executor())));

    pool.join();
    return 0;
}
