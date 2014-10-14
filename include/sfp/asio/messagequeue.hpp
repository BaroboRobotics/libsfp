#ifndef LIBSFP_ASIO_MESSAGEQUEUE_HPP
#define LIBSFP_ASIO_MESSAGEQUEUE_HPP

#include "sfp/serial_framing_protocol.h"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>

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
	using HandshakeHandler = std::function<void(sys::error_code&)>;
	using ReceiveHandler = std::function<void(sys::error_code&)>;
	using SendHandler = std::function<void(sys::error_code&)>;

	template <class... Args>
	explicit MessageQueue (Args&&... args)
			: mStream(std::forward<Args>(args)...)
			, mSfpTimer(mStream.get_io_service())
			, mStrand(mStream.get_io_service()) { }

	~MessageQueue () {
		mStream.cancel();
		mSfpTimer.cancel();
	}

#ifdef SFP_CONFIG_DEBUG
	void setDebugName (std::string debugName) {
		sfpSetDebugName(&mContext, debugName.c_str());
	}
#endif

	Stream& stream () { return mStream; }
	const Stream& stream () const { return mStream; }

	template <class Handler>
	void asyncHandshake (Handler handler) {
		mStrand.dispatch([=] () mutable {
			resetSfp();
			mHandshakeHandler = handler;
			handshakeWriteCoroutine();
			handshakeReadCoroutine();
		});
	}

	template <class Handler>
	void asyncShutdown (Handler handler) {
		mStrand.dispatch([=] () mutable {
			resetSfp();
			mStream.get_io_service().post(std::bind(handler,
				sys::error_code(sys::errc::success, sys::generic_category())));
		});
	}

	template <class Handler>
	void asyncSend (const boost::asio::const_buffer& buffer, Handler handler) {
		mStrand.dispatch([=] () mutable {
			size_t outlen;
			sfpWritePacket(&mContext,
				boost::asio::buffer_cast<const uint8_t*>(buffer),
				boost::asio::buffer_size(buffer), &outlen);
			flushWriteBuffer(handler);
		});
	}

	template <class Handler>
	void asyncReceive (const boost::asio::mutable_buffer& buffer, Handler handler) {
		mStrand.dispatch([=] () mutable {
			mReceives.emplace(std::make_pair(buffer, handler));
			postReceives();
		});
	}

private:
	void resetSfp () {
		mStream.cancel();
		mSfpTimer.cancel();

		if (mHandshakeHandler) {
			mStream.get_io_service().post(
				std::bind(mHandshakeHandler, sys::error_code(boost::asio::error::eof)));
			mHandshakeHandler = nullptr;
		}

		postReceives();
		while (mReceives.size()) {
			mStream.get_io_service().post(
				std::bind(mReceives.front().second, sys::error_code(boost::asio::error::eof)));
			mReceives.pop();
		}

		mInbox = decltype(mInbox)();
		mReceives = decltype(mReceives)();

		while (mOutbox.size()) {
			mStream.get_io_service().post(
				std::bind(mOutbox.front().second, sys::error_code(boost::asio::error::eof)));
			mOutbox.pop();
		}
		
		mWriteBuffer.clear();
		mOutbox = decltype(mOutbox)();

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
			assert(-1 != rc);
		}
		flushWriteBuffer([] (sys::error_code) { });
	}

	void handshakeWriteCoroutine () {
		BOOST_LOG(mLog) << "Spawning handshake write coroutine";
		boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
			try {
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
				assert(mHandshakeHandler);
				mStream.get_io_service().post(std::bind(mHandshakeHandler,
					sys::error_code(sys::errc::success, sys::generic_category())));
				mHandshakeHandler = nullptr;
			}
			catch (sys::system_error& e) {
				if (boost::asio::error::operation_aborted != e.code()) {
					throw;
				}
			}
		});
	}

	void handshakeReadCoroutine () {
		boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
			try {
				while (true) {
					postReceives();
					readAndWrite(yield);
				}
			}
			catch (sys::system_error& e) {
				if (boost::asio::error::operation_aborted != e.code()) {
					throw;
				}
			}
		});
	}

	void postReceives () {
		BOOST_LOG(mLog) << "mInbox.size() == " << mInbox.size() << " | mReceives.size() == "
						<< mReceives.size();
		while (mInbox.size() && mReceives.size()) {
			auto nCopied = boost::asio::buffer_copy(mReceives.front().first,
				boost::asio::buffer(mInbox.front()));
			
			auto ec = nCopied <= boost::asio::buffer_size(mReceives.front().first)
					  ? sys::error_code(sys::errc::success, sys::generic_category())
					  : sys::error_code(boost::asio::error::message_size);

			mStream.get_io_service().post(std::bind(mReceives.front().second, ec));
			mInbox.pop();
			mReceives.pop();
		}
	}

	template <class Handler>
	void flushWriteBuffer (Handler handler) {
		if (!mWriteBuffer.size()) {
			mStream.get_io_service().post(std::bind(handler,
				sys::error_code(sys::errc::success, sys::generic_category())
			));
			return;
		}
		mOutbox.emplace(std::make_pair(mWriteBuffer, handler));
		mWriteBuffer.clear();
		if (1 == mOutbox.size()) {
			BOOST_LOG(mLog) << "spawning writer coroutine";
			boost::asio::spawn(mStrand, [this] (boost::asio::yield_context yield) mutable {
				do {
					auto ec = sys::error_code(sys::errc::success, sys::generic_category());
					boost::asio::async_write(mStream, boost::asio::buffer(mOutbox.front().first), yield[ec]);
					if (boost::asio::error::operation_aborted == ec) {
						break;
					}
					mStream.get_io_service().post(std::bind(mOutbox.front().second, ec));
					mOutbox.pop();
				} while (mOutbox.size());
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
		static_cast<MessageQueue*>(data)->mInbox.emplace(buf, buf + len);
	}

	static const std::chrono::milliseconds kSfpConnectTimeout;
	static const std::chrono::milliseconds kSfpSettleTimeout;

	mutable boost::log::sources::logger mLog;

	std::queue<std::vector<uint8_t>> mInbox;
	std::queue<std::pair<boost::asio::mutable_buffer, ReceiveHandler>> mReceives;

	std::vector<uint8_t> mWriteBuffer;
	std::queue<std::pair<std::vector<uint8_t>, SendHandler>> mOutbox;

	HandshakeHandler mHandshakeHandler;

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