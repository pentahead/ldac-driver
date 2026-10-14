// Codec, PCM and pipeline unit tests (offline; real libsbc/libldac).
#include <sbc.h>

#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

#include "a2dp/media.h"
#include "codec/encoder.h"
#include "codec/pcm.h"
#include "pipeline/pcm_ring.h"
#include "testing.h"

using namespace bta;
using namespace bta::a2dp;

namespace {

// Goertzel power at f relative to total power.
double ToneRatio(const std::vector<float>& x, double rate, double f)
{
    double w = 2 * std::numbers::pi * f / rate, c = 2 * std::cos(w), s1 = 0, s2 = 0, total = 0;
    for (float v : x) {
        double s = v + c * s1 - s2;
        s2 = s1;
        s1 = s;
        total += v * v;
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return total > 0 ? (2 * p / x.size()) / total : 0;
}

StreamConfig SbcCfg(uint32_t rate = 48000, int bitpool = 53)
{
    SbcCaps sink{sbcbits::kFreq44100 | sbcbits::kFreq48000, 0x0F, 0xF0, 0x0C, 0x03, 2, static_cast<uint8_t>(bitpool)};
    NegotiationPrefs p;
    p.sampleRate = rate;
    return *NegotiateSbc(sink, p);
}

StreamConfig LdacCfg(uint32_t rate, LdacQuality q, uint8_t bits = 24)
{
    NegotiationPrefs p;
    p.sampleRate = rate;
    p.ldacQuality = q;
    p.bitsPerSample = bits;
    return *NegotiateLdac(LdacCaps{0x3C, 0x07}, p);
}

}  // namespace

TEST(Pcm, FloatConversionSaturates)
{
    std::vector<float> in{0.0f, 0.5f, -0.5f, 1.5f, -1.5f, NAN, 1.0f};
    int16_t o16[7];
    FloatToS16(in, o16);
    CHECK_EQ(o16[0], 0);
    CHECK_EQ(o16[1], 16384);
    CHECK_EQ(o16[2], -16384);
    CHECK_EQ(o16[3], 32767);
    CHECK_EQ(o16[4], -32768);
    CHECK_EQ(o16[5], 0);
    CHECK_EQ(o16[6], 32767);
    int32_t o32[7];
    FloatToS32(in, o32);
    CHECK_EQ(o32[3], INT32_MAX);
    CHECK_EQ(o32[4], INT32_MIN);
}

TEST(Pcm, ChannelRemap)
{
    std::vector<float> mono{0.1f, 0.2f}, out;
    RemapChannels(mono, 1, out, 2);
    CHECK(out.size() == 4 && out[2] == 0.2f && out[3] == 0.2f);
    std::vector<float> st{0.2f, 0.4f};
    RemapChannels(st, 2, out, 1);
    CHECK_NEAR(out[0], 0.3, 1e-6);
    std::vector<float> six{1, 0, 0, 0, 0, 0};
    RemapChannels(six, 6, out, 2);
    CHECK_NEAR(out[0], 1.0, 1e-6);
    CHECK_NEAR(out[1], 0.0, 1e-6);
}

TEST(Pcm, ResamplerPreservesToneAndRate)
{
    for (auto [in, out] : {std::pair{44100u, 48000u}, std::pair{48000u, 44100u}, std::pair{48000u, 96000u}}) {
        Resampler rs(in, out, 1);
        std::vector<float> x(in), y;
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * i / in));
        // Feed in odd-sized chunks to exercise streaming state.
        for (size_t pos = 0; pos < x.size();) {
            size_t n = std::min<size_t>(777, x.size() - pos);
            rs.Process(std::span(x).subspan(pos, n), y);
            pos += n;
        }
        CHECK_NEAR(static_cast<double>(y.size()), out, out * 0.002);
        std::vector<float> mid(y.begin() + 1000, y.end() - 1000);
        CHECK(ToneRatio(mid, out, 1000) > 0.99);
    }
}

TEST(PcmRing, WrapAroundAndOverflow)
{
    PcmRing r(10, 2);
    std::vector<float> a(16), b(16);
    for (int i = 0; i < 16; ++i) a[i] = static_cast<float>(i);
    CHECK_EQ(r.Write(a.data(), 8), size_t(8));
    CHECK_EQ(r.Read(b.data(), 6), size_t(6));
    CHECK(b[0] == 0 && b[11] == 11);
    CHECK_EQ(r.Write(a.data(), 8), size_t(8));  // wraps
    CHECK_EQ(r.Available(), size_t(10));
    CHECK_EQ(r.Write(a.data(), 3), size_t(0));  // full -> overflow counted, no overwrite
    CHECK_EQ(r.OverflowFrames(), uint64_t(3));
    CHECK_EQ(r.Read(b.data(), 8), size_t(8));
    CHECK(b[0] == 12 && b[1] == 13);  // frames 6,7 of first write
    CHECK(b[4] == 0 && b[5] == 1);    // start of second write
}

