#ifndef LIBSFP_ASIO_MESSAGEQUEUE_HPP
#define LIBSFP_ASIO_MESSAGEQUEUE_HPP

#include "sfp/serial_framing_protocol.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

#include <boost/asio/async_result.hpp>

#include <boost/log/common.hpp>
#include <boost/log/sources/logger.hpp>

#include <chrono>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

namespace sfp {
namespace asio {

namespace sys = boost::system;

using namespace std::placeholders;

/* Convert a stream object into a message queue using SFP. */
template <class Stream>
class MessageQueue {
public:
	template <class... Args>
	explicit MessageQueue (Args&&... args)
		: mImpl(std::make_shared<Impl>(std::forward<Args>(args)...))
	{}

	~MessageQueue () {
		boost::system::error_code ec;
		cancel(ec);
	}

	using HandshakeHandler = std::function<void(sys::error_code)>;
	using ReceiveHandler = std::function<void(sys::error_code, size_t)>;
	using SendHandler = std::function<void(sys::error_code)>;

	boost::asio::io_service& get_io_service () {
		return mImpl->mStream.get_io_service();
	}

	void cancel () {
		mImpl->cancel();
	}

	void cancel (boost::system::error_code& ec) {
		mImpl->cancel(ec);
	}

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (std::string debugName) {
		sfpSetDebugName(&mImpl->mContext, debugName.c_str());
	}
#endif

	Stream& stream () { return mImpl->mStream; }
	const Stream& stream () const { return mImpl->mStream; }

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncHandshake (Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		auto m = mImpl;
		m->mStrand.dispatch([m, realHandler] () mutable {
			m->reset(boost::asio::error::operation_aborted);
			boost::asio::spawn(m->mStrand, std::bind(&Impl::handshakeCoroutine, m, realHandler, _1));
		});

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncShutdown (Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		auto m = mImpl;
		m->mStrand.dispatch([m, realHandler] () mutable {
			m->reset(boost::asio::error::operation_aborted);
			m->mStream.get_io_service().post(std::bind(realHandler, sys::error_code()));
		});

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncSend (boost::asio::const_buffer buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		auto m = mImpl;
		m->mStrand.dispatch([m, buffer, realHandler] () mutable {
			size_t outlen;
			sfpWritePacket(&m->mContext,
				boost::asio::buffer_cast<const uint8_t*>(buffer),
				boost::asio::buffer_size(buffer), &outlen);
			m->flushWriteBuffer(realHandler);
		});

		return init.result.get();
	}

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code, size_t))
	asyncReceive (boost::asio::mutable_buffer buffer, Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code, size_t)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		auto m = mImpl;
		m->mStrand.dispatch([m, buffer, realHandler] () mutable {
			m->mReceives.emplace(std::make_pair(buffer, realHandler));
			m->postReceives();
			if (1 == m->mReceives.size()) {
				boost::asio::spawn(m->mStrand, std::bind(&Impl::readCoroutine, m, _1));
			}
		});

		return init.result.get();
	}

private:
    // Since a MessageQueue can post handlers which might run after the
    // MessageQueue is destroyed, all data members must be wrapped in a struct
    // to be managed by a std::shared_ptr. All handlers must then have copy of
    // this shared_ptr, either in their lambda capture list (as in the 'm' in
    // MessageQueue's async* operations) or stored in their std::bind-created
    // function object (as in the calls to this->shared_from_this() in
    // handshakeCoroutine). This guarantees that all handlers/coroutines run
    // with access to a valid this pointer.
	struct Impl : std::enable_shared_from_this<Impl> {
		template <class... Args>
		explicit Impl (Args&&... args)
				: mStream(std::forward<Args>(args)...)
				, mSfpTimer(mStream.get_io_service())
				, mStrand(mStream.get_io_service()) { }

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
			mStream.cancel(localEc);
			if (localEc) {
				ec = localEc;
			}
			mSfpTimer.cancel(localEc);
			if (localEc) {
				ec = localEc;
			}
		}

		void reset (boost::system::error_code ec) {
			cancel();

			postReceives();
			voidReceives(ec);
			mInbox = decltype(mInbox)();

			voidOutbox(ec);
			mWriteBuffer.clear();

			sfpInit(&mContext);
			sfpSetWriteCallback(&mContext, SFP_WRITE_MULTIPLE,
				(void*)writeCallback, this);
			sfpSetDeliverCallback(&mContext, deliverCallback, this);
		}

		void postReceives () {
			while (mInbox.size() && mReceives.size()) {
				auto nCopied = boost::asio::buffer_copy(mReceives.front().first,
					boost::asio::buffer(mInbox.front()));

				auto ec = nCopied == mInbox.front().size()
						  ? sys::error_code()
						  : make_error_code(boost::asio::error::message_size);

				mStream.get_io_service().post(std::bind(mReceives.front().second, ec, nCopied));
				mInbox.pop_front();
				mReceives.pop();
			}
		}

		void voidReceives (boost::system::error_code ec) {
			while (mReceives.size()) {
				mStream.get_io_service().post(std::bind(mReceives.front().second, ec, 0));
				mReceives.pop();
			}
		}

