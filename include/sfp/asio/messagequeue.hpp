#ifndef LIBSFP_ASIO_MESSAGEQUEUE_HPP
#define LIBSFP_ASIO_MESSAGEQUEUE_HPP

#include <util/asio/asynccompletion.hpp>

#include "sfp/serial_framing_protocol.h"
#include "sfp/system_error.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <boost/optional.hpp>
#include <boost/utility/in_place_factory.hpp>

#include <boost/log/attributes/constant.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sources/record_ostream.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/yield.hpp>

namespace sfp {
namespace asio {

using namespace std::placeholders;

namespace _ {

// Not all stream objects provide compatible APIs. Stream sockets, for
// for instance, require that .shutdown() is called before .close() in order
// to gracefully close the stream, while serial_port has no shutdown function.
// The StreamWrapper class template abstracts these inconsistencies so
// MessageQueueImpl doesn't have to worry about them.
template <class S>
class StreamWrapper {
public:
    using Stream = S;

    StreamWrapper (boost::asio::io_service& ios)
        : mStream(ios)
    {}

    Stream& stream () { return mStream; }
    const Stream& stream () const { return mStream; }

    void close (boost::system::error_code& ec) {
        mStream.close(ec);
    }

private:
    Stream mStream;
};

template <class Protocol, class Service>
class StreamWrapper<boost::asio::basic_stream_socket<Protocol, Service>> {
public:
    using Stream = boost::asio::basic_stream_socket<Protocol, Service>;

    StreamWrapper (boost::asio::io_service& ios)
        : mStream(ios)
    {}

    Stream& stream () { return mStream; }
    const Stream& stream () const { return mStream; }

    void close (boost::system::error_code& ec) {
        boost::system::error_code lEc;
        ec = lEc;
        mStream.shutdown(boost::asio::socket_base::shutdown_both, lEc);
        if (lEc) {
            ec = lEc;
        }
        mStream.close(lEc);
        if (lEc) {
            ec = lEc;
        }
    }

private:
    Stream mStream;
};

} // namespace _

static const std::chrono::milliseconds kSfpConnectTimeout { 100 } ;
static const std::chrono::milliseconds kSfpSettleTimeout { 200 } ;
static const std::chrono::milliseconds kSfpKeepaliveTimeout { 500 };
static const int kSfpMaxHandshakeAttempts { 50 };

typedef void HandshakeHandlerSignature(boost::system::error_code);
typedef void KeepaliveHandlerSignature(boost::system::error_code);
using KeepaliveHandler = std::function<KeepaliveHandlerSignature>;

typedef void ReceiveHandlerSignature(boost::system::error_code, size_t);
using ReceiveHandler = std::function<ReceiveHandlerSignature>;

typedef void SendHandlerSignature(boost::system::error_code);
using SendHandler = std::function<SendHandlerSignature>;

template <class S>
class MessageQueueImpl : public std::enable_shared_from_this<MessageQueueImpl<S>> {
public:
    using Stream = S;

    explicit MessageQueueImpl (boost::asio::io_service& context)
        : mStreamWrapper(context)
        , mSfpTimer(context)
    {
        mLog.add_attribute("Protocol", boost::log::attributes::constant<std::string>("SFP"));
    }

    void close (boost::system::error_code& ec) {
        mSfpTimer.cancel(ec);
        mStreamWrapper.close(ec);
        if (ec) {
            BOOST_LOG(mLog) << "Error closing MessageQueue stream: " << ec.message();
        }
        mReadPumpError = boost::asio::error::operation_aborted;
    }

#ifdef SFP_CONFIG_DEBUG
    void setDebugName (std::string debugName) {
        sfpSetDebugName(&mContext, debugName.c_str());
    }
#endif

    Stream& stream () { return mStreamWrapper.stream(); }
    const Stream& stream () const { return mStreamWrapper.stream(); }

    struct HandshakeOperation;

    template <class CompletionToken>
    BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, HandshakeHandlerSignature)
    asyncHandshake (CompletionToken&& token) {
        util::asio::AsyncCompletion<
            CompletionToken, HandshakeHandlerSignature
        > init { std::forward<CompletionToken>(token) };

        using Op = HandshakeOperation;
        util::asio::makeOperation<Op>(std::move(init.handler), this->shared_from_this())();

        return init.result.get();
    }