TEST(PcmRing, ConcurrentIntegrity)
{
    PcmRing r(1024, 2);
    constexpr uint32_t kFrames = 2'000'000;
    std::atomic<bool> bad{false};
    std::thread prod([&] {
        std::vector<float> buf(2 * 97);
        uint32_t next = 0;
        while (next < kFrames) {
            size_t n = std::min<uint32_t>(97, kFrames - next);
            for (size_t i = 0; i < n; ++i) buf[2 * i] = buf[2 * i + 1] = static_cast<float>(next + i);
            size_t off = 0;
            while (off < n) off += r.Write(buf.data() + 2 * off, n - off);
            next += static_cast<uint32_t>(n);
        }
    });
    std::vector<float> buf(2 * 64);
    uint32_t expect = 0;
    while (expect < kFrames) {
        size_t n = r.Read(buf.data(), 64);
        for (size_t i = 0; i < n; ++i) {
            if (buf[2 * i] != static_cast<float>(expect) || buf[2 * i + 1] != buf[2 * i]) bad = true;
            ++expect;
        }
    }
    prod.join();
    CHECK(!bad);  // every frame arrived exactly once, in order, untorn (retried writes count as overflow)
}

TEST(SbcEncoder, RoundTripDecodeQuality)
{
    auto cfg = SbcCfg(48000);
    auto enc = MakeSbcEncoder();
    REQUIRE_OK(enc->Open(cfg, 895));
    CHECK_EQ(enc->InputFramesPerCall(), 128u);
    CHECK(enc->InputFormat() == SampleFormat::S16);
    const int frames = 48000;  // 1 s
    std::vector<int16_t> pcm(2 * 128);
    std::vector<EncodedPayload> out;
    std::vector<float> ref;
    for (int pos = 0; pos + 128 <= frames; pos += 128) {
        for (int i = 0; i < 128; ++i) {
            float v = 0.4f * static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * (pos + i) / 48000.0));
            pcm[2 * i] = pcm[2 * i + 1] = static_cast<int16_t>(v * 32767);
            ref.push_back(v);
        }
        REQUIRE_OK(enc->Encode(pcm.data(), out));
    }
    enc->Flush(out);
    REQUIRE(!out.empty());
    sbc_t dec;
    sbc_reset(&dec);
    std::vector<float> decoded;
    size_t totalFrames = 0;
    for (auto& p : out) {
        CHECK(p.bytes.size() + kMediaOverhead <= 895);
        CHECK(p.frames >= 1 && p.frames <= 15);
        size_t off = 0;
        for (int f = 0; f < p.frames; ++f) {
            sbc_frame fr;
            REQUIRE(sbc_probe(p.bytes.data() + off, &fr) == 0);
            CHECK_EQ(fr.bitpool, 53);
            unsigned sz = sbc_get_frame_size(&fr);
            int16_t l[SBC_MAX_SAMPLES], r[SBC_MAX_SAMPLES];
            REQUIRE(sbc_decode(&dec, p.bytes.data() + off, sz, &fr, l, 1, r, 1) == 0);
            for (int i = 0; i < fr.nblocks * fr.nsubbands; ++i) decoded.push_back(l[i] / 32768.0f);
            off += sz;
            ++totalFrames;
        }
        CHECK_EQ(off, p.bytes.size());
    }
    CHECK_EQ(totalFrames, size_t(frames / 128));
    // Measure SNR after compensating the codec delay (search best lag).
    double best = -1e9;
    for (int lag = 0; lag < 200; ++lag) {
        double sig = 0, err = 0;
        for (size_t i = 2000; i + lag < decoded.size() && i < ref.size() - 200; ++i) {
            double d = decoded[i + lag] - ref[i];
            sig += ref[i] * ref[i];
            err += d * d;
        }
        best = std::max(best, 10 * std::log10(sig / std::max(err, 1e-20)));
    }
    printf("      SBC bitpool 53 round-trip SNR: %.1f dB, %zu frames, bitrate %u kbps\n", best, totalFrames,
           enc->NominalBitrate() / 1000);
    CHECK(best > 30.0);
}

TEST(SbcEncoder, LiveBitpoolChangeAndLimits)
{
    auto enc = MakeSbcEncoder();
    REQUIRE_OK(enc->Open(SbcCfg(44100, 53), 895));
    uint32_t before = enc->NominalBitrate();
    REQUIRE_OK(enc->SetQuality(35));
    CHECK(enc->NominalBitrate() < before);
    CHECK(!enc->SetQuality(60).ok());  // above configured max
    CHECK(!enc->Open(SbcCfg(48000, 53), 40).ok());  // MTU smaller than one frame
}

