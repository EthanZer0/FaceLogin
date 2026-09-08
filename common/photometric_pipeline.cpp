#include "photometric_pipeline.h"
#include "logger.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace facelogin {

namespace {

constexpr float kClipLuma = 245.0f;
constexpr float kShadowLuma = 25.0f;

float Luma(const dlib::rgb_pixel& p) {
    return 0.299f * p.red + 0.587f * p.green + 0.114f * p.blue;
}

float Quantile(std::vector<float>& values, float q) {
    if (values.empty()) return 0.0f;
    q = std::max(0.0f, std::min(1.0f, q));
    const size_t index = static_cast<size_t>(q * static_cast<float>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

bool PointInPolygon(float x, float y, const std::vector<dlib::point>& polygon) {
    bool inside = false;
    if (polygon.size() < 3) return false;
    size_t j = polygon.size() - 1;
    for (size_t i = 0; i < polygon.size(); j = i++) {
        const float xi = static_cast<float>(polygon[i].x());
        const float yi = static_cast<float>(polygon[i].y());
        const float xj = static_cast<float>(polygon[j].x());
        const float yj = static_cast<float>(polygon[j].y());
        const bool crosses = ((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / ((yj - yi) + 1e-6f) + xi);
        if (crosses) inside = !inside;
    }
    return inside;
}

std::vector<dlib::point> ErodedFacePolygon(
    const dlib::full_object_detection* landmarks,
    const dlib::rectangle& rect) {
    std::vector<dlib::point> polygon;
    if (!landmarks || landmarks->num_parts() < 33) return polygon;

    long centerX = 0;
    long centerY = 0;
    for (int i = 0; i <= 32; ++i) {
        centerX += landmarks->part(i).x();
        centerY += landmarks->part(i).y();
    }
    centerX /= 33;
    centerY /= 33;
    polygon.reserve(33);
    for (int i = 0; i <= 32; ++i) {
        const auto p = landmarks->part(i);
        const double x = centerX + (p.x() - centerX) * 0.90;
        const double y = centerY + (p.y() - centerY) * 0.90;
        polygon.emplace_back(static_cast<long>(std::lround(x)),
                             static_cast<long>(std::lround(y)));
    }
    (void)rect;
    return polygon;
}

FacePhotometricStats MeasureRegion(const dlib::matrix<dlib::rgb_pixel>& frame,
                                   long left, long top, long right, long bottom,
                                   const std::vector<dlib::point>& polygon,
                                   bool useEllipse) {
    FacePhotometricStats result;
    if (frame.size() == 0) return result;
    left = std::max<long>(0, left);
    top = std::max<long>(0, top);
    right = std::min<long>(frame.nc() - 1, right);
    bottom = std::min<long>(frame.nr() - 1, bottom);
    if (left > right || top > bottom) return result;

    std::vector<float> lumas;
    lumas.reserve(static_cast<size_t>((right - left + 1) * (bottom - top + 1) / 8 + 1));
    double sumR = 0.0, sumG = 0.0, sumB = 0.0;
    double leftSum = 0.0, rightSum = 0.0;
    long leftCount = 0, rightCount = 0;
    long clipped = 0, shadow = 0;

    const float cx = (left + right) * 0.5f;
    const float cy = (top + bottom) * 0.5f;
    const float rx = std::max(1.0f, (right - left + 1) * 0.42f);
    const float ry = std::max(1.0f, (bottom - top + 1) * 0.44f);
    for (long y = top; y <= bottom; y += 2) {
        for (long x = left; x <= right; x += 2) {
            bool selected = false;
            if (!polygon.empty()) {
                // One-pixel erosion around the bounding region avoids picking
                // up a bright wall immediately outside the contour.
                selected = PointInPolygon(static_cast<float>(x), static_cast<float>(y), polygon);
            } else if (useEllipse) {
                const float dx = (x - cx) / rx;
                const float dy = (y - cy) / ry;
                selected = dx * dx + dy * dy <= 1.0f;
            } else {
                selected = true;
            }
            if (!selected) continue;
            const auto& p = frame(y, x);
            const float lum = Luma(p);
            lumas.push_back(lum);
            sumR += p.red; sumG += p.green; sumB += p.blue;
            if (x < cx) { leftSum += lum; ++leftCount; }
            else { rightSum += lum; ++rightCount; }
            if (lum >= kClipLuma) ++clipped;
            if (lum <= kShadowLuma) ++shadow;
        }
    }
    if (lumas.size() < 8) return result;

    std::vector<float> sorted = lumas;
    std::sort(sorted.begin(), sorted.end());
    const size_t begin = sorted.size() / 10;
    const size_t end = sorted.size() - begin;
    double trimmedSum = 0.0;
    for (size_t i = begin; i < end; ++i) trimmedSum += sorted[i];
    const float mean = static_cast<float>(trimmedSum / std::max<size_t>(1, end - begin));
    const float median = Quantile(sorted, 0.50f);
    const float p10 = Quantile(sorted, 0.10f);
    const float p90 = Quantile(sorted, 0.90f);
    double variance = 0.0;
    for (float v : lumas) variance += (v - mean) * (v - mean);
    variance /= static_cast<double>(lumas.size());

    result.valid = true;
    result.meanLuma = mean;
    result.medianLuma = median;
    result.p10Luma = p10;
    result.p90Luma = p90;
    result.clippedRatio = static_cast<float>(clipped) / lumas.size();
    result.shadowRatio = static_cast<float>(shadow) / lumas.size();
    const float leftMean = leftCount ? static_cast<float>(leftSum / leftCount) : mean;
    const float rightMean = rightCount ? static_cast<float>(rightSum / rightCount) : mean;
    result.leftRightDelta = std::abs(leftMean - rightMean) / std::max(1.0f, mean);
    result.uniformity = std::max(0.0f, std::min(1.0f,
        1.0f - static_cast<float>(std::sqrt(variance) / std::max(1.0f, mean))));
    result.redMean = static_cast<float>(sumR / lumas.size());
    result.greenMean = static_cast<float>(sumG / lumas.size());
    result.blueMean = static_cast<float>(sumB / lumas.size());
    return result;
}

class ComCameraControlAdapter final : public CameraControlAdapter {
public:
    ComCameraControlAdapter(IAMVideoProcAmp* vpa, IAMCameraControl* cc)
        : m_vpa(vpa), m_cc(cc) {
        if (m_vpa) m_vpa->AddRef();
        if (m_cc) m_cc->AddRef();
    }
    ~ComCameraControlAdapter() override {
        RestoreOriginalState();
        if (m_vpa) m_vpa->Release();
        if (m_cc) m_cc->Release();
    }

    bool Probe() override {
        m_state = HardwareControlState::Probing;
        m_exposureAvailable = false;
        m_gainAvailable = false;
        if (m_cc && SUCCEEDED(m_cc->GetRange(CameraControl_Exposure, &m_exposureMin,
                                             &m_exposureMax, &m_exposureStep,
                                             &m_exposureDefault, &m_exposureCaps))) {
            m_exposureAvailable = (m_exposureStep > 0) &&
                                  ((m_exposureCaps & CameraControl_Flags_Manual) != 0);
        }
        if (m_vpa && SUCCEEDED(m_vpa->GetRange(VideoProcAmp_Gain, &m_gainMin,
                                               &m_gainMax, &m_gainStep,
                                               &m_gainDefault, &m_gainCaps))) {
            m_gainAvailable = (m_gainStep > 0) &&
                              ((m_gainCaps & VideoProcAmp_Flags_Manual) != 0);
        }
        return m_exposureAvailable || m_gainAvailable;
    }

    bool PrepareManualControl() override {
        if (m_state != HardwareControlState::Probing && !Probe()) return false;
        // The same adapter may be reused after a configuration reload. A
        // previous End() restored the old session, so this Begin() must take a
        // fresh snapshot and must not let the old "already restored" marker
        // suppress restoration of the new session's values.
        m_restored = false;
        m_exposureSaved = false;
        m_gainSaved = false;
        m_channel = Channel::None;

        // Lock every advertised automatic brightness channel before using
        // either one.  Locking Exposure while leaving Gain in Auto creates a
        // second controller in the camera driver; the driver compensates for
        // our exposure steps and the preview oscillates.  A camera that cannot
        // lock all of its advertised channels is safer in software-only mode.
        const bool exposurePrepared = !m_exposureAvailable || PrepareExposure();
        const bool gainPrepared = !m_gainAvailable || PrepareGain();
        if (exposurePrepared && gainPrepared) {
            if (m_exposureSaved) m_channel = Channel::Exposure;
            else if (m_gainSaved) m_channel = Channel::Gain;
            if (m_channel != Channel::None) {
                m_state = HardwareControlState::Active;
                return true;
            }
        }

        // Undo a partially prepared session before falling back.  Do not leave
        // one channel Manual and the other channel Auto after a failed probe.
        if (m_exposureSaved || m_gainSaved) RestoreOriginalState();
        m_channel = Channel::None;
        m_state = HardwareControlState::SoftwareOnly;
        return false;
    }

    bool StepExposure(int direction) override {
        if (m_channel != Channel::Exposure) return false;
        if (StepActive(direction)) return true;
        // A capability can be reported for exposure while the actual driver
        // refuses a manual step. Switch to a separately verified gain channel
        // before giving up the hardware path altogether.
        if (m_gainAvailable && PrepareGain()) {
            m_channel = Channel::Gain;
            return StepActive(direction);
        }
        return false;
    }

    bool StepGain(int direction) override {
        if (m_channel != Channel::Gain) return false;
        return StepActive(direction);
    }

    bool VerifyResponse(int direction, float before, float after) override {
        const float delta = after - before;
        if (std::abs(delta) < 2.0f) return false;
        return direction > 0 ? delta > 0.0f : delta < 0.0f;
    }

    bool RestoreOriginalState() override {
        if (m_restored) return true;
        bool ok = true;
        if (m_exposureSaved && m_cc) {
            ok = SUCCEEDED(m_cc->Set(CameraControl_Exposure, m_originalExposure,
                                     m_originalExposureFlags)) && ok;
            long value = 0, flags = 0;
            ok = SUCCEEDED(m_cc->Get(CameraControl_Exposure, &value, &flags)) &&
                 value == m_originalExposure && flags == m_originalExposureFlags && ok;
        }
        if (m_gainSaved && m_vpa) {
            ok = SUCCEEDED(m_vpa->Set(VideoProcAmp_Gain, m_originalGain,
                                      m_originalGainFlags)) && ok;
            long value = 0, flags = 0;
            ok = SUCCEEDED(m_vpa->Get(VideoProcAmp_Gain, &value, &flags)) &&
                 value == m_originalGain && flags == m_originalGainFlags && ok;
        }
        m_restored = true;
        m_state = ok ? HardwareControlState::Restored : HardwareControlState::SoftwareOnly;
        return ok;
    }

    HardwareControlState State() const override { return m_state; }

private:
    enum class Channel { None, Exposure, Gain };

    bool PrepareExposure() {
        if (!m_cc) return false;
        if (!m_exposureSaved && FAILED(m_cc->Get(CameraControl_Exposure,
                                                &m_originalExposure,
                                                &m_originalExposureFlags))) {
            return false;
        }
        if (FAILED(m_cc->Set(CameraControl_Exposure, m_originalExposure,
                             CameraControl_Flags_Manual))) {
            return false;
        }
        long value = 0, flags = 0;
        if (FAILED(m_cc->Get(CameraControl_Exposure, &value, &flags)) ||
            value != m_originalExposure ||
            (flags & CameraControl_Flags_Manual) == 0) {
            // Some drivers report a successful Set() but immediately keep
            // auto exposure. Restore the exact pre-session state before
            // trying the independent gain channel.
            m_cc->Set(CameraControl_Exposure, m_originalExposure,
                      m_originalExposureFlags);
            return false;
        }
        m_exposureSaved = true;
        return true;
    }

    bool PrepareGain() {
        if (!m_vpa) return false;
        if (!m_gainSaved && FAILED(m_vpa->Get(VideoProcAmp_Gain,
                                               &m_originalGain,
                                               &m_originalGainFlags))) {
            return false;
        }
        if (FAILED(m_vpa->Set(VideoProcAmp_Gain, m_originalGain,
                              VideoProcAmp_Flags_Manual))) {
            return false;
        }
        long value = 0, flags = 0;
        if (FAILED(m_vpa->Get(VideoProcAmp_Gain, &value, &flags)) ||
            value != m_originalGain ||
            (flags & VideoProcAmp_Flags_Manual) == 0) {
            m_vpa->Set(VideoProcAmp_Gain, m_originalGain, m_originalGainFlags);
            return false;
        }
        m_gainSaved = true;
        return true;
    }

    bool StepActive(int direction) {
        if (direction == 0) return false;
        if (m_channel == Channel::Exposure && m_cc) {
            long value = 0, flags = 0;
            if (FAILED(m_cc->Get(CameraControl_Exposure, &value, &flags))) return false;
            if ((flags & CameraControl_Flags_Manual) == 0) return false;
            const long next = std::max(m_exposureMin, std::min(m_exposureMax,
                value + (direction > 0 ? m_exposureStep : -m_exposureStep)));
            if (next == value || FAILED(m_cc->Set(CameraControl_Exposure, next,
                                                  CameraControl_Flags_Manual))) return false;
            long readback = 0, readbackFlags = 0;
            return SUCCEEDED(m_cc->Get(CameraControl_Exposure, &readback, &readbackFlags)) &&
                   readback == next && (readbackFlags & CameraControl_Flags_Manual) != 0;
        }
        if (m_channel == Channel::Gain && m_vpa) {
            long value = 0, flags = 0;
            if (FAILED(m_vpa->Get(VideoProcAmp_Gain, &value, &flags))) return false;
            if ((flags & VideoProcAmp_Flags_Manual) == 0) return false;
            const long next = std::max(m_gainMin, std::min(m_gainMax,
                value + (direction > 0 ? m_gainStep : -m_gainStep)));
            if (next == value || FAILED(m_vpa->Set(VideoProcAmp_Gain, next,
                                                   VideoProcAmp_Flags_Manual))) return false;
            long readback = 0, readbackFlags = 0;
            return SUCCEEDED(m_vpa->Get(VideoProcAmp_Gain, &readback, &readbackFlags)) &&
                   readback == next && (readbackFlags & VideoProcAmp_Flags_Manual) != 0;
        }
        return false;
    }

    IAMVideoProcAmp* m_vpa = nullptr;
    IAMCameraControl* m_cc = nullptr;
    HardwareControlState m_state = HardwareControlState::Disabled;
    Channel m_channel = Channel::None;
    bool m_exposureAvailable = false;
    bool m_gainAvailable = false;
    bool m_exposureSaved = false;
    bool m_gainSaved = false;
    bool m_restored = false;
    long m_exposureMin = 0, m_exposureMax = 0, m_exposureStep = 0;
    long m_exposureDefault = 0, m_exposureCaps = 0;
    long m_gainMin = 0, m_gainMax = 0, m_gainStep = 0;
    long m_gainDefault = 0, m_gainCaps = 0;
    long m_originalExposure = 0, m_originalExposureFlags = 0;
    long m_originalGain = 0, m_originalGainFlags = 0;
};

} // namespace

PhotometricMode PhotometricModeFromString(const std::string& value) {
    if (value == "software" || value == "software_only") return PhotometricMode::SoftwareOnly;
    if (value == "off") return PhotometricMode::Off;
    return PhotometricMode::Hybrid;
}

std::string PhotometricModeToString(PhotometricMode mode) {
    switch (mode) {
    case PhotometricMode::SoftwareOnly: return "software";
    case PhotometricMode::Off: return "off";
    default: return "hybrid";
    }
}

std::unique_ptr<CameraControlAdapter> CreateCameraControlAdapter(
    IAMVideoProcAmp* videoProcAmp, IAMCameraControl* cameraControl) {
    if (!videoProcAmp && !cameraControl) return nullptr;
    return std::make_unique<ComCameraControlAdapter>(videoProcAmp, cameraControl);
}

FacePhotometricStats MeasureFacePhotometricStats(
    const dlib::matrix<dlib::rgb_pixel>& frame,
    const dlib::rectangle& faceRect,
    const dlib::full_object_detection* landmarks) {
    if (frame.size() == 0) return {};
    const dlib::rectangle bounded = faceRect.is_empty()
        ? dlib::rectangle(0, 0, frame.nc() - 1, frame.nr() - 1)
        : faceRect;
    const auto polygon = ErodedFacePolygon(landmarks, bounded);
    const bool useEllipse = !bounded.is_empty();
    return MeasureRegion(frame, bounded.left(), bounded.top(), bounded.right(), bounded.bottom(),
                         polygon, useEllipse);
}

void ApplyLocalIlluminationCorrection(
    dlib::matrix<dlib::rgb_pixel>& faceChip,
    bool enabled) {
    if (!enabled || faceChip.size() == 0) return;

    // Estimate only a very low-frequency illumination field. A coarse grid
    // avoids flattening eyes, mouth and other high-frequency identity cues.
    constexpr int kGrid = 8;
    std::array<float, kGrid * kGrid> cellMean{};
    std::array<int, kGrid * kGrid> cellCount{};
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>((faceChip.nr() / 2 + 1) *
                                        (faceChip.nc() / 2 + 1)));
    double total = 0.0;
    double left = 0.0;
    double right = 0.0;
    int leftCount = 0;
    int rightCount = 0;

    for (long y = 0; y < faceChip.nr(); y += 2) {
        for (long x = 0; x < faceChip.nc(); x += 2) {
            const float luma = Luma(faceChip(y, x));
            samples.push_back(luma);
            total += luma;
            if (x < faceChip.nc() / 2) {
                left += luma;
                ++leftCount;
            } else {
                right += luma;
                ++rightCount;
            }
            const int gx = std::min(kGrid - 1,
                                    static_cast<int>(x * kGrid / faceChip.nc()));
            const int gy = std::min(kGrid - 1,
                                    static_cast<int>(y * kGrid / faceChip.nr()));
            const int index = gy * kGrid + gx;
            cellMean[index] += luma;
            ++cellCount[index];
        }
    }
    if (samples.size() < 16) return;

    std::sort(samples.begin(), samples.end());
    const float p10 = samples[samples.size() / 10];
    const float p90 = samples[(samples.size() * 9) / 10];
    const float mean = static_cast<float>(total / samples.size());
    const float leftMean = leftCount ? static_cast<float>(left / leftCount) : mean;
    const float rightMean = rightCount ? static_cast<float>(right / rightCount) : mean;
    const float sideDelta = std::abs(leftMean - rightMean) / std::max(1.0f, mean);
    if (p90 - p10 < 120.0f && sideDelta < 0.30f) return;

    for (long y = 0; y < faceChip.nr(); ++y) {
        for (long x = 0; x < faceChip.nc(); ++x) {
            const int gx = std::min(kGrid - 1,
                                    static_cast<int>(x * kGrid / faceChip.nc()));
            const int gy = std::min(kGrid - 1,
                                    static_cast<int>(y * kGrid / faceChip.nr()));
            const int index = gy * kGrid + gx;
            const float local = cellCount[index]
                ? cellMean[index] / cellCount[index] : mean;
            const float correction = std::max(-32.0f, std::min(32.0f,
                mean - local)) * 0.45f;
            auto& p = faceChip(y, x);
            const float oldLuma = Luma(p);
            const float newLuma = std::max(0.0f, std::min(255.0f, oldLuma + correction));
            const float scale = oldLuma > 1.0f
                ? newLuma / oldLuma
                : (newLuma > 0.0f ? newLuma : 1.0f);
            p.red = static_cast<unsigned char>(std::max(0.0f, std::min(255.0f, p.red * scale)));
            p.green = static_cast<unsigned char>(std::max(0.0f, std::min(255.0f, p.green * scale)));
            p.blue = static_cast<unsigned char>(std::max(0.0f, std::min(255.0f, p.blue * scale)));
        }
    }
}

PhotometricSession::~PhotometricSession() { End(); }

float PhotometricSession::Clamp(float value, float low, float high) {
    return std::max(low, std::min(high, value));
}

void PhotometricSession::Configure(const PhotometricConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_config = config;
    m_config.targetLuma = Clamp(m_config.targetLuma, 40.0f, 210.0f);
    m_config.toleranceBand = Clamp(m_config.toleranceBand, 5.0f, 60.0f);
    m_config.minDigitalGain = Clamp(m_config.minDigitalGain, 0.25f, 1.0f);
    m_config.maxDigitalGain = Clamp(m_config.maxDigitalGain, 1.0f, 3.0f);
    m_config.hardwareStepIntervalMs = std::max(250, std::min(2000, m_config.hardwareStepIntervalMs));
}

void PhotometricSession::Attach(IAMVideoProcAmp* videoProcAmp,
                                IAMCameraControl* cameraControl) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    End();
    m_adapter = CreateCameraControlAdapter(videoProcAmp, cameraControl);
    m_state = HardwareControlState::Disabled;
}

void PhotometricSession::SetAdapterForTesting(
    std::unique_ptr<CameraControlAdapter> adapter) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    End();
    m_adapter = std::move(adapter);
    m_state = HardwareControlState::Disabled;
}

