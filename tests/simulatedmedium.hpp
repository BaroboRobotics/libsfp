#ifndef SIMULATEDMEDIUM_HPP
#define SIMULATEDMEDIUM_HPP

#include <tbb/concurrent_queue.h>
#include <boost/optional.hpp>
#include <queue>
#include <atomic>
#include <thread>
#include <cmath>

/* Simulate a physical medium using three buffers (queues): a write buffer, an
 * intermediate buffer, and a read buffer. The user puts as many bytes as s/he
 * wants into the write buffer; a thread (started on object construction, shut
 * down on object destruction) moves bytes from the write buffer to the
 * intermediate buffer, and from the intermediate buffer to the read buffer, at
 * a fixed time interval.
 *
 * SimulatedMedium has two construction parameters: a propagation delay (in
 * milliseconds), and a baud rate (in bits per second). The size of the
 * intermediate buffer is calculated from these two parameters: it will always
 * have propagation-delay's worth of bytes, given the baud rate, between any
 * two ticks.
 *
 * Calling write and read from separate threads is safe. Writes and reads block
 * until the request is satisfied. */
class SimulatedMedium {
private:
    using Quantum = boost::optional<uint8_t>;
    std::queue<Quantum> mMedium;

    using MediumContainer = decltype(mMedium)::container_type;

public:
    using Milliseconds = std::chrono::duration<double, std::chrono::milliseconds::period>;

    SimulatedMedium (Milliseconds propagationDelay, unsigned baud)
            // Pack the medium full of propagation-delay's worth of nothing.
            : mMedium(MediumContainer(capacity(propagationDelay, baud), boost::none))
            , mBaud(baud)
            , mThread(&SimulatedMedium::serviceThread, this) {
        // Take advantage of concurrent_bounded_queue's blocking push feature
        // to avoid spinlocking on write.
        mWriteQueue.set_capacity(1);
    }

    ~SimulatedMedium () {
        mKillThread = true;
        mThread.join();
    }

    // Block trying to read a byte.
    uint8_t read () {
        uint8_t octet;
        mReadQueue.pop(octet);
        return octet;
    }

    // Block trying to write a byte.
    void write (uint8_t octet) {
        // Since we set the capacity of the write buffer to one, this will
        // block if something's already in there.
        mWriteQueue.push(octet);
    }

    void write (std::string s) {
        for (auto c : s) {
            write(static_cast<uint8_t>(c));
        }
    }

private:
    Milliseconds tickInterval () const {
        // Convert bits-per-second to milliseconds-per-byte.
        return Milliseconds { 1000.0 / (double(mBaud) / 8) };
    }

    // Calculate how many bytes will fit in our intermediate buffer based on
    // the baud rate and propagation delay.
    static unsigned capacity (Milliseconds propagationDelay, unsigned baud) {
        return std::ceil(double(baud / 8) * propagationDelay / Milliseconds(1000));
    }

    void serviceThread () {
        while (!mKillThread) {
            auto nextTimePoint = std::chrono::steady_clock::now() + tickInterval();

            // If an octet is available from the write buffer, load it into the
            // intermediate buffer. Otherwise, push nothing to keep the pipe
            // flowing.
            uint8_t octet;
            mMedium.push(mWriteQueue.try_pop(octet)
                    ? Quantum(octet)
                    : Quantum(boost::none));

            // Feed the read buffer if there's an octet available.
            auto quantum = mMedium.front();
            mMedium.pop();
            if (quantum) {
                mReadQueue.push(*quantum);
            }

            assert(nextTimePoint > std::chrono::steady_clock::now() &&
                    "Process is too slow to simulate this baud rate.");
            std::this_thread::sleep_until(nextTimePoint);
        }
    }

    const unsigned mBaud;

    tbb::concurrent_bounded_queue<uint8_t> mWriteQueue;
    tbb::concurrent_bounded_queue<uint8_t> mReadQueue;

    std::atomic<bool> mKillThread = { false };

    std::thread mThread;
};

#endif
