// AVDTP 1.3 signalling PDUs: header coding, fragmentation/reassembly, service capabilities.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "a2dp/codec_caps.h"

namespace bta::avdtp {

constexpr uint16_t kPsmAvdtp = 0x0019;
constexpr uint16_t kDefaultSignalMtu = 672;

enum class Signal : uint8_t {
    Discover = 0x01,
    GetCapabilities = 0x02,
    SetConfiguration = 0x03,
    GetConfiguration = 0x04,
    Reconfigure = 0x05,
    Open = 0x06,
    Start = 0x07,
    Close = 0x08,
    Suspend = 0x09,
    Abort = 0x0A,
    SecurityControl = 0x0B,
    GetAllCapabilities = 0x0C,
    DelayReport = 0x0D,
};
const char* SignalName(Signal s);

enum class MsgType : uint8_t { Command = 0, GeneralReject = 1, Accept = 2, Reject = 3 };
enum class PacketType : uint8_t { Single = 0, Start = 1, Continue = 2, End = 3 };

enum class Category : uint8_t {
    MediaTransport = 0x01,
    Reporting = 0x02,
    Recovery = 0x03,
    ContentProtection = 0x04,
    HeaderCompression = 0x05,
    Multiplexing = 0x06,
    MediaCodec = 0x07,
    DelayReporting = 0x08,
};

// Error codes (AVDTP sec. 8.20.6.2 and A2DP sec. 5.1.3).
namespace error {
constexpr uint8_t kBadHeaderFormat = 0x01, kBadLength = 0x11, kBadAcpSeid = 0x12, kSepInUse = 0x13,
                  kSepNotInUse = 0x14, kBadServCategory = 0x17, kBadPayloadFormat = 0x18,
                  kNotSupportedCommand = 0x19, kInvalidCapabilities = 0x1A, kUnsupportedConfiguration = 0x29,
                  kBadState = 0x31, kInvalidCodecType = 0xC1, kNotSupportedCodecType = 0xC2,
                  kInvalidSamplingFrequency = 0xC3;
}
std::string ErrorName(uint8_t code);

// A complete (reassembled) signalling message.
struct Message {
    uint8_t label = 0;  // transaction label 0..15
    MsgType type = MsgType::Command;
    Signal signal = Signal::Discover;
    std::vector<uint8_t> payload;
};

// Splits a message into L2CAP-MTU sized packets (sec. 8.4).
std::vector<std::vector<uint8_t>> EncodeMessage(const Message& m, uint16_t mtu = kDefaultSignalMtu);

// Reassembles incoming packets. Feed() returns a message when complete.
class Reassembler {
public:
    // Returns nullopt while incomplete; sets *error on malformed input (state is reset).
    std::optional<Message> Feed(std::span<const uint8_t> packet, std::string* error = nullptr);
    void Reset();

private:
    bool active_ = false;
    Message cur_;
    uint8_t remaining_ = 0;
};

// Stream End Point descriptor from DISCOVER (sec. 8.6.2).
struct SepInfo {
    uint8_t seid = 0;    // 1..62
    bool inUse = false;
    uint8_t mediaType = 0;
    bool isSink = false;  // TSEP: 1 = SNK
};
std::vector<uint8_t> BuildDiscoverResponse(const std::vector<SepInfo>& seps);
std::optional<std::vector<SepInfo>> ParseDiscoverResponse(std::span<const uint8_t> payload);

struct ServiceCapability {
    Category category;
    std::vector<uint8_t> data;  // LOSC bytes
};

struct Capabilities {
    bool mediaTransport = false;
    bool delayReporting = false;
    bool contentProtection = false;
    std::vector<a2dp::CodecCapability> codecs;  // normally exactly one per SEP
    std::vector<ServiceCapability> other;

    std::vector<uint8_t> Serialize() const;
    // Parses a capability list. *badCategory receives the first unknown category on failure.
    // strict=false skips unknown categories (lenient parsing of remote GET_*CAPABILITIES responses).
    static std::optional<Capabilities> Parse(std::span<const uint8_t> data, uint8_t* badCategory = nullptr,
                                             bool strict = true);
};

// Helpers for common payloads.
inline uint8_t SeidByte(uint8_t seid) { return static_cast<uint8_t>(seid << 2); }
inline uint8_t SeidFromByte(uint8_t b) { return static_cast<uint8_t>(b >> 2); }

}  // namespace bta::avdtp
