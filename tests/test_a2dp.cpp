#include "a2dp/avdtp.h"
#include "a2dp/codec_caps.h"
#include "a2dp/media.h"
#include "testing.h"

using namespace bta;
using namespace bta::a2dp;
using namespace bta::avdtp;

TEST(CodecCaps, SbcParseBuild)
{
    std::vector<uint8_t> info{0x3F, 0xFF, 0x02, 0x35};  // 44.1/48, all modes, all blocks/subbands/alloc, 2..53
    auto c = SbcCaps::Parse(info);
    REQUIRE(c.has_value());
    CHECK_EQ(int(c->freq), 0x30);
    CHECK_EQ(int(c->channelModes), 0x0F);
    CHECK_EQ(int(c->maxBitpool), 53);
    CHECK(c->Build() == info);
    CHECK(!SbcCaps::Parse(std::vector<uint8_t>{1, 2, 3}).has_value());
}

TEST(CodecCaps, LdacParseIdentify)
{
    LdacCaps l{ldacbits::kFreq48000 | ldacbits::kFreq96000, ldacbits::kStereo};
    auto cap = l.ToCapability();
    CHECK(cap.Identify() == CodecId::Ldac);
    auto ser = cap.Serialize();
    // 00 FF 2D 01 00 00 AA 00 SF CM  (LOSC 10 per AOSP A2DP_LDAC_CODEC_LEN)
    CHECK_EQ(ser.size(), size_t(10));
    CHECK_EQ(int(ser[1]), 0xFF);
    CHECK_EQ(int(ser[2]), 0x2D);
    CHECK_EQ(int(ser[3]), 0x01);
    CHECK_EQ(int(ser[6]), 0xAA);
    auto back = LdacCaps::Parse(cap.info);
    REQUIRE(back.has_value());
    CHECK_EQ(int(back->freq), int(l.freq));
    // Wrong vendor is not LDAC
    auto other = cap;
    other.info[0] = 0x4F;
    CHECK(other.Identify() != CodecId::Ldac);
    CHECK(!LdacCaps::Parse(other.info).has_value());
}

TEST(CodecCaps, NegotiateSbcPrefersSourceRateAndJointStereo)
{
    SbcCaps sink{sbcbits::kFreq44100 | sbcbits::kFreq48000, 0x0F, 0xF0, 0x0C, 0x03, 2, 53};
    NegotiationPrefs p;
    p.sourceRate = 44100;
    auto r = NegotiateSbc(sink, p);
    REQUIRE(r.ok());
    CHECK_EQ(r->sampleRate, 44100u);
    CHECK_EQ(int(r->sbc.channelModes), int(sbcbits::kJoint));
    CHECK_EQ(int(r->sbc.blocks), int(sbcbits::kBlocks16));
    CHECK_EQ(int(r->sbc.subbands), int(sbcbits::kSubbands8));
    CHECK_EQ(int(r->sbc.maxBitpool), 53);
    CHECK_EQ(r->SamplesPerCodecFrame(), 128u);
    // explicit preference wins
    p.sampleRate = 48000;
    CHECK_EQ(NegotiateSbc(sink, p)->sampleRate, 48000u);
    // sink bitpool limit respected
    sink.maxBitpool = 35;
    CHECK_EQ(int(NegotiateSbc(sink, p)->sbc.maxBitpool), 35);
    // mono-only sink
    sink.channelModes = sbcbits::kMono;
    auto m = NegotiateSbc(sink, p);
    REQUIRE(m.ok());
    CHECK_EQ(int(m->channels), 1);
    // no common rate
    sink.freq = 0;
    CHECK(!NegotiateSbc(sink, p).ok());
}

TEST(CodecCaps, NegotiateSbcBitpoolValidation)
{
    SbcCaps sink{sbcbits::kFreq48000, sbcbits::kJoint, sbcbits::kBlocks16, sbcbits::kSubbands8, sbcbits::kAllocLoudness, 60, 40};
    CHECK(!NegotiateSbc(sink, {}).ok());  // min > max is invalid
}