    struct KeepaliveOperation;

    template <class CompletionToken>
    BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, KeepaliveHandlerSignature)
    asyncKeepalive (CompletionToken&& token) {
        util::asio::AsyncCompletion<
            CompletionToken, KeepaliveHandlerSignature
        > init { std::forward<CompletionToken>(token) };

        assert(mHandshakeComplete && "asyncHandshake must succeed before calling asyncKeepalive");

        using Op = KeepaliveOperation;
        util::asio::makeOperation<Op>(std::move(init.handler), this->shared_from_this())();

        return init.result.get();
    }

    template <class CompletionToken>
    BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, SendHandlerSignature)
    asyncSend (boost::asio::const_buffer buffer, CompletionToken&& token) {
        util::asio::AsyncCompletion<
            CompletionToken, SendHandlerSignature
        > init { std::forward<CompletionToken>(token) };

        assert(mHandshakeComplete && "asyncHandshake must succeed before calling asyncSend");

        if (auto ec = getReadPumpError()) {
            init.handler(ec);
        }
        else {
            size_t outlen;
            sfpWritePacket(&mContext,
                boost::asio::buffer_cast<const uint8_t*>(buffer),
                boost::asio::buffer_size(buffer), &outlen);
            flushWriteBuffer(std::move(init.handler));
        }

        return init.result.get();
    }

    template <class CompletionToken>
    BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, ReceiveHandlerSignature)
    asyncReceive (boost::asio::mutable_buffer destination, CompletionToken&& token) {
        util::asio::AsyncCompletion<
            CompletionToken, ReceiveHandlerSignature
        > init { std::forward<CompletionToken>(token) };

        assert(mHandshakeComplete && "asyncHandshake must succeed before calling asyncReceive");

        if (auto ec = getReadPumpError()) {
            init.handler(ec, 0);
        }
        else {
            mReceiveQueue.consume(
                    [destination, handler=std::move(init.handler)](boost::system::error_code ec,
                                           const std::vector<uint8_t>& source) mutable {
                size_t nCopied = 0;
                if (!ec) {
                    nCopied = boost::asio::buffer_copy(
                        destination, boost::asio::buffer(source));

                    ec = nCopied == source.size()
                        ? boost::system::error_code()
                        : make_error_code(boost::asio::error::message_size);

                }
                handler(ec, nCopied);
            });
        }

        return init.result.get();
    }

