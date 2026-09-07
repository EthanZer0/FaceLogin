#include "../common/photometric_pipeline.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

using namespace facelogin;

static dlib::matrix<dlib::rgb_pixel> MakeFrame(unsigned char value) {
    dlib::matrix<dlib::rgb_pixel> frame(64, 64);
    for (long y = 0; y < frame.nr(); ++y) {
        for (long x = 0; x < frame.nc(); ++x) {
            frame(y, x) = dlib::rgb_pixel(value, value, value);
        }
    }
    return frame;
}

static dlib::matrix<dlib::rgb_pixel> MakeSplitChip() {
    dlib::matrix<dlib::rgb_pixel> chip(112, 112);
    for (long y = 0; y < chip.nr(); ++y) {
        for (long x = 0; x < chip.nc(); ++x) {
            const unsigned char value = x < chip.nc() / 2 ? 45 : 175;
            chip(y, x) = dlib::rgb_pixel(value, value, value);
        }
    }
    return chip;
}

class FakeCameraAdapter final : public CameraControlAdapter {
public:
    bool Probe() override { return true; }
    bool PrepareManualControl() override { m_state = HardwareControlState::Active; return true; }
    bool StepExposure(int direction) override { lastDirection = direction; ++steps; return true; }
    bool StepGain(int) override { return false; }
    bool VerifyResponse(int direction, float before, float after) override {
        return direction != 0 && after != before;
    }
    bool RestoreOriginalState() override { m_state = HardwareControlState::Restored; return true; }
    HardwareControlState State() const override { return m_state; }
    int steps = 0;
    int lastDirection = 0;
    HardwareControlState m_state = HardwareControlState::Disabled;
};

int main() {
    PhotometricSession session;
    PhotometricConfig config;
    config.mode = PhotometricMode::SoftwareOnly;
    session.Configure(config);
    assert(session.Begin());

    auto normal = MakeFrame(110);
    const auto normalStats = session.ProcessFrame(normal, dlib::rectangle(8, 8, 55, 55));
    assert(normalStats.valid);
    assert(!session.LastTransform().applied);
    assert(normal(32, 32).red == 110);

    auto dark = MakeFrame(40);
    const auto darkStats = session.ProcessFrame(dark, dlib::rectangle(8, 8, 55, 55));
    assert(darkStats.valid);
    assert(session.LastTransform().applied);
    assert(dark(32, 32).red > 40);
    assert(dark(32, 32).red <= 255);

    auto clipped = MakeFrame(255);
    const auto clippedStats = session.ProcessFrame(clipped, dlib::rectangle(8, 8, 55, 55));
    assert(clippedStats.clippedRatio > 0.9f);
    assert(session.LastTransform().unrecoverable);

    auto splitChip = MakeSplitChip();
    const unsigned char beforeLeft = splitChip(56, 20).red;
    const unsigned char beforeRight = splitChip(56, 92).red;
    ApplyLocalIlluminationCorrection(splitChip);
    const int beforeDelta = static_cast<int>(beforeRight) - beforeLeft;
    const int afterDelta = static_cast<int>(splitChip(56, 92).red) -
                           splitChip(56, 20).red;
    assert(afterDelta >= 0 && afterDelta < beforeDelta);

    session.End();

    PhotometricSession hardwareSession;
    PhotometricConfig hybrid;
    hybrid.mode = PhotometricMode::Hybrid;
    hybrid.hardwareStepIntervalMs = 250;
    hardwareSession.Configure(hybrid);
    auto fake = std::make_unique<FakeCameraAdapter>();
    auto* fakePtr = fake.get();
    hardwareSession.SetAdapterForTesting(std::move(fake));
    assert(hardwareSession.Begin());
    auto hardwareDark = MakeFrame(40);
    hardwareSession.ProcessFrame(hardwareDark, dlib::rectangle(8, 8, 55, 55));
    assert(fakePtr->steps == 1);
    assert(fakePtr->lastDirection > 0);
    assert(hardwareSession.State() == HardwareControlState::Probing);
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    auto hardwareFeedback = MakeFrame(60);
    hardwareSession.ProcessFrame(hardwareFeedback, dlib::rectangle(8, 8, 55, 55));
    assert(hardwareSession.State() == HardwareControlState::Active);
    assert(hardwareSession.HardwareActive());
    hardwareSession.End();
    return 0;
}
