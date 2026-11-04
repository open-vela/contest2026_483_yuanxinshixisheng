#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace voicelink {
// The accepted offline voice is 8 kHz; the board codec runs at 16 kHz.
// Linear interpolation, with a held final sample, preserves signed full scale.
inline std::vector<std::uint32_t> speechPcm(const std::int16_t* input,
                                           std::size_t frames, unsigned rate) {
  if (!input || !frames || (rate != 8000 && rate != 16000) ||
      frames > std::size_t(rate) * 30)
    throw std::invalid_argument("unsupported speech PCM");
  const unsigned factor = rate == 8000 ? 2 : 1;
  std::vector<std::uint32_t> output(frames * factor * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    for (unsigned part = 0; part < factor; ++part) {
      const std::int32_t sample = part ?
          (std::int32_t(input[i]) + input[i + 1 < frames ? i + 1 : i]) / 2 : input[i];
      const auto word = std::uint32_t(sample * 65536);
      const auto index = (i * factor + part) * 2;
      output[index] = output[index + 1] = word;
    }
  }
  return output;
}
}
