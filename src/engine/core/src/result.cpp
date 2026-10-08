#include "mce/result.h"

#include <cstdio>

namespace mce {

const char* errc_name(Errc code) {
    switch (code) {
        case Errc::Ok: return "Ok";
        case Errc::Unknown: return "Unknown";
        case Errc::OutOfMemory: return "OutOfMemory";
        case Errc::AssertFailed: return "AssertFailed";
        case Errc::InvalidArgument: return "InvalidArgument";
        case Errc::NotFound: return "NotFound";
        case Errc::AlreadyExists: return "AlreadyExists";
        case Errc::Unsupported: return "Unsupported";
        case Errc::FileNotFound: return "FileNotFound";
        case Errc::AccessDenied: return "AccessDenied";
        case Errc::MmapFailed: return "MmapFailed";
        case Errc::IoError: return "IoError";
        case Errc::PlatformInit: return "PlatformInit";
        case Errc::BadMagic: return "BadMagic";
        case Errc::CrcMismatch: return "CrcMismatch";
        case Errc::VersionTooNew: return "VersionTooNew";
        case Errc::Truncated: return "Truncated";
        case Errc::MissingRef: return "MissingRef";
        case Errc::OutOfBounds: return "OutOfBounds";
        case Errc::ReservedNonZero: return "ReservedNonZero";
        case Errc::Misaligned: return "Misaligned";
        case Errc::PayloadCrcMismatch: return "PayloadCrcMismatch";
        case Errc::ChunkVersionTooNew: return "ChunkVersionTooNew";
        case Errc::DuplicateGuid: return "DuplicateGuid";
        case Errc::MissingSidecar: return "MissingSidecar";
        case Errc::DependencyCycle: return "DependencyCycle";
        case Errc::ParseError: return "ParseError";
        case Errc::ImportFailed: return "ImportFailed";
        case Errc::ShaderCompile: return "ShaderCompile";
        case Errc::PipelineCreate: return "PipelineCreate";
        case Errc::DeviceLost: return "DeviceLost";
        case Errc::AudioDeviceOpen: return "AudioDeviceOpen";
        case Errc::AudioQueueFull: return "AudioQueueFull";
        case Errc::ModuleNotFound: return "ModuleNotFound";
        case Errc::ModuleMissingEntry: return "ModuleMissingEntry";
        case Errc::EngineIdMismatch: return "EngineIdMismatch";
        case Errc::ModuleInstallFailed: return "ModuleInstallFailed";
    }
    return "Errc(?)";
}

void format_error(const Error& e, FixedString<512>& out) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s [%s]%s%s (detail %u)", errc_name(e.code), e.stage ? e.stage : "",
                  e.path.empty() ? "" : " ", e.path.c_str(), e.detail);
    out.assign(buf);
}

}  // namespace mce
