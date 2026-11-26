// Wisp: extension packages - Chrome .crx (CRX2 and CRX3), Opera .nex (a CRX), Firefox .xpi and plain
// .zip - unpacked into a folder (ui/ext/package.cpp).
#pragma once

#include <string>
#include <vector>

namespace wisp::ext {

struct Unpacked {
    bool ok = false;
    std::string error;
    std::string crx_id;           // a CRX's extension id (from its public key), when it has one
    size_t files = 0, bytes = 0;
};

// Unpacks the package at path into folder (made if needed; files only inside it)
Unpacked unpack_package(const std::string &path, const std::string &folder);

// A Chrome-style extension id ("a".."p", 32 letters) from a public key or any stable string
std::string extension_id_from_bytes(const std::vector<unsigned char> &bytes);

} // namespace wisp::ext
