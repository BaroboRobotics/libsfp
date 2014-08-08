#ifndef SFPHOST_HPP
#define SFPHOST_HPP

#include "serial_framing_protocol.h"

#include <boost/signals2.hpp>

#include <string>
#include <thread>

class SfpHost {
public:
    explicit SfpHost (std::string debugName = { }) {
        sfpInit(&mContext);
        sfpSetDeliverCallback(&mContext, staticDeliver, this);
        sfpSetWriteCallback(&mContext, SFP_WRITE_ONE, (void*)staticWrite, this);
#ifdef SFP_DEBUG
        sfpSetDebugName(&mContext, debugName.c_str());
#endif
    }

    // incoming octet slot
    void input (uint8_t octet) {
        auto rc = sfpDeliverOctet(&mContext, octet, nullptr, 0, nullptr);
        assert(-1 != rc);
    }

    // outgoing octet signal
    boost::signals2::signal<void(uint8_t)> output;

    // outgoing message slot
    void sendMessage (const uint8_t* buf, size_t len) {
        size_t outlen;
        sfpWritePacket(&mContext, buf, len, &outlen);
    }

    // incoming message signal
    boost::signals2::signal<void(uint8_t*,size_t)> messageReceived;

    void connect () {
        sfpConnect(&mContext);
        waitUntilConnected();
    }

    void waitUntilConnected () {
        while (!isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool isConnected () {
        return sfpIsConnected(&mContext);
    }

private:
    static void staticDeliver (uint8_t* buf, size_t len, void* data) {
        static_cast<SfpHost*>(data)->messageReceived(buf, len);
    }

    static int staticWrite (uint8_t octet, size_t* outlen, void* data) {
        if (outlen) { *outlen = 1; }
        static_cast<SfpHost*>(data)->output(octet);
        return 0;
    }

    SFPcontext mContext;
};

#endif