		void voidOutbox (boost::system::error_code ec) {
			while (mOutbox.size()) {
				mStream.get_io_service().post(std::bind(mOutbox.front().second, ec));
				mOutbox.pop();
			}
		}

		void onFirstUserMessage (std::shared_ptr<std::vector<uint8_t>> readBuffer, boost::system::error_code ec, size_t messageSize) {
			if (!ec) {
				readBuffer->resize(messageSize);
				mInbox.push_front(std::move(*readBuffer));
			}
		}

		void handshakeCoroutine (HandshakeHandler handler, boost::asio::yield_context yield) {
			try {
				// It is possible for the receive handler function to execute
				// after the handshake coroutine exits. Make sure our read
				// buffer doesn't go out of scope.
				auto readBuffer = std::make_shared<std::vector<uint8_t>>(1024);
				mReceives.emplace(boost::asio::buffer(*readBuffer),
					std::bind(&Impl::onFirstUserMessage, this->shared_from_this(), readBuffer, _1, _2));

				assert(1 == mReceives.size());
				boost::asio::spawn(mStrand, std::bind(&Impl::readCoroutine, this->shared_from_this(), _1));

				do {
					sfpConnect(&mContext);
					flushWriteBuffer([] (sys::error_code) { });
					mSfpTimer.expires_from_now(kSfpConnectTimeout);
					mSfpTimer.async_wait(yield);
				} while (!sfpIsConnected(&mContext));

				mSfpTimer.expires_from_now(kSfpSettleTimeout);
				mSfpTimer.async_wait(yield);
				BOOST_LOG(mLog) << "sfp connection is settled";

				mStream.cancel();
				mStream.get_io_service().post(std::bind(handler, sys::error_code()));
			}
			catch (sys::system_error& e) {
				mStream.get_io_service().post(std::bind(handler, e.code()));
				if (boost::asio::error::operation_aborted != e.code()) {
					reset(e.code());
				}
			}
		}

		void readAndWrite (boost::asio::yield_context yield) {
			uint8_t buf[256];
			auto nRead = mStream.async_read_some(boost::asio::buffer(buf), yield);
			for (size_t i = 0; i < nRead; ++i) {
				auto rc = sfpDeliverOctet(&mContext, buf[i], nullptr, 0, nullptr);
				(void)rc;
				assert(-1 != rc);
			}
			flushWriteBuffer([] (sys::error_code) { });
		}

		void readCoroutine (boost::asio::yield_context yield) {
			try {
				while (mReceives.size()) {
					readAndWrite(yield);
					postReceives();
				}
			}
			catch (sys::system_error& e) {
				BOOST_LOG(mLog) << "SFP MessageQueue " << this << " readCoroutine exception caught: " << e.what();
				voidReceives(e.code());
			}
		}

		void writeCoroutine (boost::asio::yield_context yield) {
			while (mOutbox.size()) {
				auto outPair = mOutbox.front();
				mOutbox.pop();
				boost::system::error_code ec;
				boost::asio::async_write(mStream, boost::asio::buffer(outPair.first), yield[ec]);
				mStream.get_io_service().post(std::bind(outPair.second, ec));
			}
		}

		// pushes octets onto a vector, a write buffer
		static int writeCallback (uint8_t* octets, size_t len, size_t* outlen, void* data) {
			auto& writeBuffer = static_cast<MessageQueue::Impl*>(data)->mWriteBuffer;
			writeBuffer.insert(writeBuffer.end(), octets, octets + len);
			if (outlen) { *outlen = len; }
			return 0;
		}

		// Receive one message. Push onto a queue of incoming messages.
		static void deliverCallback (uint8_t* buf, size_t len, void* data) {
			static_cast<MessageQueue::Impl*>(data)->mInbox.emplace_back(buf, buf + len);
		}

		template <class Handler>
		void flushWriteBuffer (Handler handler) {
			if (!mWriteBuffer.size()) {
				mStream.get_io_service().post(std::bind(handler, sys::error_code()));
				return;
			}
			mOutbox.emplace(std::make_pair(mWriteBuffer, handler));
			mWriteBuffer.clear();
			if (1 == mOutbox.size()) {
				boost::asio::spawn(mStrand, std::bind(&Impl::writeCoroutine, this->shared_from_this(), _1));
			}
		}

		static const std::chrono::milliseconds kSfpConnectTimeout;
		static const std::chrono::milliseconds kSfpSettleTimeout;

		mutable boost::log::sources::logger mLog;

		std::deque<std::vector<uint8_t>> mInbox;
		std::queue<std::pair<boost::asio::mutable_buffer, ReceiveHandler>> mReceives;

		std::vector<uint8_t> mWriteBuffer;
		std::queue<std::pair<std::vector<uint8_t>, SendHandler>> mOutbox;

		Stream mStream;
		boost::asio::steady_timer mSfpTimer;
		boost::asio::io_service::strand mStrand;

		SFPcontext mContext;
	};

	std::shared_ptr<Impl> mImpl;
};

template <class Stream>
const std::chrono::milliseconds MessageQueue<Stream>::Impl::kSfpConnectTimeout { 100 };

template <class Stream>
const std::chrono::milliseconds MessageQueue<Stream>::Impl::kSfpSettleTimeout { 200 };

} // namespace asio
} // namespace sfp

#endif