bool PhotometricSession::Begin() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_started = true;
    m_lastHardwareStep = {};
    m_pendingResponse = false;
    m_noResponseCount = 0;
    m_reversedCount = 0;
    m_hasFilteredLuma = false;
    m_filteredLuma = 0.0f;
    m_darkEvidence = 0;
    m_brightEvidence = 0;
    m_hasSmoothedGain = false;
    m_smoothedGain = 1.0f;
    m_hasSmoothedGamma = false;
    m_smoothedGamma = 1.0f;
    m_lastTransform = {};
    if (m_config.mode == PhotometricMode::Off) {
        m_state = HardwareControlState::Disabled;
        return true;
    }
    if (m_config.mode == PhotometricMode::SoftwareOnly || !m_adapter) {
        m_state = HardwareControlState::SoftwareOnly;
        return true;
    }
    m_state = HardwareControlState::Probing;
    if (!m_adapter->Probe() || !m_adapter->PrepareManualControl()) {
        m_state = HardwareControlState::SoftwareOnly;
        FACELOGIN_WARN(L"Photometric hardware control unavailable; using software-only normalization for this session");
        return true;
    }
    // Manual control has been prepared and read back, but the driver has not
    // yet proved that a real frame responds in the requested direction. Keep
    // the session in Probing until the first feedback-verified step.
    m_state = HardwareControlState::Probing;
    FACELOGIN_INFO(L"Photometric hardware control probing; software normalization remains enabled");
    return true;
}

