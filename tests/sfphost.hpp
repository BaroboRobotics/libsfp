#ifndef SFPHOST_HPP
#define SFPHOST_HPP

#include "serial_framing_protocol.h"

#include "sfp/callback.hpp"

#include <string>
#include <thread>
#include <mutex>

class SfpHost {
public:
    explicit SfpHost (std::string debugName = { })
            : mDebugName(debugName) {
        sfpInit(&mContext);
        sfpSetDeliverCallback(&mContext, staticDeliver, this);
        sfpSetWriteCallback(&mContext, SFP_WRITE_ONE, (void*)staticWrite, this);
        sfpSetLockCallback(&mContext, staticLock, this);
        sfpSetUnlockCallback(&mContext, staticUnlock, this);
#ifdef SFP_CONFIG_DEBUG
        sfpSetDebugName(&mContext, debugName.c_str());
#endif
    }

    // incoming octet slot
    void input (uint8_t octet) {
        auto rc = sfpDeliverOctet(&mContext, octet, nullptr, 0, nullptr);
        assert(-1 != rc);
    }

    // outgoing octet signal
    util::Signal<void(uint8_t)> output;

    // outgoing message slot
    void sendMessage (const uint8_t* buf, size_t len) {
        size_t outlen;
        sfpWritePacket(&mContext, buf, len, &outlen);
    }

    // incoming message signal
    util::Signal<void(const uint8_t*,size_t)> messageReceived;

    void connect () {
        sfpConnect(&mContext);
        waitUntilConnected();
    }

    void waitUntilConnected () {
        while (!isConnected()) {
            //sfpConnect(&mContext);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool isConnected () {
        return sfpIsConnected(&mContext);
    }

    void pump () {
        sendMessage(nullptr, 0);
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

    static void staticLock (void* data) {
        static_cast<SfpHost*>(data)->mTransmitterMutex.lock();
    }

    static void staticUnlock (void* data) {
        static_cast<SfpHost*>(data)->mTransmitterMutex.unlock();
    }

    SFPcontext mContext;
    std::mutex mTransmitterMutex;
    std::string mDebugName;
};

#endif
