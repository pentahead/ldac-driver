#include "common/status.h"

namespace bta {

const char* ErrName(Err e)
{
    switch (e) {
    case Err::Ok: return "Ok";
    case Err::InvalidArgument: return "InvalidArgument";
    case Err::NotSupported: return "NotSupported";
    case Err::NotAvailable: return "NotAvailable";
    case Err::Timeout: return "Timeout";
    case Err::Rejected: return "Rejected";
    case Err::ProtocolError: return "ProtocolError";
    case Err::Disconnected: return "Disconnected";
    case Err::BufferOverflow: return "BufferOverflow";
    case Err::EncoderError: return "EncoderError";
    case Err::IoError: return "IoError";
    case Err::StateError: return "StateError";
    case Err::Internal: return "Internal";
    }
    return "Unknown";
}

std::string Status::ToString() const
{
    if (ok()) return "Ok";
    return std::string(ErrName(code_)) + ": " + msg_;
}

}  // namespace bta