TEST(CodecCaps, NegotiateLdac)
{
    LdacCaps sink{0x3C, 0x07};
    NegotiationPrefs p;
    p.sourceRate = 48000;
    p.ldacQuality = LdacQuality::High;
    auto r = NegotiateLdac(sink, p);
    REQUIRE(r.ok());
    CHECK_EQ(r->sampleRate, 48000u);
    CHECK_EQ(int(r->ldac.channelModes), int(ldacbits::kStereo));
    CHECK_EQ(r->ldacEqmid, 0);
    CHECK(!r->ldacAbr);
    CHECK_EQ(r->SamplesPerCodecFrame(), 128u);
    p.sampleRate = 96000;
    p.ldacQuality = LdacQuality::Adaptive;
    r = NegotiateLdac(sink, p);
    CHECK_EQ(r->sampleRate, 96000u);
    CHECK(r->ldacAbr);
    CHECK_EQ(r->SamplesPerCodecFrame(), 256u);
    // 44.1k family source picks 88.2 over 48 when 44.1 is absent
    LdacCaps hi{ldacbits::kFreq48000 | ldacbits::kFreq88200, ldacbits::kStereo};
    p.sampleRate = 0;
    p.sourceRate = 44100;
    CHECK_EQ(NegotiateLdac(hi, p)->sampleRate, 88200u);
    // 192 kHz only sink: unsupported by libldac
    CHECK(!NegotiateLdac(LdacCaps{ldacbits::kFreq192000, ldacbits::kStereo}, p).ok());
}

TEST(CodecCaps, ConfigFromCapabilityRequiresSingleValues)
{
    SbcCaps multi{0x30, 0x01, 0x10, 0x04, 0x01, 2, 53};
    CHECK(!ConfigFromCapability(multi.ToCapability(), {}).ok());
    SbcCaps single{0x10, 0x01, 0x10, 0x04, 0x01, 2, 53};
    auto r = ConfigFromCapability(single.ToCapability(), {});
    REQUIRE(r.ok());
    CHECK_EQ(r->sampleRate, 48000u);
    LdacCaps l{ldacbits::kFreq44100, ldacbits::kStereo};
    auto lr = ConfigFromCapability(l.ToCapability(), {});
    REQUIRE(lr.ok());
    CHECK_EQ(lr->sampleRate, 44100u);
}

TEST(Avdtp, SinglePacketRoundTrip)
{
    Message m{5, MsgType::Command, Signal::GetAllCapabilities, {0x04}};
    auto pkts = EncodeMessage(m);
    REQUIRE(pkts.size() == 1);
    CHECK_EQ(int(pkts[0][0]), 0x50);  // label 5, single, command
    CHECK_EQ(int(pkts[0][1]), 0x0C);
    Reassembler r;
    auto out = r.Feed(pkts[0]);
    REQUIRE(out.has_value());
    CHECK_EQ(int(out->label), 5);
    CHECK(out->signal == Signal::GetAllCapabilities);
    CHECK(out->payload == m.payload);
}

TEST(Avdtp, FragmentationRoundTrip)
{
    Message m{3, MsgType::Accept, Signal::GetAllCapabilities, {}};
    for (int i = 0; i < 300; ++i) m.payload.push_back(static_cast<uint8_t>(i));
    auto pkts = EncodeMessage(m, 48);
    REQUIRE(pkts.size() > 2);
    for (auto& p : pkts) CHECK(p.size() <= 48);
    CHECK_EQ(int(pkts[0][1]), int(pkts.size()));  // NOSP
    Reassembler r;
    std::optional<Message> out;
    for (auto& p : pkts) {
        std::string err;
        out = r.Feed(p, &err);
        CHECK(err.empty());
    }
    REQUIRE(out.has_value());
    CHECK(out->payload == m.payload);
    CHECK(out->type == MsgType::Accept);
}

TEST(Avdtp, ReassemblerRejectsBadSequences)
{
    Reassembler r;
    std::string err;
    CHECK(!r.Feed(std::vector<uint8_t>{0x38}, &err).has_value());  // continue without start
    CHECK(!err.empty());
    Message m{1, MsgType::Accept, Signal::Discover, std::vector<uint8_t>(100, 0x11)};
    auto pkts = EncodeMessage(m, 40);
    err.clear();
    r.Feed(pkts[0], &err);
    std::vector<uint8_t> wrongLabel = pkts[1];
    wrongLabel[0] = static_cast<uint8_t>((wrongLabel[0] & 0x0F) | 0x70);
    CHECK(!r.Feed(wrongLabel, &err).has_value());
    CHECK(!err.empty());
}