TEST(LdacEncoder, FramesBitrateAndHeaders)
{
    struct Case {
        uint32_t rate;
        LdacQuality q;
        uint8_t bits;
        double kbps;
    };
    for (const Case& c : {Case{48000, LdacQuality::High, 24, 990}, Case{48000, LdacQuality::Standard, 16, 660},
                          Case{48000, LdacQuality::Mobile, 24, 330}, Case{44100, LdacQuality::High, 16, 909},
                          Case{96000, LdacQuality::High, 24, 990}, Case{88200, LdacQuality::Standard, 24, 606}}) {
        auto cfg = LdacCfg(c.rate, c.q, c.bits);
        auto enc = MakeLdacEncoder();
        REQUIRE_OK(enc->Open(cfg, 1005));
        CHECK_EQ(enc->InputFramesPerCall(), 128u);
        const size_t bps = c.bits == 16 ? 2 : 4;
        std::vector<uint8_t> pcm(128 * 2 * bps);
        std::vector<EncodedPayload> out;
        uint64_t pcmIn = 0;
        for (uint32_t call = 0; pcmIn < c.rate * 2ull; ++call) {  // 2 s
            for (int i = 0; i < 128; ++i) {
                double v = 0.3 * std::sin(2 * std::numbers::pi * 1000 * (pcmIn + i) / c.rate);
                if (bps == 2) {
                    int16_t s = static_cast<int16_t>(v * 32767);
                    memcpy(&pcm[(2 * i) * 2], &s, 2);
                    memcpy(&pcm[(2 * i + 1) * 2], &s, 2);
                } else {
                    int32_t s = static_cast<int32_t>(v * 2147483647.0);
                    memcpy(&pcm[(2 * i) * 4], &s, 4);
                    memcpy(&pcm[(2 * i + 1) * 4], &s, 4);
                }
            }
            REQUIRE_OK(enc->Encode(pcm.data(), out));
            pcmIn += 128;
        }
        REQUIRE(!out.empty());
        uint64_t bytes = 0, frames = 0, pcmOut = 0;
        bool headersOk = true, countOk = true, sizeOk = true;
        for (auto& p : out) {
            sizeOk &= p.bytes.size() + kMediaOverhead <= 1005 && p.bytes.size() <= 661;
            size_t off = 0, n = 0;
            while (off < p.bytes.size()) {
                auto h = ParseLdacFrameHeader(std::span(p.bytes).subspan(off));
                if (!h || off + h->TotalSize() > p.bytes.size()) {
                    headersOk = false;
                    break;
                }
                off += h->TotalSize();
                ++n;
            }
            countOk &= (n == p.frames) && off == p.bytes.size();
            bytes += p.bytes.size();
            frames += p.frames;
            pcmOut += p.pcmFrames;
        }
        CHECK(headersOk);
        CHECK(countOk);
        CHECK(sizeOk);
        double seconds = static_cast<double>(pcmOut) / c.rate;
        double kbps = bytes * 8 / 1000.0 / seconds;
        printf("      LDAC %u Hz %s %u-bit: %zu packets, %llu frames, %.0f kbps (nominal %.0f), label %s\n", c.rate,
               LdacQualityName(c.q), c.bits, out.size(), static_cast<unsigned long long>(frames), kbps, c.kbps,
               enc->QualityLabel().c_str());
        CHECK_NEAR(kbps, c.kbps, c.kbps * 0.05);
        CHECK_NEAR(enc->NominalBitrate() / 1000.0, c.kbps, c.kbps * 0.02);
    }
}

TEST(LdacEncoder, RejectsSmallMtuAndLiveQuality)
{
    auto cfg = LdacCfg(48000, LdacQuality::High);
    auto enc = MakeLdacEncoder();
    CHECK(!enc->Open(cfg, 600).ok());  // libldac requires >= 679
    REQUIRE_OK(enc->Open(cfg, 895));
    REQUIRE_OK(enc->SetQuality(2));
    CHECK(enc->QualityLabel() == "MQ");
    // libldac applies the new EQMID at the next frame; encode a little, then check the bitrate.
    std::vector<int32_t> pcm(128 * 2, 0);
    std::vector<EncodedPayload> out;
    for (int i = 0; i < 20; ++i) REQUIRE_OK(enc->Encode(pcm.data(), out));
    CHECK_NEAR(enc->NominalBitrate() / 1000.0, 330, 10);
    CHECK(!enc->SetQuality(7).ok());
}

TEST(Encoders, Availability)
{
    CHECK(IsEncoderAvailable(CodecId::Sbc));
    std::string why;
    CHECK(IsEncoderAvailable(CodecId::Ldac, &why));
    CHECK(!IsEncoderAvailable(CodecId::Aac, &why));
    CHECK(CreateEncoder(CodecId::Aac) == nullptr);
}
