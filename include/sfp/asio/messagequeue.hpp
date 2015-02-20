#ifndef LIBSFP_ASIO_MESSAGEQUEUE_HPP
#define LIBSFP_ASIO_MESSAGEQUEUE_HPP

#include "sfp/serial_framing_protocol.h"
#include "sfp/system_error.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>

#include <boost/optional.hpp>

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

namespace sfp {
namespace asio {

namespace sys = boost::system;

using namespace std::placeholders;

namespace detail {

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
		boost::log::sources::logger log;
		BOOST_LOG(log) << "Closing a generic Stream";
		mStream.close(ec);
		if (ec) {
			BOOST_LOG(log) << "Stream.close() resulted in " << ec.message();
		}
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
		boost::log::sources::logger log;
		BOOST_LOG(log) << "Closing a basic_stream_socket";
		boost::system::error_code lEc;
		ec = lEc;
		mStream.shutdown(boost::asio::socket_base::shutdown_both, lEc);
		if (lEc) {
			ec = lEc;
			BOOST_LOG(log) << "basic_stream_socket.shutdown() resulted in " << ec.message();
		}
		mStream.close(lEc);
		if (lEc) {
			ec = lEc;
			BOOST_LOG(log) << "basic_stream_socket.close() resulted in " << ec.message();
		}
	}

private:
	Stream mStream;
};

} // namespace detail

using HandshakeHandlerSignature = void(sys::error_code);
using HandshakeHandler = std::function<HandshakeHandlerSignature>;

using KeepaliveHandlerSignature = void(sys::error_code);
using KeepaliveHandler = std::function<KeepaliveHandlerSignature>;

using ReceiveHandlerSignature = void(sys::error_code, size_t);
using ReceiveHandler = std::function<ReceiveHandlerSignature>;

using SendHandlerSignature = void(sys::error_code);
using SendHandler = std::function<SendHandlerSignature>;

template <class S>
class MessageQueueImpl : public std::enable_shared_from_this<MessageQueueImpl<S>> {
public:
	using Stream = S;

	explicit MessageQueueImpl (boost::asio::io_service& ios)
		: mStreamWrapper(ios)
		, mSfpTimer(ios)
		, mStrand(ios)
	{}

	~MessageQueueImpl () {
		BOOST_LOG(mLog) << "~MessageQueueImpl: (inbox: " << mInbox.size()
						<< "), (outbox: " << mOutbox.size()
						<< "), (receives: " << mReceives.size()
						<< ")";
	}

	void init (boost::log::sources::logger log) {
		mLog = log;
	    mLog.add_attribute("Protocol", boost::log::attributes::constant<std::string>("SFP"));
	}

	void destroy () {
        boost::system::error_code ec;
        close(ec);
	}

	void close (boost::system::error_code ec) {
		auto self = this->shared_from_this();
		mStrand.post([self, this] () mutable {
			boost::system::error_code ec;
			this->mSfpTimer.cancel(ec);
			this->stream().close(ec);
			if (ec) {
				BOOST_LOG(mLog) << "Error closing MessageQueue stream: " << ec.message();
			}
			this->mStreamError = boost::asio::error::operation_aborted;
		});
		// FIXME, can't report an error, because we need to worry about thread
		// safety. Could fix this by using a mutex to protect the timer and
		// stream objects. :/
		ec = boost::system::error_code();
	}

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (std::string debugName) {
		sfpSetDebugName(&mContext, debugName.c_str());
	}
#endif

	Stream& stream () { return mStreamWrapper.stream(); }
	const Stream& stream () const { return mStreamWrapper.stream(); }

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, HandshakeHandlerSignature)
	asyncHandshake (boost::asio::io_service::work work, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, HandshakeHandlerSignature
		> init { std::forward<Handler>(handler) };

		mStrand.post(std::bind(&MessageQueueImpl::asyncHandshakeImpl,
			this->shared_from_this(), work, init.handler));

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, KeepaliveHandlerSignature)
	asyncKeepalive (boost::asio::io_service::work work, Handler&& handler) {
	    boost::asio::detail::async_result_init<
	        Handler, KeepaliveHandlerSignature
	    > init { std::forward<Handler>(handler) };

	    assert(mHandshakeComplete && "asyncHandshake must succeed before calling asyncKeepalive");

	    mStrand.post(std::bind(&MessageQueueImpl::asyncKeepaliveImpl,
            this->shared_from_this(), work, init.handler));

	    return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, SendHandlerSignature)
	asyncSend (boost::asio::io_service::work work, boost::asio::const_buffer buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, SendHandlerSignature
		> init { std::forward<Handler>(handler) };

	    assert(mHandshakeComplete && "asyncHandshake must succeed before calling asyncSend");

		mStrand.post(std::bind(&MessageQueueImpl::asyncSendImpl,
			this->shared_from_this(), work, buffer, init.handler));

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, ReceiveHandlerSignature)
	asyncReceive (boost::asio::io_service::work work, boost::asio::mutable_buffer buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, ReceiveHandlerSignature
		> init { std::forward<Handler>(handler) };

	    assert(mHandshakeComplete && "asyncHandshake must succeed before calling asyncReceive");

		mStrand.post(std::bind(&MessageQueueImpl::asyncReceiveImpl,
			this->shared_from_this(), work, buffer, init.handler));

		return init.result.get();
	}