private:
    struct SendData {
        std::vector<uint8_t> buffer;
        SendHandler handler;
    };

    boost::system::error_code getReadPumpError () {
        auto ec = mReadPumpError;
        mReadPumpError = {};
        if (!ec && !stream().is_open()) {
            ec = boost::asio::error::network_down;
        }
        return ec;
    }

    void startReadPump () {
        if (mReadPumpRunning) {
            return;
        }
        mReadPumpRunning = true;
        mReadPumpError = {};
        auto buf = std::make_shared<std::vector<uint8_t>>(1024);
        readPump(buf);
    }

    void readPump (std::shared_ptr<std::vector<uint8_t>> buf) {
        if (stream().is_open()) {
            stream().async_read_some(boost::asio::buffer(*buf),
                std::bind(&MessageQueueImpl::handleRead,
                    this->shared_from_this(), buf, _1, _2));
        }
        else {
            BOOST_LOG(mLog) << "read pump failed, stream not open";
            boost::system::error_code ec = boost::asio::error::network_down;
            voidReceives(ec);
            mReadPumpRunning = false;
            mReadPumpError = ec;
        }
    }

    void handleRead (std::shared_ptr<std::vector<uint8_t>> buf,
                     boost::system::error_code ec,
                     size_t nRead) {
        auto self = this->shared_from_this();
        auto stopReadPump = [self, this] (boost::system::error_code ec) mutable {
            BOOST_LOG(this->mLog) << "read pump: " << ec.message();
            this->voidReceives(ec);
            if (ec != boost::asio::error::operation_aborted) {
                boost::system::error_code ignoredEc;
                this->close(ignoredEc);
            }
            this->mReadPumpRunning = false;
            this->mReadPumpError = ec;
        };

        if (!ec) {
            for (size_t i = 0; i < nRead; ++i) {
                auto rc = sfpDeliverOctet(&mContext, (*buf)[i], nullptr, 0, nullptr);
                (void)rc;
                assert(-1 != rc);
            }
            flushWriteBuffer([self, this, stopReadPump, buf] (boost::system::error_code ec) mutable {
                if (!ec) {
                    this->readPump(buf);
                }
                else {
                    stopReadPump(ec);
                }
            });
        }
        else {
            stopReadPump(ec);
        }
    }

    void flushWriteBuffer (SendHandler handler) {
        if (!mWriteBuffer.size()) {
            handler(boost::system::error_code());
            return;
        }
        mOutbox.emplace(SendData{mWriteBuffer, handler});
        mWriteBuffer.clear();
        if (1 == mOutbox.size()) {
            writePump();
        }
    }

    void writePump () {
        if (mOutbox.size()) {
            if (stream().is_open()) {
                boost::asio::async_write(stream(), boost::asio::buffer(mOutbox.front().buffer),
                    std::bind(&MessageQueueImpl::handleWrite,
                        this->shared_from_this(), _1, _2));
            }
            else {
                BOOST_LOG(mLog) << "write pump failed, stream not open";
                voidOutbox(boost::asio::error::network_down);
            }
        }
    }

    void handleWrite (boost::system::error_code ec, size_t nWritten) {
        if (!ec) {
            assert(mOutbox.front().buffer.size() == nWritten);
            auto handler = mOutbox.front().handler;
            mOutbox.pop();
            handler(ec);
            writePump();
        }
        else {
            BOOST_LOG(mLog) << "write pump: " << ec.message();
            voidOutbox(ec);
            if (ec != boost::asio::error::operation_aborted) {
                boost::system::error_code ignoredEc;
                close(ignoredEc);
            }
        }
    }

    void voidReceives (boost::system::error_code ec) {
        while (mReceiveQueue.depth() < 0) {
            mReceiveQueue.produce(ec, std::vector<uint8_t>());
        }
        while (mReceiveQueue.depth() > 0) {
            mReceiveQueue.consume(
                    [this](boost::system::error_code ec, const std::vector<uint8_t>&) {
                BOOST_LOG(mLog) << "Discarding SFP message: " << ec.message();
            });
        }
    }

    void voidOutbox (boost::system::error_code ec) {
        while (mOutbox.size()) {
            auto handler = mOutbox.front().handler;
            mOutbox.pop();
            handler(ec);
        }
    }

    // pushes octets onto a vector, a write buffer
    static int writeCallback (uint8_t* octets, size_t len, size_t* outlen, void* data) {
        auto& writeBuffer = static_cast<MessageQueueImpl*>(data)->mWriteBuffer;
        writeBuffer.insert(writeBuffer.end(), octets, octets + len);
        if (outlen) { *outlen = len; }
        return 0;
    }

    // Receive one message. Push onto a queue of incoming messages.
    static void deliverCallback (uint8_t* buf, size_t len, void* data) {
        static_cast<MessageQueueImpl*>(data)->mReceiveQueue.produce(
            boost::system::error_code(), std::vector<uint8_t>(buf, buf + len));
    }

    util::ProducerConsumerQueue<boost::system::error_code, std::vector<uint8_t>> mReceiveQueue;

    std::vector<uint8_t> mWriteBuffer;
    std::queue<SendData> mOutbox;

    bool mReadPumpRunning = false;
    boost::system::error_code mReadPumpError;

    _::StreamWrapper<Stream> mStreamWrapper;
    boost::asio::steady_timer mSfpTimer;

    SFPcontext mContext;
    bool mHandshakeComplete = false;

    mutable boost::log::sources::logger mLog;
};

template <class Stream>
struct MessageQueueImpl<Stream>::HandshakeOperation {
    using Nest = MessageQueueImpl;

    HandshakeOperation (std::shared_ptr<Nest> nest)
        : nest_(std::move(nest))
    {}

    std::shared_ptr<Nest> nest_;

    int nTries_ = 0;

    boost::system::error_code rc_ = boost::asio::error::operation_aborted;

    auto result () {
        return std::make_tuple(rc_);
    }

