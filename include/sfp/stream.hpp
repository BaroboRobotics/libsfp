// Copyright (c) 2014-2016 Barobo, Inc.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef LIBSFP_STREAM_HPP
#define LIBSFP_STREAM_HPP

#include <composed/op.hpp>
#include <composed/timed.hpp>
#include <composed/phaser.hpp>

#include <sfp/serial_framing_protocol.h>

#include <beast/core/consuming_buffers.hpp>
#include <beast/core/handler_alloc.hpp>
#include <beast/core/streambuf.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <utility>

#include <boost/asio/yield.hpp>

namespace sfp {

static const std::chrono::milliseconds kSfpConnectTimeout { 100 } ;
static const std::chrono::milliseconds kSfpSettleTimeout { 200 } ;
static const int kSfpMaxHandshakeAttempts { 50 };

template <class AsyncStream, class Alloc = std::allocator<char>>
class stream {
    AsyncStream next_layer_;
    boost::asio::io_service::strand write_strand;
    composed::phaser<boost::asio::io_service::strand&> write_phaser;

    beast::basic_streambuf<Alloc> read_buffer;
    beast::basic_streambuf<Alloc> write_buffer;

    beast::consuming_buffers<decltype(read_buffer.data())> input_sequence;
    // View into read_buffer, if there are bytes that have been read but not yet SFP-processed.

    SFPcontext sfp_context;
    SFPpacket sfp_packet;

public:
    template <class... Args>
    stream(Alloc alloc, Args&&... args)
        : next_layer_(std::forward<Args>(args)...)
        , write_strand(next_layer_.get_io_service())
        , write_phaser(write_strand)
        , read_buffer(alloc)
        , write_buffer(alloc)
    {}

#ifdef SFP_CONFIG_DEBUG
    void set_debug_name(std::string debugName) {
        sfpSetDebugName(&sfp_context, debugName.c_str());
    }
#endif

    boost::asio::io_service& get_io_service() { return next_layer_.get_io_service(); }

    AsyncStream& next_layer() { return next_layer_; }
    const AsyncStream& next_layer() const { return next_layer_; }

private:
    template <class Handler = void(boost::system::error_code)>
    struct handshake_op;

    template <class DynamicBuffer, class Handler = void(boost::system::error_code, size_t)>
    struct read_op;

    template <class ConstBufferSequence, class Handler = void(boost::system::error_code)>
    struct write_op;

public:
    template <class Token>
    auto async_handshake(Token&& token) {
        return composed::operation<handshake_op<>>{}(*this, std::forward<Token>(token));
    }

    template <class DynamicBuffer, class Token>
    auto async_read(DynamicBuffer& buffer, Token&& token) {
        // Read one message into the provided buffer.
        BOOST_ASSERT(sfpIsConnected(&self.sfp_context), "SFP handshake not yet completed");
        return composed::operation<read_op<DynamicBuffer>>{}(*this, buffer, std::forward<Token>(token));
    }

    template <class ConstBufferSequence, class Token>
    auto async_write(const ConstBufferSequence& buffer, Token&& token) {
        BOOST_ASSERT(sfpIsConnected(&self.sfp_context), "SFP handshake not yet completed");
        return composed::operation<write_op<ConstBufferSequence>>{}(*this, buffer, std::forward<Token>(token));
    }

private:
    template <class Handler = void(boost::system::error_code)>
    struct read_until_connected_op;

    template <class Handler = void(boost::system::error_code)>
    struct read_until_delivery_op;

    template <class Token>
    auto async_read_until_connected(Token&& token) {
        return composed::operation<read_until_connected_op<>>{}(*this, std::forward<Token>(token));
    }

    template <class Token>
    auto async_read_until_delivery(Token&& token) {
        return composed::operation<read_until_delivery_op<>>{}(*this, std::forward<Token>(token));
    }

    int on_sfp_write(uint8_t* octets, size_t len, size_t* outlen) {
        auto n = boost::asio::buffer_copy(
                write_buffer.prepare(len), boost::asio::buffer(octets, len));
        write_buffer.commit(n);
        if (outlen) { *outlen = n; }
        return 0;
    }

    void deliver_input_sequence() {
        sfp_packet.len = 0;
        for (auto octet: input_sequence) {
            auto& pkt = sfp_packet;
            auto rc = sfpDeliverOctet(
                    &sfp_context, octet, pkt.buf, sizeof(pkt.buf), &pkt.len);
            BOOST_ASSERT(rc != -1);
            // Ignore zero-length messages. This means we can rely on sfp_packet.len being non-zero
            // to indicate the presence of a message.
            if (rc == 1 && pkt.len != 0) {
                break;
            }
        }
    }
};

template <class AsyncStream, class Alloc>
template <class Handler>
struct stream<AsyncStream, Alloc>::handshake_op: boost::asio::coroutine {
    using handler_type = Handler;
    using allocator_type = beast::handler_alloc<char, handler_type>;
    using executor_type = composed::handler_executor<handler_type>;

    using logger_type = composed::logger;
    logger_type get_logger() const { return &lg; }

    stream& self;