private:
	struct SendData {
		boost::asio::io_service::work work;
		std::vector<uint8_t> buffer;
		SendHandler handler;
	};

	struct ReceiveData {
		boost::asio::io_service::work work;
		boost::asio::mutable_buffer buffer;
		ReceiveHandler handler;
	};

	boost::system::error_code getStreamError () {
		auto ec = mStreamError;
		mStreamError = boost::system::error_code();
		if (!ec && !stream().is_open()) {
			ec = boost::asio::error::network_down;
		}
		return ec;
	}

	void asyncHandshakeImpl (boost::asio::io_service::work work, HandshakeHandler handler) {
		auto ec = getStreamError();
		if (!ec) {
			mHandshakeComplete = false;

			postReceives();
			voidReceives(boost::asio::error::operation_aborted);
			mInbox = decltype(mInbox)();

			mWriteBuffer.clear();

			sfpInit(&mContext);
			sfpSetWriteCallback(&mContext, SFP_WRITE_MULTIPLE,
				(void*)writeCallback, this);
			sfpSetDeliverCallback(&mContext, deliverCallback, this);

			startReadPump();
			doHandshake(work, handler);
		}
		else {
			work.get_io_service().post(std::bind(handler, ec));
		}
	}

	void asyncKeepaliveImpl (boost::asio::io_service::work work, KeepaliveHandler handler) {
		auto ec = getStreamError();
		if (!ec) {
			doKeepalive(work, handler);
		}
		else {
			work.get_io_service().post(std::bind(handler, ec));
		}
	}

	void asyncSendImpl (boost::asio::io_service::work work,
						boost::asio::const_buffer buffer,
						SendHandler handler) {
		auto ec = getStreamError();
		if (!ec) {
			size_t outlen;
			sfpWritePacket(&mContext,
				boost::asio::buffer_cast<const uint8_t*>(buffer),
				boost::asio::buffer_size(buffer), &outlen);
			flushWriteBuffer(work, [handler] (boost::system::error_code ec) { handler(ec); });
		}
		else {
			work.get_io_service().post(std::bind(handler, ec));
		}
	}

	void asyncReceiveImpl (boost::asio::io_service::work work,
						   boost::asio::mutable_buffer buffer,
						   ReceiveHandler handler) {
		auto ec = getStreamError();
		if (!ec) {
			mReceives.emplace(ReceiveData{work, buffer, handler});
			postReceives();
		}
		else {
			work.get_io_service().post(std::bind(handler, ec, 0));
		}
	}

	void doHandshake (boost::asio::io_service::work work, HandshakeHandler handler, int nTries = 0) {
		boost::asio::io_service::work localWork { stream().get_io_service() };

		if (kSfpMaxHandshakeAttempts && nTries > kSfpMaxHandshakeAttempts) {
			BOOST_LOG(mLog) << "Giving up handshake after " << nTries << " attempts";
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, Status::HANDSHAKE_FAILED));
		}
		else {
			sfpConnect(&mContext);
			flushWriteBuffer(localWork, mStrand.wrap(
				std::bind(&MessageQueueImpl::handshakeStepOne,
					this->shared_from_this(), work, handler, ++nTries, _1)));
		}
	}

	void handshakeStepOne (boost::asio::io_service::work work,
						   HandshakeHandler handler,
						   int nTries,
						   boost::system::error_code ec) {
		if (!ec || boost::asio::error::operation_aborted == ec) {
			ec = getStreamError();
		}


		if (!ec) {
			if (sfpIsConnected(&mContext)) {
				mSfpTimer.expires_from_now(kSfpSettleTimeout);
				mSfpTimer.async_wait(mStrand.wrap(
					std::bind(&MessageQueueImpl::handshakeFinish,
						this->shared_from_this(), work, handler, _1)));
			}
			else {
				mSfpTimer.expires_from_now(kSfpConnectTimeout);
				mSfpTimer.async_wait(mStrand.wrap(
					std::bind(&MessageQueueImpl::handshakeStepTwo,
						this->shared_from_this(), work, handler, nTries, _1)));
			}
		}
		else {
			if (ec != boost::asio::error::operation_aborted) {
				boost::system::error_code ignoredEc;
				close(ignoredEc);
			}
			BOOST_LOG(mLog) << "Handshake step one failed: " << ec.message();
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, ec));
		}
	}

	void handshakeStepTwo (boost::asio::io_service::work work,
						   HandshakeHandler handler,
						   int nTries,
						   boost::system::error_code ec) {
		if (!ec || boost::asio::error::operation_aborted == ec) {
			ec = getStreamError();
		}

		if (!ec) {
			if (sfpIsConnected(&mContext)) {
				mSfpTimer.expires_from_now(kSfpSettleTimeout);
				mSfpTimer.async_wait(mStrand.wrap(
					std::bind(&MessageQueueImpl::handshakeFinish,
						this->shared_from_this(), work, handler, _1)));
			}
			else {
				doHandshake(work, handler, nTries);
			}
		}
		else {
			BOOST_LOG(mLog) << "Handshake step two failed: " << ec.message();
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, ec));
		}
	}

	void handshakeFinish (boost::asio::io_service::work work,
						   HandshakeHandler handler,
						   boost::system::error_code ec) {
		if (!ec || boost::asio::error::operation_aborted == ec) {
			ec = getStreamError();
		}

		if (!ec) {
			if (sfpIsConnected(&mContext)) {
				mHandshakeComplete = true;
				auto& ios = work.get_io_service();
				ios.post(std::bind(handler, ec));
				BOOST_LOG(mLog) << "handshake complete";
			}
			else {
				doHandshake(work, handler);
			}
		}
		else {
			BOOST_LOG(mLog) << "Handshake finish failed: " << ec.message();
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, ec));
		}
	}

	void doKeepalive (boost::asio::io_service::work work,
					  KeepaliveHandler handler) {
		mSfpTimer.expires_from_now(kSfpKeepaliveTimeout);
		mSfpTimer.async_wait(mStrand.wrap(
			std::bind(&MessageQueueImpl::keepaliveStepOne,
				this->shared_from_this(), work, handler, _1)));
	}

	void keepaliveStepOne (boost::asio::io_service::work work,
						   KeepaliveHandler handler,
						   boost::system::error_code ec) {
		if (!ec) {
			asyncSend(work, boost::asio::const_buffer(), mStrand.wrap(
				std::bind(&MessageQueueImpl::keepaliveStepTwo,
					this->shared_from_this(), work, handler, _1)));
		}
		else {
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, ec));
		}
	}

	void keepaliveStepTwo (boost::asio::io_service::work work,
						   KeepaliveHandler handler,
						   boost::system::error_code ec) {
		if (!ec) {
			doKeepalive(work, handler);
		}
		else {
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, ec));
		}
    }

	void startReadPump () {
		if (mReadPumpRunning) {
			return;
		}
		mReadPumpRunning = true;
		mStreamError = boost::system::error_code();
		auto buf = std::make_shared<std::vector<uint8_t>>(1024);
		readPump(buf);
	}

	void readPump (std::shared_ptr<std::vector<uint8_t>> buf) {
		if (stream().is_open()) {
			stream().async_read_some(boost::asio::buffer(*buf), mStrand.wrap(
				std::bind(&MessageQueueImpl::handleRead,
					this->shared_from_this(), buf, _1, _2)));
		}
		else {
			BOOST_LOG(mLog) << "read pump failed, stream not open";
			boost::system::error_code ec = boost::asio::error::network_down;
			voidReceives(ec);
			mReadPumpRunning = false;
			mStreamError = ec;
		}
	}

	void handleRead (std::shared_ptr<std::vector<uint8_t>> buf,
					 boost::system::error_code ec,
					 size_t nRead) {
		auto self = this->shared_from_this();
		auto stopReadPump = [self, this] (sys::error_code ec) mutable {
			BOOST_LOG(this->mLog) << "read pump: " << ec.message();
			this->voidOutbox(ec);
			this->voidReceives(ec);
			if (ec != boost::asio::error::operation_aborted) {
				boost::system::error_code ignoredEc;
				this->close(ignoredEc);
			}
			this->mReadPumpRunning = false;
			this->mStreamError = ec;
		};

		if (!ec) {
			for (size_t i = 0; i < nRead; ++i) {
				auto rc = sfpDeliverOctet(&mContext, (*buf)[i], nullptr, 0, nullptr);
				(void)rc;
				assert(-1 != rc);
			}
			boost::asio::io_service::work localWork { stream().get_io_service() };
			flushWriteBuffer(localWork, mStrand.wrap([self, this, stopReadPump, buf] (sys::error_code ec) mutable {
				if (!ec) {
					this->postReceives();
					this->readPump(buf);
				}
				else {
					stopReadPump(ec);
				}
			}));
		}
		else {
			stopReadPump(ec);
		}
	}

	void flushWriteBuffer (boost::asio::io_service::work work, SendHandler handler) {
		if (!mWriteBuffer.size()) {
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, sys::error_code()));
			return;
		}
		mOutbox.emplace(SendData{work, mWriteBuffer, handler});
		mWriteBuffer.clear();
		if (1 == mOutbox.size()) {
			writePump();
		}
	}

	void writePump () {
		if (mOutbox.size()) {
			if (stream().is_open()) {
				boost::asio::async_write(stream(), boost::asio::buffer(mOutbox.front().buffer), mStrand.wrap(
					std::bind(&MessageQueueImpl::handleWrite,
						this->shared_from_this(), _1, _2)));
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
			auto& ios = mOutbox.front().work.get_io_service();
			ios.post(std::bind(mOutbox.front().handler, ec));
			mOutbox.pop();
			writePump();
		}
		else {
			BOOST_LOG(mLog) << "write pump: " << ec.message();
			voidOutbox(ec);
			voidReceives(ec);
			if (ec != boost::asio::error::operation_aborted) {
				boost::system::error_code ignoredEc;
				close(ignoredEc);
			}
		}
	}

	void postReceives () {
		while (mInbox.size() && mReceives.size()) {
			auto& receive = mReceives.front();
			auto nCopied = boost::asio::buffer_copy(receive.buffer,
				boost::asio::buffer(mInbox.front()));

			auto ec = nCopied == mInbox.front().size()
					  ? sys::error_code()
					  : make_error_code(boost::asio::error::message_size);

			auto& ios = receive.work.get_io_service();
			ios.post(std::bind(receive.handler, ec, nCopied));
			mInbox.pop();
			mReceives.pop();
		}
	}

	void voidReceives (boost::system::error_code ec) {
		while (mReceives.size()) {
			auto& receive = mReceives.front();
			auto& ios = receive.work.get_io_service();
			ios.post(std::bind(receive.handler, ec, 0));
			mReceives.pop();
		}
	}

	void voidOutbox (boost::system::error_code ec) {
		while (mOutbox.size()) {
			auto& ios = mOutbox.front().work.get_io_service();
			ios.post(std::bind(mOutbox.front().handler, ec));
			mOutbox.pop();
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
		static_cast<MessageQueueImpl*>(data)->mInbox.emplace(buf, buf + len);
	}

	std::chrono::milliseconds kSfpConnectTimeout { 100 } ;
	std::chrono::milliseconds kSfpSettleTimeout { 200 } ;
	std::chrono::milliseconds kSfpKeepaliveTimeout { 500 };
	static constexpr const int kSfpMaxHandshakeAttempts { 50 };

	std::queue<std::vector<uint8_t>> mInbox;
	std::queue<ReceiveData> mReceives;

	std::vector<uint8_t> mWriteBuffer;
	std::queue<SendData> mOutbox;

	bool mReadPumpRunning = false;
	boost::system::error_code mStreamError;

	detail::StreamWrapper<Stream> mStreamWrapper;
	boost::asio::steady_timer mSfpTimer;
	boost::asio::io_service::strand mStrand;

	SFPcontext mContext;
	bool mHandshakeComplete = false;

	mutable boost::log::sources::logger mLog;
};

template <class Impl>
class MessageQueueService : public boost::asio::io_service::service {
public:
	using Stream = typename Impl::Stream;

    static boost::asio::io_service::id id;

    explicit MessageQueueService (boost::asio::io_service& ios)
        : boost::asio::io_service::service(ios)
        , mAsyncWork(boost::in_place(std::ref(mAsyncIoService)))
        , mAsyncThread([this] () mutable {
            boost::log::sources::logger log;
            try {
	            boost::system::error_code ec;
	            auto nHandlers = this->mAsyncIoService.run(ec);
	            BOOST_LOG(log) << "SFP MessageQueueService: " << nHandlers << " completed with " << ec.message();
            }
            catch (std::exception& e) {
            	BOOST_LOG(log) << "SFP MessageQueueService died with " << e.what();
            }
            catch (...) {
            	BOOST_LOG(log) << "SFP MessageQueueService died by unknown cause";
            }
        })
    {}

    ~MessageQueueService () {
        mAsyncWork = boost::none;
        //mAsyncIoService.stop();
        mAsyncThread.join();
    }

    using implementation_type = std::shared_ptr<Impl>;

    void construct (implementation_type& impl) {
        impl.reset(new Impl(mAsyncIoService));
    }

    void move_construct (implementation_type& impl, implementation_type& other) {
		impl = std::move(other);
    }

    void destroy (implementation_type& impl) {
        impl->destroy();
        impl.reset();
    }

    void close (implementation_type& impl, boost::system::error_code& ec) {
        impl->close(ec);
    }

    void init (implementation_type& impl, boost::log::sources::logger log) {
        impl->init(log);
    }

    boost::log::sources::logger log (const implementation_type& impl) const {
        return impl->log();
    }

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (implementation_type& impl, std::string debugName) {
		impl->setDebugName(debugName);
	}
#endif

	Stream& stream (implementation_type& impl) {
		return impl->stream();
	}

	const Stream& stream (implementation_type& impl) const {
		return impl->stream();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, HandshakeHandlerSignature)
	asyncHandshake (implementation_type& impl, Handler&& handler) {
        boost::asio::io_service::work work { this->get_io_service() };
        return impl->asyncHandshake(work, std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, KeepaliveHandlerSignature)
	asyncKeepalive (implementation_type& impl, Handler&& handler) {
		boost::asio::io_service::work work { this->get_io_service() };
		return impl->asyncKeepalive(work, std::forward<Handler>(handler));
	}


	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, SendHandlerSignature)
	asyncSend (implementation_type& impl, boost::asio::const_buffer buffer, Handler&& handler) {
        boost::asio::io_service::work work { this->get_io_service() };
        return impl->asyncSend(work, buffer, std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, ReceiveHandlerSignature)
	asyncReceive (implementation_type& impl, boost::asio::mutable_buffer buffer, Handler&& handler) {
        boost::asio::io_service::work work { this->get_io_service() };
        return impl->asyncReceive(work, buffer, std::forward<Handler>(handler));
	}

private:
    void shutdown_service () {}

    boost::asio::io_service mAsyncIoService;
    boost::optional<boost::asio::io_service::work> mAsyncWork;
    std::thread mAsyncThread;
};

template <class Impl>
boost::asio::io_service::id MessageQueueService<Impl>::id;

/* Convert a stream object into a message queue using SFP. */
template <class Service>
class BasicMessageQueue : public boost::asio::basic_io_object<Service> {
public:
	using Stream = typename Service::Stream;

	BasicMessageQueue (boost::asio::io_service& ios, boost::log::sources::logger log)
		: boost::asio::basic_io_object<Service>(ios)
	{
        this->get_service().init(this->get_implementation(), log);
	}

	BasicMessageQueue (const BasicMessageQueue&) = delete;
	BasicMessageQueue& operator= (const BasicMessageQueue&) = delete;

	void close () {
		boost::system::error_code ec;
		close(ec);
		if (ec) {
			throw boost::system::system_error(ec);
		}
	}

    void close (boost::system::error_code& ec) {
        this->get_service().close(this->get_implementation(), ec);
    }

    boost::log::sources::logger log () const {
        return this->get_service().log(this->get_implementation());
    }

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (std::string debugName) {
		this->get_service().setDebugName(this->get_implementation(), debugName);
	}
#endif

	Stream& stream () {
		return this->get_service().stream(this->get_implementation());
	}

	const Stream& stream () const {
		return this->get_service().stream(this->get_implementation());
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, HandshakeHandlerSignature)
	asyncHandshake (Handler&& handler) {
		return this->get_service().asyncHandshake(this->get_implementation(),
			std::forward<Handler>(handler));
	}

	// Ping the remote end of the message queue every given timeout interval with
	// zero-length messages, to make sure our device is still working. If such a
	// zero-length message fails to send, forward the error code produced to the
	// user's handler.
	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, KeepaliveHandlerSignature)
	asyncKeepalive (Handler&& handler) {
		return this->get_service().asyncKeepalive(this->get_implementation(),
			std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, SendHandlerSignature)
	asyncSend (boost::asio::const_buffer buffer, Handler&& handler) {
		return this->get_service().asyncSend(this->get_implementation(),
			buffer, std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, ReceiveHandlerSignature)
	asyncReceive (boost::asio::mutable_buffer buffer, Handler&& handler) {
		return this->get_service().asyncReceive(this->get_implementation(),
			buffer, std::forward<Handler>(handler));
	}
};

template <class Stream>
using MessageQueue = BasicMessageQueue<MessageQueueService<MessageQueueImpl<Stream>>>;

} // namespace asio
} // namespace sfp

#endif
