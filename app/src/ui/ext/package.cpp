// Wisp: extension packages (ui/ext/package.h).
//
// A .crx starts with "Cr24": CRX2 = version 2, the public key and signature lengths, the key, the
// signature, then the zip; CRX3 = version 3, a header length, a CrxFileHeader (protobuf), then the
// zip. The extension id is the first 16 bytes of the SHA-256 of the public key, each nibble written as
// 'a'..'p' (CRX3 carries it ready in signed_header_data.crx_id). Opera's .nex is a CRX; Firefox's .xpi
// and a plain .zip are the zip alone. The zip is read through its central directory (stored and
// deflated entries); names that would leave the folder are refused.
#include "package.h"

#include "../../common/wisp.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <zlib.h>

namespace wisp::ext {
namespace {

bool read_file(const std::string &path, std::vector<unsigned char> &data)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (256L << 20)) {
        fclose(f);
        return false;
    }
    data.resize(static_cast<size_t>(size));
    const bool ok = fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

uint16_t le16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

std::string id_from_hash(const unsigned char *hash16)
{
    std::string id;
    for (int i = 0; i < 16; i++) {
        id += static_cast<char>('a' + (hash16[i] >> 4));
        id += static_cast<char>('a' + (hash16[i] & 15));
    }
    return id;
}

// protobuf: the value of field `field` (length-delimited) in [p, end), or nullptr
const unsigned char *proto_field(const unsigned char *p, const unsigned char *end, uint64_t field, size_t &len)
{
    while (p < end) {
        uint64_t key = 0;
        int shift = 0;
        while (p < end) {
            const unsigned char b = *p++;
            key |= static_cast<uint64_t>(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        const int wire = static_cast<int>(key & 7);
        const uint64_t number = key >> 3;
        if (wire == 0) {
            while (p < end && (*p++ & 0x80)) { }
        } else if (wire == 2) {
            uint64_t length = 0;
            shift = 0;
            while (p < end) {
                const unsigned char b = *p++;
                length |= static_cast<uint64_t>(b & 0x7f) << shift;
                shift += 7;
                if (!(b & 0x80))
                    break;
            }
            if (length > static_cast<uint64_t>(end - p))
                return nullptr;
            if (number == field) {
                len = static_cast<size_t>(length);
                return p;
            }
            p += length;
        } else if (wire == 5) {
            p += 4;
        } else if (wire == 1) {
            p += 8;
        } else {
            return nullptr;
        }
    }
    return nullptr;
}

bool make_dirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            const std::string part = path.substr(0, i);
            if (mkdir(part.c_str(), 0777) && errno != EEXIST)
                return false;
        }
    }
    return true;
}

bool safe_name(const std::string &name)
{
    if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t slash = name.find('/', start);
        const std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..")
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

} // namespace

std::string extension_id_from_bytes(const std::vector<unsigned char> &bytes)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(bytes.data(), bytes.size(), hash);
    return id_from_hash(hash);
}

Unpacked unpack_package(const std::string &path, const std::string &folder)
{
    Unpacked result;
    std::vector<unsigned char> data;
    if (!read_file(path, data)) {
        result.error = "the package could not be read";
        return result;
    }
    size_t zip = 0;
    if (data.size() >= 16 && !memcmp(data.data(), "Cr24", 4)) {
        const uint32_t version = le32(&data[4]);
        if (version == 2) {
            const uint32_t key = le32(&data[8]), sig = le32(&data[12]);
            zip = 16 + static_cast<size_t>(key) + sig;
            if (zip > data.size()) {
                result.error = "a broken CRX2 header";
                return result;
            }
            result.crx_id = extension_id_from_bytes(std::vector<unsigned char>(data.begin() + 16, data.begin() + 16 + key));
        } else if (version == 3) {
            const uint32_t header = le32(&data[8]);
            zip = 12 + static_cast<size_t>(header);
            if (zip > data.size()) {
                result.error = "a broken CRX3 header";
                return result;
            }
            // CrxFileHeader.signed_header_data (10000) -> SignedData.crx_id (1): 16 bytes
            size_t len = 0;
            const unsigned char *signed_data = proto_field(&data[12], &data[12] + header, 10000, len);
            size_t id_len = 0;
            const unsigned char *id = signed_data ? proto_field(signed_data, signed_data + len, 1, id_len) : nullptr;
            if (id && id_len == 16)
                result.crx_id = id_from_hash(id);
        } else {
            result.error = "an unknown CRX version (" + std::to_string(version) + ")";
            return result;
        }
    }

    // the zip's end of central directory, from the end
    const unsigned char *base = data.data() + zip;
    const size_t size = data.size() - zip;
    size_t eocd = std::string::npos;
    for (size_t i = size >= 22 ? size - 22 : 0; i + 22 <= size; i--) {
        if (le32(base + i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0 || size - i > 65557)
            break;
    }
    if (eocd == std::string::npos) {
        result.error = "not a zip archive";
        return result;
    }
    const uint16_t count = le16(base + eocd + 10);
    size_t dir = le32(base + eocd + 16);
    if (!make_dirs(folder)) {
        result.error = "the folder " + folder + " could not be made";
        return result;
    }
    for (uint16_t n = 0; n < count; n++) {
        if (dir + 46 > size || le32(base + dir) != 0x02014b50) {
            result.error = "a broken zip directory";
            return result;
        }
        const uint16_t method = le16(base + dir + 10);
        const uint32_t packed = le32(base + dir + 20), plain = le32(base + dir + 24);
        const uint16_t name_len = le16(base + dir + 28), extra_len = le16(base + dir + 30), comment_len = le16(base + dir + 32);
        const uint32_t local = le32(base + dir + 42);
        const std::string name(reinterpret_cast<const char *>(base + dir + 46), name_len);
        dir += 46 + static_cast<size_t>(name_len) + extra_len + comment_len;
        if (!safe_name(name)) {
            result.error = "a file name that leaves the folder: " + name;
            return result;
        }
        const std::string out = folder + "/" + name;
        if (name.back() == '/') {
            make_dirs(out.substr(0, out.size() - 1));
            continue;
        }
        const size_t slash = out.find_last_of('/');
        make_dirs(out.substr(0, slash));
        if (local + 30 > size || le32(base + local) != 0x04034b50) {
            result.error = "a broken zip entry: " + name;
            return result;
        }
        const size_t start = local + 30 + le16(base + local + 26) + le16(base + local + 28);
        if (start + packed > size) {
            result.error = "a truncated zip entry: " + name;
            return result;
        }
        std::vector<unsigned char> content(plain);
        if (method == 0) {
            memcpy(content.data(), base + start, plain);
        } else if (method == 8) {
            z_stream z {};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
                result.error = "zlib";
                return result;
            }
            z.next_in = const_cast<unsigned char *>(base + start);
            z.avail_in = packed;
            z.next_out = content.data();
            z.avail_out = plain;
            const int ret = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (ret != Z_STREAM_END) {
                result.error = "a damaged zip entry: " + name;
                return result;
            }
        } else {
            result.error = "an unsupported compression in " + name;
            return result;
        }
        FILE *f = fopen(out.c_str(), "wb");
        if (!f || (plain && fwrite(content.data(), 1, plain, f) != plain)) {
            if (f)
                fclose(f);
            result.error = "could not write " + out;
            return result;
        }
        fclose(f);
        result.files++;
        result.bytes += plain;
    }
    result.ok = true;
    return result;
}

} // namespace wisp::ext
