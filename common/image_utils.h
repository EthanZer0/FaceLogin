#pragma once
#include <dlib/matrix.h>
#include <dlib/pixel.h>

namespace facelogin {

/// Rotate a dlib RGB image clockwise by 0, 90, 180, or 270 degrees.
/// Other values are silently treated as 0 (no-op).
/// Frame dimensions swap accordingly for 90/270.
inline void RotateFrame(dlib::matrix<dlib::rgb_pixel>& frame, int rotation) {
    if (rotation == 0) return;

    long h = frame.nr();
    long w = frame.nc();

    if (rotation == 180) {
        dlib::matrix<dlib::rgb_pixel> out(h, w);
        for (long y = 0; y < h; ++y)
            for (long x = 0; x < w; ++x)
                out(y, x) = frame(h - 1 - y, w - 1 - x);
        frame = std::move(out);
    } else if (rotation == 90) {
        dlib::matrix<dlib::rgb_pixel> out(w, h);
        for (long y = 0; y < w; ++y)
            for (long x = 0; x < h; ++x)
                out(y, x) = frame(h - 1 - x, y);
        frame = std::move(out);
    } else if (rotation == 270) {
        dlib::matrix<dlib::rgb_pixel> out(w, h);
        for (long y = 0; y < w; ++y)
            for (long x = 0; x < h; ++x)
                out(y, x) = frame(x, w - 1 - y);
        frame = std::move(out);
    }
}

} // namespace facelogin
