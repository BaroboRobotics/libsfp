#ifndef LIBSFP_ASIO_MESSAGEQUEUE_HPP
#define LIBSFP_ASIO_MESSAGEQUEUE_HPP

#include "sfp/serial_framing_protocol.h"

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include <boost/asio/async_result.hpp>

#include <boost/optional.hpp>

#include <boost/log/common.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/utility/manipulators/dump.hpp>

#include <chrono>
#include <memory>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace sfp {
namespace asio {

namespace sys = boost::system;

using namespace std::placeholders;

template <class S>
class MessageQueueImpl : public std::enable_shared_from_this<MessageQueueImpl<S>> {
public:
	using Stream = S;

	using HandshakeHandler = std::function<void(sys::error_code)>;
	using ReceiveHandler = std::function<void(sys::error_code, size_t)>;
	using SendHandler = std::function<void(sys::error_code)>;

	explicit MessageQueueImpl (boost::asio::io_service& ios)
		: mStream(ios)
		, mSfpTimer(mStream.get_io_service())
		, mStrand(mStream.get_io_service())
	{}

	void init (boost::log::sources::logger log) {
		mLog = log;
	    mLog.add_attribute("Protocol", boost::log::attributes::constant<std::string>("SFP"));
	}

	void destroy () {
        boost::system::error_code ec;
        cancel(ec);
        mStream.close();
	}

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (std::string debugName) {
		sfpSetDebugName(&mContext, debugName.c_str());
	}
#endif

	void cancel () {
		boost::system::error_code ec;
		cancel(ec);
		if (ec) {
			throw boost::system::system_error(ec);
		}
	}

	void cancel (boost::system::error_code& ec) {
		boost::system::error_code localEc;
		ec = localEc;
		mSfpTimer.cancel(localEc);
		if (localEc) {
			ec = localEc;
		}
#warning sfp::asio::MessageQueue::cancel is only half-functional, FIXME
// Maybe we could go the mutex route? Lock a mutex, then void the handlers. Ugly, oh well
	}

	Stream& stream () { return mStream; }
	const Stream& stream () const { return mStream; }

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncHandshake (boost::asio::io_service::work work, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };

		mStrand.post(std::bind(&MessageQueueImpl::asyncHandshakeImpl,
			this->shared_from_this(), work, init.handler));

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncSend (boost::asio::io_service::work work, boost::asio::const_buffer buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };

		mStrand.post(std::bind(&MessageQueueImpl::asyncSendImpl,
			this->shared_from_this(), work, buffer, init.handler));

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code, size_t))
	asyncReceive (boost::asio::io_service::work work, boost::asio::mutable_buffer buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code, size_t)
		> init { std::forward<Handler>(handler) };

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

	void asyncHandshakeImpl (boost::asio::io_service::work work, HandshakeHandler handler) {
		reset(boost::asio::error::operation_aborted);
		startReadPump();
		doHandshake(work, handler);
	}

	void asyncSendImpl (boost::asio::io_service::work work,
						boost::asio::const_buffer buffer,
						SendHandler handler) {
		auto data = boost::asio::buffer_cast<const uint8_t*>(buffer);
		auto size = boost::asio::buffer_size(buffer);

		BOOST_LOG(mLog) << "sending message: "
					    << boost::log::dump(data, size, 8);

		size_t outlen;
		sfpWritePacket(&mContext,
			boost::asio::buffer_cast<const uint8_t*>(buffer),
			boost::asio::buffer_size(buffer), &outlen);
		flushWriteBuffer(work, [handler] (boost::system::error_code ec) { handler(ec); });
	}

	void asyncReceiveImpl (boost::asio::io_service::work work,
						   boost::asio::mutable_buffer buffer,
						   ReceiveHandler handler) {
		mReceives.emplace(ReceiveData{work, buffer, handler});
		postReceives();
	}

	void reset (boost::system::error_code ec) {
		cancel();

		postReceives();
		mInbox = decltype(mInbox)();

		mWriteBuffer.clear();

		sfpInit(&mContext);
		sfpSetWriteCallback(&mContext, SFP_WRITE_MULTIPLE,
			(void*)writeCallback, this);
		sfpSetDeliverCallback(&mContext, deliverCallback, this);
	}

	void doHandshake (boost::asio::io_service::work work, HandshakeHandler handler) {
		boost::asio::io_service::work localWork { mStream.get_io_service() };
		sfpConnect(&mContext);
		flushWriteBuffer(localWork, mStrand.wrap(
			std::bind(&MessageQueueImpl::handshakeStepOne,
				this->shared_from_this(), work, handler, _1)));
	}

	void handshakeStepOne (boost::asio::io_service::work work,
						   HandshakeHandler handler,
						   boost::system::error_code ec) {
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
						this->shared_from_this(), work, handler, _1)));
			}
		}
		else {
			BOOST_LOG(mLog) << "Handshake step one failed: " << ec.message();
			auto& ios = work.get_io_service();
			ios.post(std::bind(handler, ec));
		}
	}

	void handshakeStepTwo (boost::asio::io_service::work work,
						   HandshakeHandler handler,
						   boost::system::error_code ec) {
		if (!ec) {
			if (sfpIsConnected(&mContext)) {
				mSfpTimer.expires_from_now(kSfpSettleTimeout);
				mSfpTimer.async_wait(mStrand.wrap(
					std::bind(&MessageQueueImpl::handshakeFinish,
						this->shared_from_this(), work, handler, _1)));
			}
			else {
				doHandshake(work, handler);
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
		if (!ec) {
			if (sfpIsConnected(&mContext)) {
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

	void startReadPump () {
		if (mReadPumpRunning) {
			return;
		}
		mReadPumpRunning = true;
		auto buf = std::make_shared<std::vector<uint8_t>>(1024);
		readPump(buf);
	}

	void readPump (std::shared_ptr<std::vector<uint8_t>> buf) {
		mStream.async_read_some(boost::asio::buffer(*buf), mStrand.wrap(
			std::bind(&MessageQueueImpl::handleRead,
				this->shared_from_this(), buf, _1, _2)));
	}

	void handleRead (std::shared_ptr<std::vector<uint8_t>> buf,
					 boost::system::error_code ec,
					 size_t nRead) {
		if (!ec) {
			for (size_t i = 0; i < nRead; ++i) {
				auto rc = sfpDeliverOctet(&mContext, (*buf)[i], nullptr, 0, nullptr);
				(void)rc;
				assert(-1 != rc);
			}
			auto self = this->shared_from_this();
			boost::asio::io_service::work localWork { mStream.get_io_service() };
			flushWriteBuffer(localWork, [self, this] (sys::error_code ec) {
				BOOST_LOG(mLog) << "write buffer flushed from read coroutine with " << ec.message();
			});
			postReceives();
			readPump(buf);
		}
		else {
			BOOST_LOG(mLog) << "read pump: " << ec.message();
			voidReceives(ec);
			mReadPumpRunning = false;
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
			boost::asio::async_write(mStream, boost::asio::buffer(mOutbox.front().buffer), mStrand.wrap(
				std::bind(&MessageQueueImpl::handleWrite,
					this->shared_from_this(), _1, _2)));
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

			BOOST_LOG(mLog) << "received message: "
							<< boost::log::dump(mInbox.front().data(), mInbox.front().size(), 8);

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

	static const std::chrono::milliseconds kSfpConnectTimeout;
	static const std::chrono::milliseconds kSfpSettleTimeout;

	std::queue<std::vector<uint8_t>> mInbox;
	std::queue<ReceiveData> mReceives;

	std::vector<uint8_t> mWriteBuffer;
	std::queue<SendData> mOutbox;

	bool mReadPumpRunning = false;

	Stream mStream;
	boost::asio::steady_timer mSfpTimer;
	boost::asio::io_service::strand mStrand;

	SFPcontext mContext;

	mutable boost::log::sources::logger mLog;
};

template <class Stream>
const std::chrono::milliseconds MessageQueueImpl<Stream>::kSfpConnectTimeout { 100 };

template <class Stream>
const std::chrono::milliseconds MessageQueueImpl<Stream>::kSfpSettleTimeout { 200 };

template <class Impl>
class MessageQueueService : public boost::asio::io_service::service {
public:
	using Stream = typename Impl::Stream;

    static boost::asio::io_service::id id;

    explicit MessageQueueService (boost::asio::io_service& ios)
        : boost::asio::io_service::service(ios)
        , mAsyncWork(boost::in_place(std::ref(mAsyncIoService)))
        , mAsyncThread(static_cast<
			size_t(boost::asio::io_service::*)()
			>(&boost::asio::io_service::run), &mAsyncIoService)
    {}

    ~MessageQueueService () {
        mAsyncWork = boost::none;
        mAsyncIoService.stop();
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

    void cancel (implementation_type& impl, boost::system::error_code& ec) {
        impl->cancel(ec);
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
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncHandshake (implementation_type& impl, Handler&& handler) {
        boost::asio::io_service::work work { this->get_io_service() };
        return impl->asyncHandshake(work, std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncSend (implementation_type& impl, boost::asio::const_buffer buffer, Handler&& handler) {
        boost::asio::io_service::work work { this->get_io_service() };
        return impl->asyncSend(work, buffer, std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code, size_t))
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

    void cancel () {
        boost::system::error_code ec;
        cancel(ec);
        if (ec) {
            throw boost::system::system_error(ec);
        }
    }

    void cancel (boost::system::error_code& ec) {
        this->get_service().cancel(this->get_implementation(), ec);
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
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncHandshake (Handler&& handler) {
		return this->get_service().asyncHandshake(this->get_implementation(),
			std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncSend (boost::asio::const_buffer buffer, Handler&& handler) {
		return this->get_service().asyncSend(this->get_implementation(),
			buffer, std::forward<Handler>(handler));
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code, size_t))
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