    composed::associated_logger_t<handler_type> lg;
    boost::system::error_code ec;
    boost::system::error_code settle_ec;

    handshake_op(handler_type& h, stream& s)
        : self(s)
        , lg(composed::get_associated_logger(h))
    {}

    void operator()(composed::op<handshake_op>&);
};

template <class AsyncStream, class Alloc>
template <class Handler>
void stream<AsyncStream, Alloc>::handshake_op<Handler>::
operator()(composed::op<handshake_op>& op) {
    if (!ec) reenter(this) {
        sfpInit(&self.sfp_context);
        sfpSetWriteCallback(&self.sfp_context, SFP_WRITE_MULTIPLE,
            (void*)+[](uint8_t* octets, size_t len, size_t* outlen, void* data) {
                static_cast<stream*>(data)->on_sfp_write(octets, len, outlen);
            }, &self);
        sfpConnect(&self.sfp_context);
        yield return boost::asio::async_write(self.next_layer_, self.write_buffer.data(), op(ec, std::ignore));
        self.write_buffer.consume(self.write_buffer.size());

        if (!sfpIsConnected(&self.sfp_context)) {
            yield return self.async_read_until_connected(composed::timed(
                    self.next_layer_, kSfpConnectTimeout, op(ec));
        }

        // If async_read_until_connected() ended with us receiving a message (non-zero
        // sfp_packet.len), we are guaranteed to be on the same page as the remote. If the
        // handshake ended with no message, we need to keep on reading for a bit, to make sure
        // the remote is on the same page. Think of this as de-bouncing the SFP connection
        // state.

        if (self.sfp_packet.len == 0) {
            yield return self.async_read_until_delivery(composed::timed(
                    self.next_layer_, kSfpSettleTimeout, op(settle_ec));
            ec = settle_ec == boost::asio::error::timed_out && sfpIsConnected(&self.sfp_context)
                    ? boost::system::error_code{}
                    : settle_ec;
            BOOST_ASSERT(sfpIsConnected(&self.sfp_context) || ec);
            // This assertion says: async_read_until_delivery cannot have completed successfully
            // without leaving the SFP context in a connected state.

            BOOST_LOG(lg) << "SFP handshake settle period error: " << ec.message();
        }

        if (!ec) {
            BOOST_LOG(lg) << "SFP handshake complete";
        }
    }
    op.complete(ec);
}

template <class AsyncStream, class Alloc>
template <class Handler>
struct stream<AsyncStream, Alloc>::read_until_connected_op: boost::asio::coroutine {
    using handler_type = Handler;
    using allocator_type = beast::handler_alloc<char, handler_type>;
    using executor_type = composed::handler_executor<handler_type>;

    using logger_type = composed::logger;
    logger_type get_logger() const { return &lg; }

    stream& self;
    size_t rx_n = 0;

    composed::associated_logger_t<handler_type> lg;
    boost::system::error_code ec;

    read_until_connected_op(handler_type& h, stream& s)
        : self(s)
        , lg(composed::get_associated_logger(h))
    {}

    void operator()(composed::op<read_until_connected_op>&);
};

template <class AsyncStream, class Alloc>
template <class Handler>
void stream<AsyncStream, Alloc>::read_until_connected_op<Handler>::
operator()(composed::op<read_until_connected_op>& op) {
    if (!ec) reenter(this) {
        while (!sfpIsConnected(&self.sfp_context)) {
            self.read_buffer.consume(self.read_buffer.size());
            yield return self.next_layer_.async_read_some(self.read_buffer.prepare(256), op(ec, rx_n));
            self.read_buffer.commit(rx_n);
            self.input_sequence = {self.read_buffer};
            self.deliver_input_sequence();
            yield return boost::asio::async_write(self.next_layer_, self.write_buffer.data(), op(ec, std::ignore));
            self.write_buffer.consume(self.write_buffer.size());
        }
    }
    op.complete(ec);
}

template <class AsyncStream, class Alloc>
template <class Handler>
struct stream<AsyncStream, Alloc>::read_until_delivery_op: boost::asio::coroutine {
    using handler_type = Handler;
    using allocator_type = beast::handler_alloc<char, handler_type>;
    using executor_type = composed::handler_executor<handler_type>;

    using logger_type = composed::logger;
    logger_type get_logger() const { return &lg; }

    stream& self;
    size_t rx_n = 0;

    composed::associated_logger_t<handler_type> lg;
    boost::system::error_code ec;

    read_until_delivery_op(handler_type& h, stream& s)
        : self(s)
        , lg(composed::get_associated_logger(h))
    {}

    void operator()(composed::op<read_until_delivery_op>&);
};

template <class AsyncStream, class Alloc>
template <class Handler>
void stream<AsyncStream, Alloc>::read_until_delivery_op<Handler>::
operator()(composed::op<read_until_delivery_op>& op) {
    if (!ec) reenter(this) {
        BOOST_ASSERT(self.sfp_packet.len == 0);
        BOOST_ASSERT(self.input_sequence.begin() == self.input_sequence.end());
        while (self.sfp_packet.len == 0) {
            self.read_buffer.consume(self.read_buffer.size());
            yield return self.next_layer_.async_read_some(self.read_buffer.prepare(256), op(ec, rx_n));
            self.read_buffer.commit(rx_n);
            self.input_sequence = {self.read_buffer};
            self.deliver_input_sequence();
            yield return boost::asio::async_write(self.next_layer_, self.write_buffer.data(), op(ec, std::ignore));
            self.write_buffer.consume(self.write_buffer.size());
        }
    }
    op.complete(ec);
}

template <class AsyncStream, class Alloc>
template <class DynamicBuffer, class Handler>
struct stream<AsyncStream, Alloc>::read_op: boost::asio::coroutine {
    using handler_type = Handler;
    using allocator_type = beast::handler_alloc<char, handler_type>;
    using executor_type = composed::handler_executor<handler_type>;

    using logger_type = composed::logger;
    logger_type get_logger() const { return &lg; }

    stream& self;
    DynamicBuffer& dest_buffer;
    composed::work_guard<composed::phaser<boost::asio::io_service::strand&>> work;
    size_t rx_n = 0;

    composed::associated_logger_t<handler_type> lg;
    boost::system::error_code ec;
    boost::system::error_code flush_ec;

    read_op(handler_type& h, stream& s, DynamicBuffer& b)
        : self(s)
        , dest_buffer(b)
        , lg(composed::get_associated_logger(h))
    {}

    void operator()(composed::op<read_op>&);
};

template <class AsyncStream, class Alloc>
template <class DynamicBuffer, class Handler>
void stream<AsyncStream, Alloc>::read_op<DynamicBuffer, Handler>::
operator()(composed::op<read_op>& op) {
    if (!ec) reenter(this) {
        if (self.sfp_packet.len == 0 && self.input_sequence.begin() != self.input_sequence.end()) {
            // Drain the input_sequence buffers if there's anything left in there.
            yield return self.write_phaser.dispatch(op());
            work = composed::make_work_guard(self.write_phaser);
            self.deliver_input_sequence();
            yield return boost::asio::async_write(self.next_layer_, self.write_buffer.data(),
                    op(flush_ec, std::ignore));
            self.write_buffer.consume(self.write_buffer.size());
            work = {};
        }

        while (!flush_ec && self.sfp_packet.len == 0) {
            self.read_buffer.consume(self.read_buffer.size());
            yield return self.next_layer_.async_read_some(self.read_buffer.prepare(256), op(ec, rx_n));
            self.read_buffer.commit(rx_n);
            self.input_sequence = {self.read_buffer};
            yield return self.write_phaser.dispatch(op());
            work = composed::make_work_guard(self.write_phaser);
            self.deliver_input_sequence();
            yield return boost::asio::async_write(self.next_layer_, self.write_buffer.data(),
                    op(flush_ec, std::ignore));
            self.write_buffer.consume(self.write_buffer.size());
            work = {};
        }

        rx_n = boost::asio::buffer_copy(
                dest_buffer.prepare(self.sfp_packet.len),
                boost::asio::buffer(self.sfp_packet.buf, self.sfp_packet.len));
        dest_buffer.commit(rx_n);
        self.sfp_packet.len = 0;
        memset(self.sfp_packet.buf, 0xcd, sizeof(self.sfp_packet.buf));
        // scrub the packet's memory, more for debugability than for security

        if (!ec && flush_ec) {
            ec = flush_ec;
            // Defer propagating any flush error 
        }
    }
    op.complete(ec, rx_n);
}

template <class AsyncStream, class Alloc>
template <class ConstBufferSequence, class Handler>
struct stream<AsyncStream, Alloc>::write_op: boost::asio::coroutine {
    using handler_type = Handler;
    using allocator_type = beast::handler_alloc<char, handler_type>;
    using executor_type = composed::handler_executor<handler_type>;

    using logger_type = composed::logger;
    logger_type get_logger() const { return &lg; }

    stream& self;
    ConstBufferSequence& source_buffer;
    composed::work_guard<composed::phaser<boost::asio::io_service::strand&>> work;

    composed::associated_logger_t<handler_type> lg;
    boost::system::error_code ec;

    write_op(handler_type& h, stream& s, const ConstBufferSequence& b)
        : self(s)
        , source_buffer(b)
        , lg(composed::get_associated_logger(h))
    {}

    void operator()(composed::op<write_op>&);
};

template <class AsyncStream, class Alloc>
template <class ConstBufferSequence, class Handler>
void stream<AsyncStream, Alloc>::write_op<ConstBufferSequence, Handler>::
operator()(composed::op<write_op>& op) {
    if (!ec) reenter(this) {
        yield return self.write_phaser.dispatch(op());
        work = composed::make_work_guard(self.write_phaser);
        sfpWritePacket(&self.sfp_context,
                boost::asio::buffer_cast<const uint8_t*>(source_buffer),
                boost::asio::buffer_size(source_buffer), nullptr);
        yield return boost::asio::async_write(self.next_layer_, self.write_buffer.data(), op(ec, std::ignore));
        self.write_buffer.consume(self.write_buffer.size());
    }
    op.complete(ec);
}

} // namespace sfp

#include <boost/asio/unyield.hpp>

#endif
