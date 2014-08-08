#ifndef GARBLER_HPP
#define GARBLER_HPP

#include "sfp/callback.hpp"

#include <random>
#include <cstdint>

class Garbler {
public:
    Garbler (double byteDropProbability, double bitFlipProbability)
            : mByteDropProbability(byteDropProbability)
            , mBitFlipProbability(bitFlipProbability)
            , randomEngine(randomDevice()) { }

    void input (uint8_t octet) {
        if (!dropByte()) {
            output(flipBits(octet));
        }
    }

    util::Signal<void(uint8_t)> output;

private:
    bool dropByte () {
        return std::generate_canonical<double,10>(randomEngine) < mByteDropProbability;
    }

    uint8_t flipBits (uint8_t octet) {
        for (int i = 0; i < 8; ++i) {
            if (std::generate_canonical<double,10>(randomEngine) < mBitFlipProbability) {
                octet ^= (1 << i);
            }
        }
        return octet;
    }

    const double mByteDropProbability;
    const double mBitFlipProbability;

    std::random_device randomDevice;
    std::default_random_engine randomEngine;
};

#endif
