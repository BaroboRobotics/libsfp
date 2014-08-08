#ifndef SIMULATEDMEDIUM_HPP
#define SIMULATEDMEDIUM_HPP

#include <tbb/concurrent_queue.h>
#include <boost/optional.hpp>
#include <boost/signals2.hpp>
#include <queue>
#include <atomic>
#include <thread>
#include <cmath>

/* Simulate a physical medium using two buffers (queues) and a callback: a
 * write buffer, an intermediate buffer, and a deliver callback. The user puts as
 * many bytes as s/he wants into the write buffer; a thread (started on object
 * construction, shut down on object destruction) moves bytes from the write
 * buffer to the intermediate buffer, and from the intermediate buffer to the
 * deliver callback, at a fixed time interval.
 *
 * SimulatedMedium has two construction parameters: a propagation delay (in
 * milliseconds), and a baud rate (in bits per second). The size of the
 * intermediate buffer is calculated from these two parameters: it will always
 * have propagation-delay's worth of bytes, given the baud rate, between any
 * two ticks. */
class SimulatedMedium {
private:
    using Quantum = boost::optional<uint8_t>;
    std::queue<Quantum> mMedium;

    using MediumContainer = decltype(mMedium)::container_type;

public:
    using Milliseconds = std::chrono::duration<double, std::chrono::milliseconds::period>;
    using Baud = unsigned;

    SimulatedMedium (Milliseconds propagationDelay, Baud baud)
            // Pack the medium full of propagation-delay's worth of nothing.
            : mMedium(MediumContainer(capacity(propagationDelay, baud), boost::none))
            , mBaud(baud)
            , mThread(&SimulatedMedium::thread, this) {
        // Take advantage of concurrent_bounded_queue's blocking push feature
        // to avoid spinlocking on write.
        mWriteQueue.set_capacity(1);
    }

    ~SimulatedMedium () {
        mKillThread = true;
        mThread.join();
    }

    boost::signals2::signal<void(uint8_t)> output;

    // Block trying to write a byte.
    void input (uint8_t octet) {
        // Since we set the capacity of the write buffer to one, this will
        // block if something's already in there.
        mWriteQueue.push(octet);
    }

private:
    Milliseconds tickInterval () const {
        // Convert bits-per-second to milliseconds-per-byte.
        return Milliseconds { 1000.0 / (double(mBaud) / 8) };
    }

    // Calculate how many bytes will fit in our intermediate buffer based on
    // the baud rate and propagation delay.
    static unsigned capacity (Milliseconds propagationDelay, Baud baud) {
        return std::ceil(double(baud / 8) * propagationDelay / Milliseconds(1000));
    }

    void thread () {
        while (!mKillThread) {
            auto nextTimePoint = std::chrono::steady_clock::now() + tickInterval();

            // If an octet is available from the write buffer, load it into the
            // intermediate buffer. Otherwise, push nothing to keep the pipe
            // flowing.
            uint8_t octet;
            mMedium.push(mWriteQueue.try_pop(octet)
                    ? Quantum(octet)
                    : Quantum(boost::none));

            // Feed the deliver callback if there's an octet available.
            auto quantum = mMedium.front();
            mMedium.pop();
            if (quantum) {
                output(*quantum);
            }

            if (nextTimePoint < std::chrono::steady_clock::now()) {
                fprintf(stderr, "Process is too slow to simulate this baud rate.\n");
            }
            else {
                std::this_thread::sleep_until(nextTimePoint);
            }
        }
    }

    const Baud mBaud;
    tbb::concurrent_bounded_queue<uint8_t> mWriteQueue;
    std::atomic<bool> mKillThread = { false };
    std::thread mThread;
};

#endif
