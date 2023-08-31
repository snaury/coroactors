#include <coroactors/actor.h>
#include <coroactors/asio_actor_scheduler.h>
#include <coroactors/asio_awaitable.h>
#include <coroactors/detach_awaitable.h>
#include <asio/thread_pool.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <iostream>

using namespace coroactors;

using asio::ip::tcp;
using asio::any_io_executor;

using default_token = asio_awaitable_t<>;
using tcp_socket = default_token::as_default_on_t<tcp::socket>;
using tcp_acceptor = default_token::as_default_on_t<tcp::acceptor>;

actor<void> serve_client(actor_scheduler& scheduler, tcp_socket socket) {
    actor_context context(scheduler);
    co_await context();

    try {
        char data[1024];
        for (;;) {
            size_t n = co_await context.with_timeout(
                std::chrono::seconds(5),
                socket.async_read_some(asio::buffer(data)));
            co_await asio::async_write(socket, asio::buffer(data, n));
        }
    } catch (std::exception& e) {
        std::cout << "ERROR: " << e.what() << std::endl;
    }
}

actor<void> listener(actor_scheduler& scheduler, any_io_executor executor) {
    actor_context context(scheduler);
    co_await context();

    tcp_acceptor acceptor(executor, {tcp::v4(), 54321});
    for (;;) {
        std::cout << "Waiting for a client..." << std::endl;
        auto socket = co_await acceptor.async_accept();
        std::cout << "Client connection from " << socket.remote_endpoint() << std::endl;
        detach_awaitable(serve_client(scheduler, std::move(socket)));
    }
}

int main() {
    asio::thread_pool pool(4);
    asio_actor_scheduler scheduler(pool.executor());

    stop_source source;
    detach_awaitable(
        with_stop_token(source.get_token(),
            listener(scheduler, pool.executor())));

    pool.join();
    return 0;
}
