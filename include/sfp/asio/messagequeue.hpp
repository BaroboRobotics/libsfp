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
#include <queue>
#include <utility>
#include <vector>

namespace sfp {
namespace asio {

namespace sys = boost::system;

/* Convert a stream object into a message queue using SFP. */
template <class Stream>
class MessageQueue {
public:
	using HandshakeHandler = std::function<void(sys::error_code)>;
	using ReceiveHandler = std::function<void(sys::error_code, size_t)>;
	using SendHandler = std::function<void(sys::error_code)>;

	template <class... Args>
	explicit MessageQueue (Args&&... args)
			: mStream(std::forward<Args>(args)...)
			, mSfpTimer(mStream.get_io_service())
			, mStrand(mStream.get_io_service()) { }

	~MessageQueue () {
		boost::system::error_code ec;
		cancel(ec);
	}

	void cancel () {
		boost::system::error_code ec;
		cancel(ec);
		if (ec) {
			throw boost::system::system_error(ec);
		}
	}

	void cancel (boost::system::error_code ec) {
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

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (std::string debugName) {
		sfpSetDebugName(&mContext, debugName.c_str());
	}
#endif

	Stream& stream () { return mStream; }
	const Stream& stream () const { return mStream; }

	template <class Handler>
	BOOST_ASIO_INITFN_RESULT_TYPE(Handler, void(boost::system::error_code))
	asyncHandshake (Handler&& handler) {
		boost::asio::detail::async_result_init<
			Handler, void(boost::system::error_code)
		> init { std::forward<Handler>(handler) };
		auto& realHandler = init.handler;

		mStrand.dispatch([this, realHandler] () mutable {
			reset(boost::asio::error::operation_aborted);
			handshakeCoroutine(realHandler);
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

		mStrand.dispatch([this, realHandler] () mutable {
			reset(boost::asio::error::operation_aborted);
			mStream.get_io_service().post(std::bind(realHandler, sys::error_code()));
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

		mStrand.dispatch([this, buffer, realHandler] () mutable {
			size_t outlen;
			sfpWritePacket(&mContext,
				boost::asio::buffer_cast<const uint8_t*>(buffer),
				boost::asio::buffer_size(buffer), &outlen);
			flushWriteBuffer(realHandler);
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

		mStrand.dispatch([this, buffer, realHandler] () mutable {
			mReceives.emplace(std::make_pair(buffer, realHandler));
			postReceives();
			if (1 == mReceives.size()) {
				readCoroutine();
			}
		});

		return init.result.get();
	}

private:
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

	template <class Handler>
	void handshakeCoroutine (Handler&& handler) {
		BOOST_LOG(mLog) << "Spawning handshake write coroutine";
		boost::asio::spawn(mStrand, [this, handler] (boost::asio::yield_context yield) mutable {
			try {
				// It is possible for the receive handler function to execute
				// after the handshake coroutine exits. Make sure our read
				// buffer doesn't go out of scope.
				auto readBuffer = std::make_shared<std::vector<uint8_t>>(1024);
				mReceives.emplace(boost::asio::buffer(*readBuffer),
					[this, readBuffer] (boost::system::error_code ec, size_t messageSize) {
						if (!ec) {
							readBuffer->resize(messageSize);
							mInbox.push_front(std::move(*readBuffer));
						}
					});

				assert(1 == mReceives.size());
				readCoroutine();

				do {
					BOOST_LOG(mLog) << "Sending sfp connect packet";
					sfpConnect(&mContext);
					flushWriteBuffer([] (sys::error_code) { });
					mSfpTimer.expires_from_now(kSfpConnectTimeout);
					mSfpTimer.async_wait(yield);
				} while (!sfpIsConnected(&mContext));

				BOOST_LOG(mLog) << "sfp appears to be connected";
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
		});
	}

	void readCoroutine () {
		boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
			try {
				while (mReceives.size()) {
					readAndWrite(yield);
					postReceives();
				}
			}
			catch (sys::system_error& e) {
				voidReceives(e.code());
			}
		});
	}

	void postReceives () {
		BOOST_LOG(mLog) << "mInbox.size() == " << mInbox.size() << " | mReceives.size() == "
						<< mReceives.size();
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

	template <class Handler>
	void flushWriteBuffer (Handler handler) {
		if (!mWriteBuffer.size()) {
			mStream.get_io_service().post(std::bind(handler, sys::error_code()));
			return;
		}
		mOutbox.emplace(std::make_pair(mWriteBuffer, handler));
		mWriteBuffer.clear();
		if (1 == mOutbox.size()) {
			BOOST_LOG(mLog) << "spawning writer coroutine";
			boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
				try {
					do {
						boost::asio::async_write(mStream, boost::asio::buffer(mOutbox.front().first), yield);
						mStream.get_io_service().post(
							std::bind(mOutbox.front().second, sys::error_code())
						);
						mOutbox.pop();
					} while (mOutbox.size());
				}
				catch (boost::system::system_error& e) {
					reset(e.code());
				}
			});
		}
	}

	// pushes octets onto a vector, a write buffer
	static int writeCallback (uint8_t* octets, size_t len, size_t* outlen, void* data) {
		auto& writeBuffer = static_cast<MessageQueue*>(data)->mWriteBuffer;
		writeBuffer.insert(writeBuffer.end(), octets, octets + len);
		if (outlen) { *outlen = len; }
		return 0;
	}

	// Receive one message. Push onto a queue of incoming messages.
	static void deliverCallback (uint8_t* buf, size_t len, void* data) {
		static_cast<MessageQueue*>(data)->mInbox.emplace_back(buf, buf + len);
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

template <class Stream>
const std::chrono::milliseconds MessageQueue<Stream>::kSfpConnectTimeout { 100 };
//std::chrono::milliseconds(100);

template <class Stream>
const std::chrono::milliseconds MessageQueue<Stream>::kSfpSettleTimeout { 200 };
//std::chrono::milliseconds(200);

} // namespace asio
} // namespace sfp

#endif