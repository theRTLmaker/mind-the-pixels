#ifndef SIMULATOR_PNG_WRITER_H_
#define SIMULATOR_PNG_WRITER_H_

#include <cstdint>
#include <string>
#include <vector>

// Writes an 8-bit grayscale PNG. gray holds width * height values, 0=black.
bool WriteGrayPng(const std::string& path, int width, int height,
                  const std::vector<uint8_t>& gray);

#endif  // SIMULATOR_PNG_WRITER_H_
