// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Minimal RIFF/WAVE reader+writer for the offline tools.
// Handles PCM 16/24/32-bit and IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE, and
// skips unknown chunks (bext, LIST, iXML...). Deinterleaved double output.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wavio {

struct Audio {
    int sampleRate = 48000;
    int channels = 0;
    std::vector<std::vector<double>> ch;  // ch[c][i]
    size_t frames() const { return ch.empty() ? 0 : ch[0].size(); }
};

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

inline bool read(const std::string& path, Audio& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    long fsz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsz < 44) { std::fclose(f); err = "file too short"; return false; }

    std::vector<uint8_t> hdr(12);
    if (std::fread(hdr.data(), 1, 12, f) != 12) { std::fclose(f); err = "short read"; return false; }
    if (std::memcmp(hdr.data(), "RIFF", 4) || std::memcmp(hdr.data() + 8, "WAVE", 4)) {
        std::fclose(f); err = "not a RIFF/WAVE file"; return false;
    }

    int fmtTag = 0, bits = 0, chans = 0, rate = 0;
    bool haveFmt = false;
    long dataPos = 0; uint32_t dataLen = 0;

    uint8_t ch[8];
    while (std::fread(ch, 1, 8, f) == 8) {
        const uint32_t sz = rd32(ch + 4);
        const long here = std::ftell(f);
        if (!std::memcmp(ch, "fmt ", 4)) {
            std::vector<uint8_t> fmt(sz < 16 ? 16 : sz, 0);
            if (std::fread(fmt.data(), 1, sz, f) != sz) break;
            fmtTag = rd16(fmt.data());
            chans  = rd16(fmt.data() + 2);
            rate   = (int)rd32(fmt.data() + 4);
            bits   = rd16(fmt.data() + 14);
            if (fmtTag == 0xFFFE && sz >= 40) fmtTag = rd16(fmt.data() + 24);  // SubFormat
            haveFmt = true;
        } else if (!std::memcmp(ch, "data", 4)) {
            dataPos = here;
            dataLen = sz;
            if ((long)dataLen > fsz - here) dataLen = (uint32_t)(fsz - here);
        }
        std::fseek(f, here + (long)sz + ((long)sz & 1), SEEK_SET);  // chunks are word-aligned
    }

    if (!haveFmt || !dataPos || chans <= 0) { std::fclose(f); err = "missing fmt/data"; return false; }
    const int bytes = bits / 8;
    if (bytes < 2 || bytes > 8) { std::fclose(f); err = "unsupported bit depth"; return false; }

    const size_t frames = dataLen / (size_t)(bytes * chans);
    out.sampleRate = rate;
    out.channels = chans;
    out.ch.assign((size_t)chans, std::vector<double>(frames, 0.0));

    std::fseek(f, dataPos, SEEK_SET);
    const size_t kBlock = 1 << 16;
    std::vector<uint8_t> buf(kBlock * (size_t)(bytes * chans));
    size_t done = 0;
    while (done < frames) {
        const size_t want = std::min(kBlock, frames - done);
        const size_t got = std::fread(buf.data(), (size_t)(bytes * chans), want, f);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) {
            for (int c = 0; c < chans; ++c) {
                const uint8_t* p = buf.data() + (i * (size_t)chans + (size_t)c) * (size_t)bytes;
                double v = 0.0;
                if (fmtTag == 3) {  // IEEE float
                    if (bytes == 4) { float s; std::memcpy(&s, p, 4); v = s; }
                    else            { double s; std::memcpy(&s, p, 8); v = s; }
                } else {
                    if (bytes == 2) {
                        v = (double)(int16_t)rd16(p) / 32768.0;
                    } else if (bytes == 3) {
                        int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                                              (uint32_t)p[2] << 24);
                        v = (double)(s >> 8) / 8388608.0;
                    } else if (bytes == 4) {
                        v = (double)(int32_t)rd32(p) / 2147483648.0;
                    }
                }
                out.ch[(size_t)c][done + i] = v;
            }
        }
        done += got;
    }
    std::fclose(f);
    return true;
}

inline void wr32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x)); v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 24));
}
inline void wr16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x)); v.push_back((uint8_t)(x >> 8));
}

// bitDepth: 24 (PCM) or 32 (IEEE float). Float output is the default for
// analysis so that nothing is quantised or clipped on the way out.
inline bool write(const std::string& path, const Audio& a, int bitDepth, std::string& err) {
    const int chans = a.channels;
    const size_t frames = a.frames();
    const bool isFloat = (bitDepth == 32);
    const int bytes = bitDepth / 8;
    const uint32_t dataLen = (uint32_t)(frames * (size_t)(chans * bytes));

    std::vector<uint8_t> h;
    h.insert(h.end(), {'R','I','F','F'}); wr32(h, 36 + dataLen);
    h.insert(h.end(), {'W','A','V','E'});
    h.insert(h.end(), {'f','m','t',' '}); wr32(h, 16);
    wr16(h, isFloat ? 3 : 1);
    wr16(h, (uint16_t)chans);
    wr32(h, (uint32_t)a.sampleRate);
    wr32(h, (uint32_t)(a.sampleRate * chans * bytes));
    wr16(h, (uint16_t)(chans * bytes));
    wr16(h, (uint16_t)bitDepth);
    h.insert(h.end(), {'d','a','t','a'}); wr32(h, dataLen);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    std::fwrite(h.data(), 1, h.size(), f);

    const size_t kBlock = 1 << 16;
    std::vector<uint8_t> buf(kBlock * (size_t)(chans * bytes));
    size_t done = 0;
    while (done < frames) {
        const size_t n = std::min(kBlock, frames - done);
        uint8_t* p = buf.data();
        for (size_t i = 0; i < n; ++i) {
            for (int c = 0; c < chans; ++c) {
                const double v = a.ch[(size_t)c][done + i];
                if (isFloat) {
                    float s = (float)v;
                    std::memcpy(p, &s, 4); p += 4;
                } else {
                    double cl = v > 1.0 ? 1.0 : (v < -1.0 ? -1.0 : v);
                    int32_t s = (int32_t)std::lround(cl * 8388607.0);
                    *p++ = (uint8_t)(s); *p++ = (uint8_t)(s >> 8); *p++ = (uint8_t)(s >> 16);
                }
            }
        }
        std::fwrite(buf.data(), 1, (size_t)(p - buf.data()), f);
        done += n;
    }
    std::fclose(f);
    return true;
}

}  // namespace wavio