void PhotometricSession::End() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_adapter && m_started) {
        if (!m_adapter->RestoreOriginalState()) {
            FACELOGIN_WARN(L"Photometric hardware state could not be fully restored");
        }
    }
    m_started = false;
    if (m_state != HardwareControlState::Disabled) m_state = HardwareControlState::Restored;
    m_pendingResponse = false;
}

bool PhotometricSession::HardwareActive() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_started &&
        (m_state == HardwareControlState::Probing || m_state == HardwareControlState::Active) &&
        m_adapter;
}

bool PhotometricSession::Enabled() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_config.mode != PhotometricMode::Off;
}

HardwareControlState PhotometricSession::State() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_state;
}

FramePhotometricTransform PhotometricSession::LastTransform() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_lastTransform;
}

void PhotometricSession::Demote(HardwareControlState state, const wchar_t* reason) {
    if (m_adapter) m_adapter->RestoreOriginalState();
    m_state = state;
    m_pendingResponse = false;
    FACELOGIN_WARN(L"Photometric hardware control demoted for current session: %s", reason);
}

void PhotometricSession::UpdateHardware(const FacePhotometricStats& stats) {
    if (!HardwareActive() || !stats.valid) return;

    // Face boxes and landmarks move by a few pixels even when the camera and
    // lighting are static.  Use a short EMA for the hardware loop and require
    // repeated evidence before writing a new camera value.
    if (!m_hasFilteredLuma) {
        m_filteredLuma = stats.medianLuma;
        m_hasFilteredLuma = true;
    } else {
        m_filteredLuma += (stats.medianLuma - m_filteredLuma) * 0.25f;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::milliseconds(m_config.hardwareStepIntervalMs);
    if (m_pendingResponse && now - m_lastHardwareStep >= interval) {
        if (m_adapter->VerifyResponse(m_pendingDirection, m_pendingLuma, m_filteredLuma)) {
            m_pendingResponse = false;
            m_noResponseCount = 0;
            m_reversedCount = 0;
            m_darkEvidence = 0;
            m_brightEvidence = 0;
            if (m_state == HardwareControlState::Probing) {
                m_state = HardwareControlState::Active;
                FACELOGIN_INFO(L"Photometric hardware response verified; hardware control active");
            }
        } else {
            const float delta = stats.medianLuma - m_pendingLuma;
            if (std::abs(delta) >= 2.0f &&
                ((m_pendingDirection > 0 && delta < 0.0f) ||
                 (m_pendingDirection < 0 && delta > 0.0f))) {
                if (++m_reversedCount >= 2) {
                    Demote(HardwareControlState::Reversed, L"driver brightness response reversed");
                    return;
                }
            } else if (++m_noResponseCount >= 3) {
                Demote(HardwareControlState::Unresponsive, L"camera did not respond to verified steps");
                return;
            }
        }
    }
    if (m_pendingResponse || now - m_lastHardwareStep < interval) return;

    const float lower = m_config.targetLuma - m_config.toleranceBand;
    const float upper = m_config.targetLuma + m_config.toleranceBand;
    constexpr float kHardwareHysteresis = 8.0f;
    int direction = 0;
    const bool localContrast = stats.p90Luma - stats.p10Luma > 180.0f ||
        stats.leftRightDelta > 0.45f;
    if (!localContrast) {
        const bool bright = stats.clippedRatio >= 0.02f ||
            m_filteredLuma > upper + kHardwareHysteresis;
        const bool dark = stats.shadowRatio >= 0.35f &&
            m_filteredLuma < lower - kHardwareHysteresis;
        if (bright) {
            ++m_brightEvidence;
            m_darkEvidence = 0;
        } else if (dark) {
            ++m_darkEvidence;
            m_brightEvidence = 0;
        } else {
            m_darkEvidence = 0;
            m_brightEvidence = 0;
        }
        constexpr int kRequiredEvidence = 3;
        if (m_brightEvidence >= kRequiredEvidence) direction = -1;
        else if (m_darkEvidence >= kRequiredEvidence) direction = 1;
    } else {
        m_darkEvidence = 0;
        m_brightEvidence = 0;
    }
    if (direction == 0) return;

    bool stepped = m_adapter->StepExposure(direction);
    if (!stepped) stepped = m_adapter->StepGain(direction);
    if (!stepped) {
        Demote(HardwareControlState::SoftwareOnly, L"manual step or readback failed");
        return;
    }
    m_pendingResponse = true;
    m_pendingDirection = direction;
    m_pendingLuma = m_filteredLuma;
    m_lastHardwareStep = now;
}

FramePhotometricTransform PhotometricSession::BuildTransform(const FacePhotometricStats& stats) {
    FramePhotometricTransform transform;
    if (!stats.valid || m_config.mode == PhotometricMode::Off) return transform;

    const bool normal = stats.medianLuma >= m_config.targetLuma - m_config.toleranceBand &&
        stats.medianLuma <= m_config.targetLuma + m_config.toleranceBand &&
        stats.meanLuma >= m_config.targetLuma - m_config.toleranceBand * 1.5f &&
        stats.meanLuma <= m_config.targetLuma + m_config.toleranceBand * 1.5f &&
        stats.clippedRatio < 0.02f && stats.shadowRatio < 0.35f &&
        // A face can have a normal median while one side is under a window
        // or the forehead is clipped. Treat that distribution as unsettled;
        // median/mean alone would incorrectly declare convergence.
        stats.p90Luma - stats.p10Luma <= 180.0f &&
        stats.leftRightDelta <= 0.45f && stats.uniformity >= 0.40f;
    const bool oneSidedBlowout = stats.clippedRatio > 0.15f &&
        stats.leftRightDelta > 0.45f && stats.p10Luma < 55.0f;
    const bool cannotRecover = stats.clippedRatio > 0.35f || oneSidedBlowout ||
        (stats.shadowRatio > 0.80f && stats.medianLuma < 40.0f);
    transform.unrecoverable = cannotRecover;

    float desiredGain = 1.0f;
    if (!normal) {
        if (stats.medianLuma > 1.0f) desiredGain = m_config.targetLuma / stats.medianLuma;
        desiredGain = Clamp(desiredGain, m_config.minDigitalGain, m_config.maxDigitalGain);
        if (stats.clippedRatio >= 0.02f) desiredGain = std::min(1.0f, desiredGain);
    }

    if (!m_hasSmoothedGain) {
        m_smoothedGain = desiredGain;
        m_hasSmoothedGain = true;
    } else {
        // A 15% one-frame change is visible in the Console preview. Keep the
        // transformation continuous even when the raw luma crosses the
        // normal-band boundary by one or two units.
        constexpr float maxRatio = 1.05f;
        m_smoothedGain = Clamp(desiredGain,
                               m_smoothedGain / maxRatio,
                               m_smoothedGain * maxRatio);
    }
    transform.gain = Clamp(m_smoothedGain, m_config.minDigitalGain, m_config.maxDigitalGain);

    // Gamma is only used for genuinely dark distributions.  It is applied to
    // luma while preserving the original chroma ratios.
    float desiredGamma = 1.0f;
    if (stats.medianLuma < m_config.targetLuma - m_config.toleranceBand &&
        stats.shadowRatio >= 0.35f) {
        const float ratio = Clamp(stats.medianLuma / 255.0f, 0.05f, 0.95f);
        desiredGamma = Clamp(std::log(m_config.targetLuma / 255.0f) /
                             std::log(ratio), 0.65f, 1.0f);
    }
    if (!m_hasSmoothedGamma) {
        m_smoothedGamma = desiredGamma;
        m_hasSmoothedGamma = true;
    } else {
        constexpr float kGammaStep = 0.04f;
        m_smoothedGamma = Clamp(desiredGamma,
                                m_smoothedGamma - kGammaStep,
                                m_smoothedGamma + kGammaStep);
    }
    transform.gamma = Clamp(m_smoothedGamma, 0.65f, 1.0f);
    transform.applied = std::abs(transform.gain - 1.0f) > 0.01f ||
                        std::abs(transform.gamma - 1.0f) > 0.01f;
    return transform;
}

void PhotometricSession::ApplyTransform(dlib::matrix<dlib::rgb_pixel>& frame,
                                        const FramePhotometricTransform& transform) {
    if (!transform.applied || frame.size() == 0) return;
    for (long y = 0; y < frame.nr(); ++y) {
        for (long x = 0; x < frame.nc(); ++x) {
            auto& p = frame(y, x);
            const float oldLuma = Luma(p);
            float newLuma = Clamp(oldLuma * transform.gain, 0.0f, 255.0f);
            if (transform.gamma != 1.0f) {
                newLuma = 255.0f * std::pow(Clamp(newLuma / 255.0f, 0.0f, 1.0f), transform.gamma);
            }
            const float scale = oldLuma > 1.0f ? newLuma / oldLuma : (newLuma > 0.0f ? newLuma : 1.0f);
            p.red = static_cast<unsigned char>(Clamp(p.red * scale, 0.0f, 255.0f));
            p.green = static_cast<unsigned char>(Clamp(p.green * scale, 0.0f, 255.0f));
            p.blue = static_cast<unsigned char>(Clamp(p.blue * scale, 0.0f, 255.0f));
        }
    }
}

FacePhotometricStats PhotometricSession::ProcessStats(
    dlib::matrix<dlib::rgb_pixel>& frame,
    const FacePhotometricStats& stats,
    bool allowHardware) {
    if (allowHardware) UpdateHardware(stats);
    m_lastTransform = BuildTransform(stats);
    ApplyTransform(frame, m_lastTransform);
    return stats;
}

FacePhotometricStats PhotometricSession::ProcessFrame(
    dlib::matrix<dlib::rgb_pixel>& frame,
    const dlib::rectangle& faceRect,
    const dlib::full_object_detection* landmarks) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto stats = MeasureFacePhotometricStats(frame, faceRect, landmarks);
    // A tiny crop is dominated by interpolation/noise and cannot provide a
    // stable photometric estimate. Let the caller wait for a closer frame
    // instead of amplifying it into a misleading embedding.
    if (!faceRect.is_empty() &&
        (faceRect.width() < 48 || faceRect.height() < 48)) {
        m_lastTransform = {};
        m_lastTransform.unrecoverable = true;
        return stats;
    }
    return ProcessStats(frame, stats, true);
}

