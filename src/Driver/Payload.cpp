//===--- Payload.cpp - Reading and writing a bundled payload --------------===//
#include "acvm/Driver/Payload.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace acvm {
namespace {

bool readWholeFile(const std::string& path, std::vector<char>& bytes, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "cannot read '" + path + "'";
        return false;
    }
    return true;
}

} // namespace

bool appendPayload(const std::string& executablePath, const std::string& payloadPath,
                   std::string& error) {
    std::vector<char> payload;
    if (!readWholeFile(payloadPath, payload, error)) {
        return false;
    }

    // Read the runtime in, append, write it back. Streaming would avoid holding the
    // executable in memory, but a bundle is a few megabytes and the simpler shape is the
    // one that cannot leave a half-appended file behind if a write fails midway.
    std::vector<char> executable;
    if (!readWholeFile(executablePath, executable, error)) {
        return false;
    }

    PayloadFooter footer{};
    std::memcpy(footer.magic, kPayloadMagic, sizeof(footer.magic));
    footer.offset = executable.size();
    footer.size = payload.size();

    std::ofstream out(executablePath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write '" + executablePath + "'";
        return false;
    }
    out.write(executable.data(), static_cast<std::streamsize>(executable.size()));
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
    out.flush();
    if (!out) {
        error = "cannot write '" + executablePath + "'";
        return false;
    }
    return true;
}

bool readEmbeddedPayload(const std::string& executablePath, std::string& payload,
                         std::string& error) {
    std::ifstream in(executablePath, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open '" + executablePath + "'";
        return false;
    }
    const std::streamoff total = in.tellg();
    if (total < static_cast<std::streamoff>(kPayloadFooterSize)) {
        return false;   // too small to hold a footer, so nothing was appended
    }

    in.seekg(total - static_cast<std::streamoff>(kPayloadFooterSize));
    PayloadFooter footer{};
    if (!in.read(reinterpret_cast<char*>(&footer), sizeof(footer))) {
        return false;
    }
    if (std::memcmp(footer.magic, kPayloadMagic, sizeof(footer.magic)) != 0) {
        return false;   // an ordinary executable, not a bundle
    }

    // The footer is the only thing that says where the payload is, so a footer that does
    // not describe a region inside the file means the file is damaged. Saying so is better
    // than returning "no payload", which would run the runtime's usage text and leave the
    // user wondering why their application printed a compiler banner.
    const std::uint64_t limit = static_cast<std::uint64_t>(total) - kPayloadFooterSize;
    if (footer.size == 0 || footer.offset > limit || footer.size > limit - footer.offset) {
        error = "'" + executablePath + "' has a bundle footer that does not match its contents";
        return false;
    }

    payload.resize(static_cast<std::size_t>(footer.size));
    in.seekg(static_cast<std::streamoff>(footer.offset));
    if (!in.read(payload.data(), static_cast<std::streamsize>(payload.size()))) {
        error = "cannot read the bundle payload in '" + executablePath + "'";
        return false;
    }
    return true;
}

} // namespace acvm
