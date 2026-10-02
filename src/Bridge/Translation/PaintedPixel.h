#pragma once

#include <algorithm>
#include <cstdint>

namespace FluentShell::Bridge::Translation {

// The three BGR channels rendered over black and white can have different
// coverage (ClearType). A scalar alpha must accommodate the most covered
// channel so the retained black-pass color is valid premultiplied BGRA.
inline uint8_t RecoverPaintAlpha(const uint8_t* black, const uint8_t* white) noexcept {
    int showThrough = 255;
    for (unsigned channel = 0; channel < 3; ++channel)
        showThrough = std::min(showThrough,
            static_cast<int>(white[channel]) - static_cast<int>(black[channel]));
    return static_cast<uint8_t>(std::clamp(255 - showThrough, 0, 255));
}

} // namespace FluentShell::Bridge::Translation
