//===--- Payload.h - A program appended to the executable that runs it ----===//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace acvm {

/// A single-file bundle is:
///
///     [ executable ][ payload ][ footer ]
///
/// Appending is safe because the Windows loader reads the PE headers and section table and
/// ignores whatever follows the last section; nothing is patched, and the executable runs
/// normally whether or not a payload is present.
///
/// The footer sits at the very end and is a fixed size, so finding it is a seek to
/// `fileSize - sizeof(PayloadFooter)` and a magic check -- no scan, and no length prefix to
/// keep in sync at the front.
inline constexpr char kPayloadMagic[8] = {'A', 'P', 'A', 'C', 'K', '0', '0', '1'};
inline constexpr std::size_t kPayloadFooterSize = 24;

struct PayloadFooter {
    char magic[8];
    std::uint64_t offset;
    std::uint64_t size;
};
static_assert(sizeof(PayloadFooter) == kPayloadFooterSize,
              "the footer is located by its size; changing it breaks every bundle");

/// Appends `payloadPath` to the copy of the runtime at `executablePath`.
///
/// Rewrites the whole file rather than patching it, because the footer has to be the last
/// thing in the file and there is no room reserved for it.
bool appendPayload(const std::string& executablePath, const std::string& payloadPath,
                   std::string& error);

/// Reads the payload out of `executablePath`.
///
/// Returns false when there is none -- the ordinary case for an unbundled runtime -- and
/// also when there is one but it is not where the footer says. `error` is set only for the
/// second kind: a missing payload is not a failure.
bool readEmbeddedPayload(const std::string& executablePath, std::string& payload,
                         std::string& error);

} // namespace acvm
