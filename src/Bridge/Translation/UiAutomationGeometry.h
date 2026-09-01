#pragma once

#include <Windows.h>

#include <algorithm>

namespace FluentShell::Bridge::Translation {

inline bool ContentViewportFitsCanonicalSize(
    const RECT& visibleViewport,
    const RECT& canonicalClientBounds,
    LONG tolerance = 18) noexcept {
    const LONG visibleWidth = visibleViewport.right - visibleViewport.left;
    const LONG visibleHeight = visibleViewport.bottom - visibleViewport.top;
    const LONG canonicalWidth = canonicalClientBounds.right - canonicalClientBounds.left;
    const LONG canonicalHeight = canonicalClientBounds.bottom - canonicalClientBounds.top;
    return visibleWidth > 0 && visibleHeight > 0 &&
        canonicalWidth > 0 && canonicalHeight > 0 &&
        visibleWidth <= canonicalWidth + tolerance &&
        visibleHeight <= canonicalHeight + tolerance;
}

// XAML reports the visible intersection of an element and every clipping
// ancestor. Native node rectangles remain root-client-relative, including
// coordinates outside that client viewport.
inline bool ComputeVisibleUiaBounds(
    const RECT& rootRelativeBounds,
    POINT contentScreenOrigin,
    const RECT& rootViewport,
    const RECT* parentVisibleBounds,
    RECT& visibleBounds) noexcept {
    visibleBounds = {
        contentScreenOrigin.x + rootRelativeBounds.left,
        contentScreenOrigin.y + rootRelativeBounds.top,
        contentScreenOrigin.x + rootRelativeBounds.right,
        contentScreenOrigin.y + rootRelativeBounds.bottom,
    };
    const auto intersect = [&](const RECT& clip) noexcept {
        visibleBounds.left = std::max(visibleBounds.left, clip.left);
        visibleBounds.top = std::max(visibleBounds.top, clip.top);
        visibleBounds.right = std::min(visibleBounds.right, clip.right);
        visibleBounds.bottom = std::min(visibleBounds.bottom, clip.bottom);
        return visibleBounds.right > visibleBounds.left &&
            visibleBounds.bottom > visibleBounds.top;
    };
    return intersect(rootViewport) &&
        (!parentVisibleBounds || intersect(*parentVisibleBounds));
}

// ElementFromPoint cannot resolve a window through another top-level that
// legitimately occludes it. Structural UIA and native-isolation checks remain
// authoritative when none of the sampled points expose the proxy.
inline bool ScreenHitTestMatchesExposure(
    bool proxyExposedAtSample,
    bool proxyResolvedAtSample) noexcept {
    return proxyResolvedAtSample || !proxyExposedAtSample;
}

} // namespace FluentShell::Bridge::Translation