    template <class Op>
    void operator() (Op&& op, boost::system::error_code ec = {}) {
        if (!ec || boost::asio::error::operation_aborted == ec) {
            ec = nest_->getReadPumpError();
        }
        if (!ec) reenter (op) {
            nest_->mHandshakeComplete = false;
            nest_->voidReceives(boost::asio::error::operation_aborted);
            nest_->mWriteBuffer.clear();

            sfpInit(&nest_->mContext);
            sfpSetWriteCallback(&nest_->mContext, SFP_WRITE_MULTIPLE,
                (void*)&Nest::writeCallback, nest_.get());
            sfpSetDeliverCallback(&nest_->mContext, deliverCallback, nest_.get());

            nest_->startReadPump();

            while (1) {
                if (kSfpMaxHandshakeAttempts && nTries_ > kSfpMaxHandshakeAttempts) {
                    BOOST_LOG(nest_->mLog) << "Giving up handshake after "
                        << nTries_ << " attempts";
                    rc_ = Status::HANDSHAKE_FAILED;
                    yield break;
                }

                sfpConnect(&nest_->mContext);
                ++nTries_;
                yield nest_->flushWriteBuffer(std::move(op));

                if (!sfpIsConnected(&nest_->mContext)) {
                    nest_->mSfpTimer.expires_from_now(kSfpConnectTimeout);
                    yield nest_->mSfpTimer.async_wait(std::move(op));
                }

                if (sfpIsConnected(&nest_->mContext)) {
                    nest_->mSfpTimer.expires_from_now(kSfpSettleTimeout);
                    yield nest_->mSfpTimer.async_wait(std::move(op));
                    if (sfpIsConnected(&nest_->mContext)) {
                        nest_->mHandshakeComplete = true;
                        BOOST_LOG(nest_->mLog) << "handshake complete";
                        rc_ = ec;
                        yield break;
                    }
                }
            }
        }
        else if (boost::asio::error::operation_aborted != ec) {
            rc_ = ec;
            nest_->close(ec); // ignored ec
        }
    }
};

template <class Stream>
struct MessageQueueImpl<Stream>::KeepaliveOperation {
    using Nest = MessageQueueImpl<Stream>;

    KeepaliveOperation (std::shared_ptr<Nest> nest)
        : nest_(std::move(nest))
    {}

    std::shared_ptr<Nest> nest_;

    boost::system::error_code rc_ = boost::asio::error::operation_aborted;

    auto result () const {
        BOOST_LOG(nest_->mLog) << "Keepalive operation finished with " << rc_.message();
        return std::make_tuple(rc_);
    }

    template <class Op>
    void operator() (Op&& op, boost::system::error_code ec = {}) {
        if (!ec || boost::asio::error::operation_aborted == ec) {
            ec = nest_->getReadPumpError();
        }
        if (!ec) reenter (op) {
            while (1) {
                nest_->mSfpTimer.expires_from_now(kSfpKeepaliveTimeout);
                yield nest_->mSfpTimer.async_wait(std::move(op));
                yield nest_->asyncSend(boost::asio::const_buffer(), std::move(op));
            }
        }
        else if (boost::asio::error::operation_aborted != ec) {
            rc_ = ec;
        }
    }
};

// Convert a stream object into a message queue using SFP.
template <class S>
class MessageQueue : public util::asio::TransparentIoObject<MessageQueueImpl<S>> {
public:
    using Stream = S;

    explicit MessageQueue (boost::asio::io_service& context)
        : util::asio::TransparentIoObject<MessageQueueImpl<S>>(context)
    {}

    boost::log::sources::logger& log () const {
        return this->get_implementation()->log();
    }

#ifdef SFP_CONFIG_DEBUG
    void setDebugName (std::string debugName) {
        this->get_implementation()->setDebugName(debugName);
    }
#endif

    Stream& stream () {
        return this->get_implementation()->stream();
    }

    const Stream& stream () const {
        return this->get_implementation()->stream();
    }

    UTIL_ASIO_DECL_ASYNC_METHOD(asyncHandshake)

    // Ping the remote end of the message queue every given timeout interval with
    // zero-length messages, to make sure our device is still working. If such a
    // zero-length message fails to send, forward the error code produced to the
    // user's handler.
    UTIL_ASIO_DECL_ASYNC_METHOD(asyncKeepalive)

    UTIL_ASIO_DECL_ASYNC_METHOD(asyncSend)
    UTIL_ASIO_DECL_ASYNC_METHOD(asyncReceive)
};

}} // namespace sfp::asio

#include <boost/asio/unyield.hpp>

#endif