FacePhotometricStats PhotometricSession::NormalizeForDetection(
    dlib::matrix<dlib::rgb_pixel>& frame) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto stats = MeasureFacePhotometricStats(
        frame, dlib::rectangle(0, 0, frame.nc() - 1, frame.nr() - 1), nullptr);
    return ProcessStats(frame, stats, false);
}

bool UnifiedFacePipeline::ProcessFrame(const dlib::matrix<dlib::rgb_pixel>& raw,
                                       const dlib::rectangle& faceRect,
                                       const dlib::full_object_detection& landmarks,
                                       UnifiedFaceFrame& output) {
    // The downstream ArcFace alignment and blink detector use the documented
    // 106-point layout. Reject a partial landmark result here rather than
    // letting a backend-specific short result reach an index-based consumer.
    if (raw.size() == 0 || faceRect.is_empty() || landmarks.num_parts() < 106) return false;
    output = {};
    output.rawFrame = raw;
    output.normalizedFrame = raw;
    output.faceRect = faceRect;
    output.landmarks = landmarks;
    output.faceDetected = true;
    output.stats = m_session.ProcessFrame(output.normalizedFrame, faceRect, &output.landmarks);
    output.transform = m_session.LastTransform();
    output.qualityAccepted = output.stats.valid && !output.transform.unrecoverable;
    return output.qualityAccepted;
}

} // namespace facelogin
