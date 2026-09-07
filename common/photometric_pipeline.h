#pragma once

#include "photometric_types.h"
#include <dshow.h>
#include <chrono>
#include <memory>
#include <mutex>

namespace facelogin {

// Abstracts IAMVideoProcAmp/IAMCameraControl from the image pipeline.  The
// implementation owns AddRef'd interfaces, so camera teardown cannot leave a
// processing thread with borrowed COM pointers.
class CameraControlAdapter {
public:
    virtual ~CameraControlAdapter() = default;
    virtual bool Probe() = 0;
    virtual bool PrepareManualControl() = 0;
    virtual bool StepExposure(int direction) = 0;
    virtual bool StepGain(int direction) = 0;
    virtual bool VerifyResponse(int direction, float before, float after) = 0;
    virtual bool RestoreOriginalState() = 0;
    virtual HardwareControlState State() const = 0;
};

// Works for both Media Foundation and DirectShow captures because both expose
// the same Windows camera-control COM interfaces.  The capture backends only
// provide their interfaces; no backend-specific details leak upward.
std::unique_ptr<CameraControlAdapter> CreateCameraControlAdapter(
    IAMVideoProcAmp* videoProcAmp,
    IAMCameraControl* cameraControl);

FacePhotometricStats MeasureFacePhotometricStats(
    const dlib::matrix<dlib::rgb_pixel>& frame,
    const dlib::rectangle& faceRect,
    const dlib::full_object_detection* landmarks = nullptr);

// Correct only slowly varying illumination inside an aligned recognition
// chip. This intentionally does not operate on the full anti-spoof frame and
// leaves chroma/high-frequency facial detail intact. It is a no-op for a
// reasonably uniform chip, preserving the old embedding domain in normal
// light.
void ApplyLocalIlluminationCorrection(
    dlib::matrix<dlib::rgb_pixel>& faceChip,
    bool enabled = true);

class PhotometricSession {
public:
    PhotometricSession() = default;
    ~PhotometricSession();

    PhotometricSession(const PhotometricSession&) = delete;
    PhotometricSession& operator=(const PhotometricSession&) = delete;

    void Configure(const PhotometricConfig& config);
    void Attach(IAMVideoProcAmp* videoProcAmp, IAMCameraControl* cameraControl);
    // Test hook for deterministic state-machine tests. Production callers use
    // Attach(), which creates the COM adapter for the active camera backend.
    void SetAdapterForTesting(std::unique_ptr<CameraControlAdapter> adapter);
    bool Begin();
    void End();

    bool Enabled() const;
    bool HardwareActive() const;
    HardwareControlState State() const;
    FramePhotometricTransform LastTransform() const;

    // Process one raw frame.  Hardware control is deliberately slow and
    // feedback-verified; software normalization is evaluated on every frame.
    FacePhotometricStats ProcessFrame(
        dlib::matrix<dlib::rgb_pixel>& frame,
        const dlib::rectangle& faceRect,
        const dlib::full_object_detection* landmarks = nullptr);

    // Used only to make a dark frame detectable.  It never writes camera
    // controls and uses whole-frame statistics because no face is known yet.
    FacePhotometricStats NormalizeForDetection(
        dlib::matrix<dlib::rgb_pixel>& frame);

    static void ApplyTransform(dlib::matrix<dlib::rgb_pixel>& frame,
                               const FramePhotometricTransform& transform);

private:
    FacePhotometricStats ProcessStats(dlib::matrix<dlib::rgb_pixel>& frame,
                                      const FacePhotometricStats& stats,
                                      bool allowHardware);
    void UpdateHardware(const FacePhotometricStats& stats);
    FramePhotometricTransform BuildTransform(const FacePhotometricStats& stats);
    void Demote(HardwareControlState state, const wchar_t* reason);
    static float Clamp(float value, float low, float high);

    PhotometricConfig m_config;
    std::unique_ptr<CameraControlAdapter> m_adapter;
    HardwareControlState m_state = HardwareControlState::Disabled;
    bool m_started = false;

    std::chrono::steady_clock::time_point m_lastHardwareStep{};
    bool m_pendingResponse = false;
    int m_pendingDirection = 0;
    float m_pendingLuma = 0.0f;
    int m_noResponseCount = 0;
    int m_reversedCount = 0;

    bool m_hasSmoothedGain = false;
    float m_smoothedGain = 1.0f;
    FramePhotometricTransform m_lastTransform;
    mutable std::recursive_mutex m_mutex;
};

// Small adapter used by callers that already own the face box/landmarks.  It
// produces the same result object for enrollment and authentication without
// coupling this common module to ONNX model classes.
class UnifiedFacePipeline {
public:
    explicit UnifiedFacePipeline(PhotometricSession& session) : m_session(session) {}

    bool ProcessFrame(const dlib::matrix<dlib::rgb_pixel>& raw,
                      const dlib::rectangle& faceRect,
                      const dlib::full_object_detection& landmarks,
                      UnifiedFaceFrame& output);

    bool NormalizeForDetection(dlib::matrix<dlib::rgb_pixel>& frame) {
        return m_session.NormalizeForDetection(frame).valid;
    }

private:
    PhotometricSession& m_session;
};

} // namespace facelogin
