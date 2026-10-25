#include "png_writer.h"

#include <cstdio>

#include <zlib.h>

namespace {

void PutBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void PutChunk(std::vector<uint8_t>& out, const char type[4],
              const std::vector<uint8_t>& data) {
    PutBe32(out, static_cast<uint32_t>(data.size()));
    const size_t type_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uLong crc = crc32(0L, out.data() + type_start,
                            static_cast<uInt>(out.size() - type_start));
    PutBe32(out, static_cast<uint32_t>(crc));
}

}  // namespace

bool WriteGrayPng(const std::string& path, int width, int height,
                  const std::vector<uint8_t>& gray) {
    if (width <= 0 || height <= 0 ||
        gray.size() != static_cast<size_t>(width) * height) {
        return false;
    }

    // Each scanline is prefixed with filter type 0 (none).
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(width + 1) * height);
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = gray.data() + static_cast<size_t>(y) * width;
        raw.insert(raw.end(), row, row + width);
    }

    uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, raw.data(),
                  static_cast<uLong>(raw.size()), 9) != Z_OK) {
        return false;
    }
    compressed.resize(compressed_size);

    std::vector<uint8_t> header;
    PutBe32(header, static_cast<uint32_t>(width));
    PutBe32(header, static_cast<uint32_t>(height));
    header.push_back(8);  // bit depth
    header.push_back(0);  // colour type: grayscale
    header.push_back(0);  // compression
    header.push_back(0);  // filter
    header.push_back(0);  // interlace

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    PutChunk(png, "IHDR", header);
    PutChunk(png, "IDAT", compressed);
    PutChunk(png, "IEND", {});

    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    return std::fclose(file) == 0 && ok;
}
