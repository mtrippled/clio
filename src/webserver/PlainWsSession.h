#ifndef RIPPLE_REPORTING_WS_SESSION_H
#define RIPPLE_REPORTING_WS_SESSION_H

#include <boost/asio/dispatch.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <etl/ReportingETL.h>
#include <rpc/RPC.h>
#include <webserver/Listener.h>
#include <webserver/WsBase.h>

#include <iostream>

namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

class ReportingETL;

// Echoes back all received WebSocket messages
class PlainWsSession : public WsSession<PlainWsSession>
{
    websocket::stream<boost::beast::tcp_stream> ws_;

public:
    // Take ownership of the socket
    explicit PlainWsSession(
        boost::asio::ip::tcp::socket&& socket,
        std::shared_ptr<BackendInterface const> backend,
        std::shared_ptr<SubscriptionManager> subscriptions,
        std::shared_ptr<ETLLoadBalancer> balancer,
        DOSGuard& dosGuard,
        RPC::Counters& counters,
        boost::beast::flat_buffer&& buffer)
        : WsSession(
              backend,
              subscriptions,
              balancer,
              dosGuard,
              counters,
              std::move(buffer))
        , ws_(std::move(socket))
    {
    }

    websocket::stream<boost::beast::tcp_stream>&
    ws()
    {
        return ws_;
    }

    std::string
    ip()
    {
        return ws()
            .next_layer()
            .socket()
            .remote_endpoint()
            .address()
            .to_string();
    }

    ~PlainWsSession() = default;
};

class WsUpgrader : public std::enable_shared_from_this<WsUpgrader>
{
    boost::beast::tcp_stream http_;
    boost::optional<http::request_parser<http::string_body>> parser_;
    boost::beast::flat_buffer buffer_;
    std::shared_ptr<BackendInterface const> backend_;
    std::shared_ptr<SubscriptionManager> subscriptions_;
    std::shared_ptr<ETLLoadBalancer> balancer_;
    DOSGuard& dosGuard_;
    RPC::Counters& counters_;
    http::request<http::string_body> req_;

public:
    WsUpgrader(
        boost::asio::ip::tcp::socket&& socket,
        std::shared_ptr<BackendInterface const> backend,
        std::shared_ptr<SubscriptionManager> subscriptions,
        std::shared_ptr<ETLLoadBalancer> balancer,
        DOSGuard& dosGuard,
        RPC::Counters& counters,
        boost::beast::flat_buffer&& b)
        : http_(std::move(socket))
        , backend_(backend)
        , subscriptions_(subscriptions)
        , balancer_(balancer)
        , dosGuard_(dosGuard)
        , counters_(counters)
        , buffer_(std::move(b))
    {
    }
    WsUpgrader(
        boost::beast::tcp_stream&& stream,
        std::shared_ptr<BackendInterface const> backend,
        std::shared_ptr<SubscriptionManager> subscriptions,
        std::shared_ptr<ETLLoadBalancer> balancer,
        DOSGuard& dosGuard,
        RPC::Counters& counters,
        boost::beast::flat_buffer&& b,
        http::request<http::string_body> req)
        : http_(std::move(stream))
        , backend_(backend)
        , subscriptions_(subscriptions)
        , balancer_(balancer)
        , dosGuard_(dosGuard)
        , counters_(counters)
        , buffer_(std::move(b))
        , req_(std::move(req))
    {
    }

    void
    run()
    {
        // We need to be executing within a strand to perform async operations
        // on the I/O objects in this session. Although not strictly necessary
        // for single-threaded contexts, this example code is written to be
        // thread-safe by default.

        net::dispatch(
            http_.get_executor(),
            boost::beast::bind_front_handler(
                &WsUpgrader::do_upgrade, shared_from_this()));
    }

private:
    void
    do_upgrade()
    {
        parser_.emplace();

        // Apply a reasonable limit to the allowed size
        // of the body in bytes to prevent abuse.
        parser_->body_limit(10000);

        // Set the timeout.
        boost::beast::get_lowest_layer(http_).expires_after(
            std::chrono::seconds(30));

        on_upgrade();
    }

    void
    on_upgrade()
    {
        // See if it is a WebSocket Upgrade
        if (!websocket::is_upgrade(req_))
            return;

        // Disable the timeout.
        // The websocket::stream uses its own timeout settings.
        boost::beast::get_lowest_layer(http_).expires_never();

        std::make_shared<PlainWsSession>(
            http_.release_socket(),
            backend_,
            subscriptions_,
            balancer_,
            dosGuard_,
            counters_,
            std::move(buffer_))
            ->run(std::move(req_));
    }
};

#endif  // RIPPLE_REPORTING_WS_SESSION_H
