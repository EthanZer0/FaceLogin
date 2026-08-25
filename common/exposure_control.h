#pragma once

// Face-region exposure control (1.9.0).
//
// Feedback loop that keeps the FACE's brightness inside a fixed target band:
//  1. measures the face region's mean luma (BT.601, subsampled) — whole-frame
//     luma is useless here, a dark background with a blown-out face reads as
//     "fine" on the frame average;
//  2. steers the camera's own controls when the device supports them
//     (manual exposure preferred, manual gain as fallback) — physical
//     exposure reduction is the only way to actually prevent highlight
//     clipping, which no post-gain can undo;
//  3. tops up the residual with a frame-level digital gain so the effective
//     face luma lands on the target immediately, even before the camera step
//     takes effect on the next frames.
//
// Enrollment (console) and unlock (service) both converge to the same target,
// so anchors and probes live in the same brightness domain regardless of the
// room — an over/underexposed face no longer shifts the embedding (this is
// the photometric half of the cross-environment recognition failures).
//
// Clipped pixels (> ~240) are unrecoverable information loss; the controller
// reports their share per iteration so diagnostics can distinguish "face
// overexposed" from "dim but recoverable".
//
// Threading: NOT thread-safe. The service drives it from the auth-loop
// thread, the console from the frame thread. Attach() before the camera/thread
// starts collecting, Reset() when the camera is released.

#include <windows.h>
#include <dshow.h>          // IAMVideoProcAmp / IAMCameraControl
#include <chrono>
#include <dlib/matrix.h>
#include <dlib/pixel.h>

namespace facelogin {

class FaceExposureController {
public:
    FaceExposureController() = default;
    ~FaceExposureController() { Reset(); }

    FaceExposureController(const FaceExposureController&) = delete;
    FaceExposureController& operator=(const FaceExposureController&) = delete;

    // Attach the camera control interfaces. BORROWED — owned by the capture
    // class; Reset() must run before the capture class releases them. Either
    // pointer may be null (device without manual controls → digital-only).
    void Attach(IAMVideoProcAmp* videoProcAmp, IAMCameraControl* cameraControl);

    void Configure(bool enabled, float targetLuma, float toleranceBand);
    bool Enabled() const { return m_enabled; }

    // One steering iteration on a frame whose face bbox is already known.
    // Measures the RAW camera luma, nudges the camera (if supported), then
    // applies the session digital gain in place so THIS frame's effective
    // face luma is at the target immediately. Returns true when the camera
    // luma is already inside the band (digital gain ≈ 1.0). `iteration` is
    // only used in the diagnostics log.
    bool SteerFrame(dlib::matrix<dlib::rgb_pixel>& frame,
                    const dlib::rectangle& faceRect, int iteration);

    // Apply the current session digital gain (no-op at 1.0). Call on every
    // frame that leaves the steer path (match loop, liveness, verify,
    // enrollment samples) so all of them see the same normalized image.
    void ApplySessionGain(dlib::matrix<dlib::rgb_pixel>& frame) const;

    // Mean luma of a region (BT.601 weights, 4px subsample, clamped to the
    // frame). 0 when the region is empty or invalid.
    static float MeasureLuma(const dlib::matrix<dlib::rgb_pixel>& frame,
                             const dlib::rectangle& region);

    // Restore the camera's original control flags/values (auto modes) and
    // clear the session state. Call when the camera is released.
    void Reset();

private:
    enum class Channel { Digital, Gain, Exposure };

    void ProbeCapabilities();
    bool ApplyCameraCorrection(float faceLuma);   // false once fully digital
    bool SetExposureManual(float faceLuma);       // false → demote channel
    bool SetGainManual(float faceLuma);           // false → demote channel
    static void ApplyGainInPlace(dlib::matrix<dlib::rgb_pixel>& frame, float gain);
    static float Clamp(float v, float lo, float hi);

    IAMVideoProcAmp* m_vpa = nullptr;    // borrowed
    IAMCameraControl* m_cc = nullptr;    // borrowed

    bool m_enabled = false;
    float m_target = 110.0f;
    float m_band = 15.0f;
    float m_sessionGain = 1.0f;

    Channel m_channel = Channel::Digital;
    bool m_capabilityLogged = false;
    bool m_cameraChanged = false;        // set once a camera control was touched
    bool m_lastInBand = false;           // previous SteerFrame band state (log gating)

    // Camera-step pacing (control-loop stability): a camera needs time to
    // respond to an exposure write; stepping every frame at 30fps makes a
    // laggy driver oscillate (bright→dark→bright in the preview). At most one
    // step per kCameraStepIntervalMs; the digital gain corrects every frame
    // in between.
    std::chrono::steady_clock::time_point m_lastCameraStepAt{};
    // No-response detection: some cameras accept Set() but keep AE in charge.
    // If the measured luma does not move within a few consecutive steps,
    // demote to digital-only (once per camera open) so the loop stops
    // hammering a control that does nothing.
    float m_lastStepLuma = -1.0f;
    int m_unresponsiveSteps = 0;

    // Original camera values, restored on Reset().
    long m_restoreExposureValue = 0;
    long m_restoreExposureFlags = 0;
    bool m_restoreExposureValid = false;
    long m_restoreGainValue = 0;
    long m_restoreGainFlags = 0;
    bool m_restoreGainValid = false;
};

} // namespace facelogin