TEST(Avdtp, DiscoverCodec)
{
    std::vector<SepInfo> seps{{1, false, 0, true}, {2, true, 0, true}, {5, false, 1, false}};
    auto bytes = BuildDiscoverResponse(seps);
    auto back = ParseDiscoverResponse(bytes);
    REQUIRE(back.has_value());
    REQUIRE(back->size() == 3);
    CHECK_EQ(int((*back)[1].seid), 2);
    CHECK((*back)[1].inUse);
    CHECK((*back)[0].isSink);
    CHECK(!(*back)[2].isSink);
    CHECK_EQ(int((*back)[2].mediaType), 1);
    CHECK(!ParseDiscoverResponse(std::vector<uint8_t>{0x04}).has_value());
    CHECK(!ParseDiscoverResponse(std::vector<uint8_t>{0x00, 0x08}).has_value());  // SEID 0 invalid
}

TEST(Avdtp, CapabilitiesRoundTripAndStrictness)
{
    Capabilities c;
    c.mediaTransport = true;
    c.delayReporting = true;
    c.codecs.push_back(LocalLdacCaps().ToCapability());
    auto bytes = c.Serialize();
    // 01 00 | 07 0A 00 FF 2D 01 00 00 AA 00 SF CM | 08 00
    CHECK_EQ(int(bytes[2]), 0x07);
    CHECK_EQ(int(bytes[3]), 0x0A);
    auto back = Capabilities::Parse(bytes);
    REQUIRE(back.has_value());
    CHECK(back->mediaTransport && back->delayReporting);
    CHECK(back->codecs.size() == 1 && back->codecs[0].Identify() == CodecId::Ldac);

    std::vector<uint8_t> unknown = bytes;
    unknown.push_back(0x2A);
    unknown.push_back(0x00);
    uint8_t bad = 0;
    CHECK(!Capabilities::Parse(unknown, &bad, true).has_value());
    CHECK_EQ(int(bad), 0x2A);
    CHECK(Capabilities::Parse(unknown, nullptr, false).has_value());
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 6);
    CHECK(!Capabilities::Parse(truncated).has_value());
}

TEST(Media, RtpPacketizeParse)
{
    RtpPacketizer rtp(0x11223344);
    std::vector<uint8_t> payload{0x9C, 1, 2, 3};
    auto p1 = rtp.Build(payload, 3, 384);
    auto p2 = rtp.Build(payload, 1, 128);
    auto m1 = ParseMediaPacket(p1), m2 = ParseMediaPacket(p2);
    REQUIRE(m1 && m2);
    CHECK_EQ(int(p1[0]), 0x80);
    CHECK_EQ(int(m1->payloadType), 96);
    CHECK_EQ(int(m1->frames), 3);
    CHECK_EQ(m1->ssrc, 0x11223344u);
    CHECK_EQ(int(m2->sequence), int(m1->sequence) + 1);
    CHECK_EQ(m2->timestamp, m1->timestamp + 384);
    CHECK(m1->payload.size() == payload.size());
    CHECK(!ParseMediaPacket(std::vector<uint8_t>(5, 0)).has_value());
}

TEST(Media, LdacHeaderParse)
{
    // sync AA, sfid=1 (48k), cci=2 (stereo), frame_length=330 -> stores 329, status 0
    uint32_t bits = (1u << 13) | (2u << 11) | (329u << 2);
    std::vector<uint8_t> f{0xAA, static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits)};
    auto h = ParseLdacFrameHeader(f);
    REQUIRE(h.has_value());
    CHECK_EQ(int(h->sampleRateId), 1);
    CHECK_EQ(int(h->channelConfigId), 2);
    CHECK_EQ(int(h->frameLength), 330);
    f[0] = 0xAB;
    CHECK(!ParseLdacFrameHeader(f).has_value());
}
