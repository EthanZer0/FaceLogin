#include "exposure_control.h"
#include "logger.h"
#include <cmath>

namespace facelogin {

namespace {
// Max camera step per iteration: ±1/3 EV (log2 factor ±0.338). Larger steps
// overshoot on noisy single-frame measurements and make the loop oscillate
// around the band.
constexpr double kMaxCameraStepLog2 = 0.338;         // log2(1.26)
// Digital gain bounds. Beyond these a plain multiplier cannot help — the
// frame is either clipped (highlights already destroyed) or so dark that
// amplification drowns the face in sensor noise.
constexpr float kMinDigitalGain = 0.5f;
constexpr float kMaxDigitalGain = 2.0f;
// Pixels brighter than this count as clipped for the diagnostics share.
constexpr float kClipLuma = 240.0f;
// Minimum interval between camera-control steps. The driver needs a beat to
// apply an exposure/gain write; stepping every frame (30fps preview) makes a
// laggy camera oscillate dark↔bright as the loop overtakes its own updates.
constexpr auto kCameraStepIntervalMs = std::chrono::milliseconds(400);
// No-response detection: N consecutive camera steps that move the measured
// luma by less than this count as "device ignores manual mode" → demote to
// digital-only. Laptop cameras commonly accept Set() but keep AE in charge.
constexpr int kNoResponseStepCount = 3;
constexpr float kNoResponseLumaDelta = 8.0f;
}

float FaceExposureController::Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void FaceExposureController::Attach(IAMVideoProcAmp* videoProcAmp,
                                    IAMCameraControl* cameraControl) {
    Reset();
    m_vpa = videoProcAmp;
    m_cc = cameraControl;
    m_capabilityLogged = false;   // re-probe per camera open
}

void FaceExposureController::Configure(bool enabled, float targetLuma,
                                       float toleranceBand) {
    m_enabled = enabled;
    m_target = Clamp(targetLuma, 40.0f, 200.0f);
    m_band = Clamp(toleranceBand, 5.0f, 60.0f);
    if (!m_enabled) m_sessionGain = 1.0f;
}

float FaceExposureController::MeasureLuma(
    const dlib::matrix<dlib::rgb_pixel>& frame,
    const dlib::rectangle& region) {
    if (frame.size() == 0) return 0.0f;
    dlib::rectangle r = region.intersect(
        dlib::rectangle(0, 0, frame.nc() - 1, frame.nr() - 1));
    if (r.is_empty()) return 0.0f;

    double sum = 0.0;
    long count = 0;
    for (long y = r.top(); y <= r.bottom(); y += 4) {
        for (long x = r.left(); x <= r.right(); x += 4) {
            const auto& p = frame(y, x);
            sum += 0.299 * p.red + 0.587 * p.green + 0.114 * p.blue;
            count++;
        }
    }
    return count ? static_cast<float>(sum / count) : 0.0f;
}

void FaceExposureController::ProbeCapabilities() {
    m_capabilityLogged = true;
    m_channel = Channel::Digital;

    long minV = 0, maxV = 0, stepV = 0, defV = 0;
    long caps = 0;
    if (m_cc && SUCCEEDED(m_cc->GetRange(CameraControl_Exposure,
                                         &minV, &maxV, &stepV, &defV, &caps))) {
        m_channel = Channel::Exposure;
        FACELOGIN_INFO(L"Exposure control: manual exposure supported "
                       L"(range %ld..%ld, step %ld) — channel=exposure",
                       minV, maxV, stepV);
        return;
    }
    if (m_vpa && SUCCEEDED(m_vpa->GetRange(VideoProcAmp_Gain,
                                           &minV, &maxV, &stepV, &defV, &caps))) {
        m_channel = Channel::Gain;
        FACELOGIN_INFO(L"Exposure control: manual gain supported "
                       L"(range %ld..%ld, step %ld) — channel=gain",
                       minV, maxV, stepV);
        return;
    }
    FACELOGIN_INFO(L"Exposure control: no manual camera control — digital gain only");
}

bool FaceExposureController::SteerFrame(
    dlib::matrix<dlib::rgb_pixel>& frame,
    const dlib::rectangle& faceRect, int iteration) {
    if (!m_enabled) return true;
    if (!m_capabilityLogged) ProbeCapabilities();

    // Measure raw camera luma + clipped share over the face region.
    if (frame.size() == 0) return false;
    dlib::rectangle r = faceRect.intersect(
        dlib::rectangle(0, 0, frame.nc() - 1, frame.nr() - 1));
    if (r.is_empty()) return false;

    double sum = 0.0;
    long count = 0;
    long clipped = 0;
    for (long y = r.top(); y <= r.bottom(); y += 4) {
        for (long x = r.left(); x <= r.right(); x += 4) {
            const auto& p = frame(y, x);
            const float l = 0.299f * p.red + 0.587f * p.green + 0.114f * p.blue;
            sum += l;
            count++;
            if (l > kClipLuma) clipped++;
        }
    }
    if (count == 0) return false;
    const float luma = static_cast<float>(sum / count);
    const float clipPct = 100.0f * static_cast<float>(clipped) / count;

    const bool inBand = std::fabs(luma - m_target) <= m_band;
    const bool cameraChanged = !inBand && ApplyCameraCorrection(luma);

    // Normalize THIS frame right away — the effective face luma lands on the
    // target even before the camera step (if any) lands on future frames.
    m_sessionGain = Clamp(m_target / luma, kMinDigitalGain, kMaxDigitalGain);
    ApplyGainInPlace(frame, m_sessionGain);

    // Log only meaningful transitions: a camera control action, or the face
    // entering/leaving the band. A converged controller would otherwise spam
    // the log at preview frame rate.
    const bool bandChanged = (inBand != m_lastInBand);
    m_lastInBand = inBand;
    if (cameraChanged || bandChanged) {
        FACELOGIN_INFO(L"Exposure steer: iter=%d faceLuma=%.0f clip=%.1f%% "
                       L"band=%.0f±%.0f channel=%hs digital=%.2fx%ls%ls",
                       iteration, luma, clipPct, m_target, m_band,
                       m_channel == Channel::Exposure ? "exposure" :
                       m_channel == Channel::Gain ? "gain" : "digital",
                       m_sessionGain,
                       cameraChanged ? L" camera=changed" : L"",
                       inBand ? L" (in-band)" : L"");
    }
    return inBand;
}

bool FaceExposureController::ApplyCameraCorrection(float faceLuma) {
    // Pacing: at most one camera step per interval, regardless of how many
    // frames arrive. Stepping every frame overtakes a laggy driver's response
    // and the loop oscillates around the band (visible as exposure pumping in
    // the 30fps preview). The digital gain corrects every frame in between,
    // so the effective domain stays on target even mid-step.
    const bool stepDue = m_lastCameraStepAt.time_since_epoch().count() == 0 ||
        (std::chrono::steady_clock::now() - m_lastCameraStepAt) >= kCameraStepIntervalMs;
    if (!stepDue) return false;

    if (m_channel == Channel::Exposure) {
        if (!SetExposureManual(faceLuma)) {
            FACELOGIN_WARN(L"Exposure control: manual exposure rejected — falling back to gain");
            m_channel = Channel::Gain;
        } else {
            m_lastCameraStepAt = std::chrono::steady_clock::now();
            // No-response detection: some cameras accept Set() but keep AE in
            // charge. If the luma never moves across consecutive steps, stop
            // hammering the dead control — demote to digital-only.
            if (m_lastStepLuma >= 0.0f &&
                std::fabs(faceLuma - m_lastStepLuma) < kNoResponseLumaDelta) {
                if (++m_unresponsiveSteps >= kNoResponseStepCount) {
                    FACELOGIN_WARN(L"Exposure control: camera ignores manual exposure "
                                   L"(luma unchanged over %d steps) — digital gain only",
                                   m_unresponsiveSteps);
                    m_channel = Channel::Digital;
                    return false;
                }
            } else {
                m_unresponsiveSteps = 0;
            }
            m_lastStepLuma = faceLuma;
            return true;
        }
    }
    if (m_channel == Channel::Gain) {
        if (!SetGainManual(faceLuma)) {
            FACELOGIN_WARN(L"Exposure control: manual gain rejected — digital gain only");
            m_channel = Channel::Digital;
            return false;
        }
        m_lastCameraStepAt = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

bool FaceExposureController::SetExposureManual(float faceLuma) {
    if (!m_cc) return false;
    if (!m_restoreExposureValid) {
        if (FAILED(m_cc->Get(CameraControl_Exposure,
                             &m_restoreExposureValue, &m_restoreExposureFlags))) {
            return false;
        }
        m_restoreExposureValid = true;
    }
    long minV = 0, maxV = 0, stepV = 0, defV = 0;
    long caps = 0;
    if (FAILED(m_cc->GetRange(CameraControl_Exposure,
                              &minV, &maxV, &stepV, &defV, &caps))) {
        return false;
    }
    long cur = 0;
    long curFlags = 0;
    if (FAILED(m_cc->Get(CameraControl_Exposure, &cur, &curFlags))) return false;

    // Exposure units vary by driver. The legacy convention is
    // value = −10000·log2(seconds) (LARGE spans — a larger value = shorter
    // exposure = darker). Many cameras (this one: −8..0 step 1) instead
    // report plain stops — value = log2(seconds) — where a larger value =
    // longer exposure = BRIGHTER. Normalize by the range span:
    //   small span (< 1000) → units ARE stops, larger = brighter
    //   large span          → −10000 per stop, larger = darker
    // and always move at least one driver step so step=1 cameras actually
    // move (a ±1/3 EV correction would otherwise round to zero).
    const long span = maxV - minV;
    if (span <= 0) return false;
    const bool largerIsBrighter = (span < 1000);
    const double unitsPerStop = largerIsBrighter ? 1.0 : (span / 8.0);

    // Desired change in stops (negative = darken), clamped to ±1/3 EV/iter.
    const double dStops = Clamp(static_cast<float>(std::log2(m_target / faceLuma)),
                                -static_cast<float>(kMaxCameraStepLog2),
                                static_cast<float>(kMaxCameraStepLog2));
    long delta = static_cast<long>(std::lround(dStops * unitsPerStop));
    if (delta == 0 && dStops != 0.0) delta = (dStops > 0 ? 1 : -1);
    long newVal = largerIsBrighter ? cur + delta : cur - delta;
    if (stepV > 0) {
        // Snap onto the driver's step grid so the value is accepted verbatim.
        newVal = minV + static_cast<long>(
            std::round(static_cast<double>(newVal - minV) / stepV)) * stepV;
    }
    newVal = newVal < minV ? minV : (newVal > maxV ? maxV : newVal);
    if (newVal == cur) return true;   // already at the edge — nothing to set
    if (FAILED(m_cc->Set(CameraControl_Exposure, newVal,
                         CameraControl_Flags_Manual))) {
        return false;
    }
    m_cameraChanged = true;
    return true;
}

bool FaceExposureController::SetGainManual(float faceLuma) {
    if (!m_vpa) return false;
    if (!m_restoreGainValid) {
        if (FAILED(m_vpa->Get(VideoProcAmp_Gain,
                              &m_restoreGainValue, &m_restoreGainFlags))) {
            return false;
        }
        m_restoreGainValid = true;
    }
    long minV = 0, maxV = 0, stepV = 0, defV = 0;
    long caps = 0;
    if (FAILED(m_vpa->GetRange(VideoProcAmp_Gain,
                               &minV, &maxV, &stepV, &defV, &caps))) {
        return false;
    }
    long cur = 0;
    long curFlags = 0;
    if (FAILED(m_vpa->Get(VideoProcAmp_Gain, &cur, &curFlags))) return false;

    // Gain is linear: multiply by the (clamped ±1/3 EV) brighten factor.
    const float f = Clamp(m_target / faceLuma,
                          1.0f / static_cast<float>(std::exp2(kMaxCameraStepLog2)),
                          static_cast<float>(std::exp2(kMaxCameraStepLog2)));
    long newVal = static_cast<long>(cur * f);
    if (stepV > 0) {
        newVal = minV + static_cast<long>(
            std::round(static_cast<double>(newVal - minV) / stepV)) * stepV;
    }
    newVal = newVal < minV ? minV : (newVal > maxV ? maxV : newVal);
    if (FAILED(m_vpa->Set(VideoProcAmp_Gain, newVal, VideoProcAmp_Flags_Manual))) {
        return false;
    }
    m_cameraChanged = true;
    return true;
}

void FaceExposureController::ApplySessionGain(
    dlib::matrix<dlib::rgb_pixel>& frame) const {
    if (!m_enabled || m_sessionGain == 1.0f) return;
    ApplyGainInPlace(frame, m_sessionGain);
}

void FaceExposureController::ApplyGainInPlace(
    dlib::matrix<dlib::rgb_pixel>& frame, float gain) {
    const long n = static_cast<long>(frame.size());
    for (long i = 0; i < n; i++) {
        auto& p = frame(i);
        int r = static_cast<int>(p.red   * gain + 0.5f);
        int g = static_cast<int>(p.green * gain + 0.5f);
        int b = static_cast<int>(p.blue  * gain + 0.5f);
        p.red   = static_cast<unsigned char>(r > 255 ? 255 : r);
        p.green = static_cast<unsigned char>(g > 255 ? 255 : g);
        p.blue  = static_cast<unsigned char>(b > 255 ? 255 : b);
    }
}

void FaceExposureController::Reset() {
    if (m_cameraChanged) {
        if (m_cc && m_restoreExposureValid) {
            m_cc->Set(CameraControl_Exposure,
                      m_restoreExposureValue, m_restoreExposureFlags);
        }
        if (m_vpa && m_restoreGainValid) {
            m_vpa->Set(VideoProcAmp_Gain,
                       m_restoreGainValue, m_restoreGainFlags);
        }
        FACELOGIN_INFO(L"Exposure control: camera controls restored to original");
    }
    m_vpa = nullptr;
    m_cc = nullptr;
    m_sessionGain = 1.0f;
    m_channel = Channel::Digital;
    m_capabilityLogged = false;
    m_cameraChanged = false;
    m_lastInBand = false;
    m_lastCameraStepAt = {};
    m_lastStepLuma = -1.0f;
    m_unresponsiveSteps = 0;
    m_restoreExposureValid = false;
    m_restoreGainValid = false;
}

} // namespace facelogin