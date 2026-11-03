#include <juce_gui_basics/juce_gui_basics.h>

#include "Standalone/UI/TransportBarComponent.h"

#include <cstdio>
#include <string>

// 纯布局测试：不实例化完整 TransportBar（避免引入 Processor 依赖），
// 仅验证 computeTierLayout 对 externalEditAvailable 的槽位分配契约。
//
// VST3AraSingleClip profile 下：
//   available=true  → 外部编辑按钮槽位非空，且其它关键槽位（record/play 等）不受影响；
//   available=false → 外部编辑按钮槽位为空，其它关键槽位不受影响。
// StandaloneFull / ExternalEdit profile 不受该开关影响（保留既有行为）。

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using TransportBar = OpenTune::TransportBarComponent;

void checkKeySlotsUnchanged(const char* context,
                            const TransportBar::TierLayout& a,
                            const TransportBar::TierLayout& b)
{
    check(a.tier == b.tier, (std::string(context) + ": tier unchanged").c_str());
    check(a.fileButton == b.fileButton, (std::string(context) + ": fileButton unchanged").c_str());
    check(a.playButton == b.playButton, (std::string(context) + ": playButton unchanged").c_str());
    check(a.pauseButton == b.pauseButton, (std::string(context) + ": pauseButton unchanged").c_str());
    check(a.stopButton == b.stopButton, (std::string(context) + ": stopButton unchanged").c_str());
    check(a.recordButton == b.recordButton, (std::string(context) + ": recordButton unchanged").c_str());
    check(a.timeDisplay == b.timeDisplay, (std::string(context) + ": timeDisplay unchanged").c_str());
    check(a.bpmField == b.bpmField, (std::string(context) + ": bpmField unchanged").c_str());
}

void testVst3ProfileExternalEditAvailability()
{
    const juce::Rectangle<int> bounds(0, 0, 4096, 64);
    const auto available = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::VST3AraSingleClip, bounds, true);
    const auto unavailable = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::VST3AraSingleClip, bounds, false);

    check(!available.externalEditButton.isEmpty(),
          "VST3 profile with capture session: external edit slot is allocated");
    check(unavailable.externalEditButton.isEmpty(),
          "VST3 profile ARA-bound: external edit slot is empty");
    check(available.requiredWidth > unavailable.requiredWidth,
          "hidden external edit frees layout width (no empty gap)");
    checkKeySlotsUnchanged("VST3 profile", available, unavailable);
}

void testOtherProfilesUnaffected()
{
    const juce::Rectangle<int> bounds(0, 0, 4096, 64);

    // 默认参数（available=true）与显式 false 在其它 profile 下行为一致。
    const auto standaloneDefault = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::StandaloneFull, bounds);
    const auto standaloneOff = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::StandaloneFull, bounds, false);
    check(standaloneDefault.externalEditButton.isEmpty(),
          "Standalone profile never allocates external edit slot");
    checkKeySlotsUnchanged("Standalone profile", standaloneDefault, standaloneOff);

    const auto externalDefault = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::ExternalEdit, bounds);
    const auto externalOff = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::ExternalEdit, bounds, false);
    check(!externalDefault.saveAndReturnButton.isEmpty(),
          "ExternalEdit profile allocates save-and-return slot");
    checkKeySlotsUnchanged("ExternalEdit profile", externalDefault, externalOff);
}

void testCompactAndOverflowTiersRespectAvailability()
{
    const juce::Rectangle<int> probe(0, 0, 4096, 64);

    const auto fullOn = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::VST3AraSingleClip, probe, true);
    const juce::Rectangle<int> compactOnBounds(0, 0, fullOn.requiredWidth - 1, 64);
    const auto compactOn = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::VST3AraSingleClip, compactOnBounds, true);
    check(compactOn.tier == TransportBar::LayoutTier::Compact, "capture compact tier reached");
    check(!compactOn.externalEditButton.isEmpty(),
          "Compact tier with capture session: external edit slot allocated");

    const auto fullOff = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::VST3AraSingleClip, probe, false);
    const juce::Rectangle<int> compactOffBounds(0, 0, fullOff.requiredWidth - 1, 64);
    const auto compactOff = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::VST3AraSingleClip, compactOffBounds, false);
    check(compactOff.tier == TransportBar::LayoutTier::Compact, "ARA compact tier reached");
    check(compactOff.externalEditButton.isEmpty(),
          "Compact tier ARA-bound: external edit slot empty");

    const juce::Rectangle<int> overflowBounds(0, 0, compactOff.requiredWidth - 1, 64);
    const auto overflowOff = TransportBar::computeTierLayout(
        TransportBar::LayoutProfile::VST3AraSingleClip, overflowBounds, false);
    check(overflowOff.tier == TransportBar::LayoutTier::Overflow, "overflow tier reached");
    check(overflowOff.externalEditButton.isEmpty(),
          "Overflow tier ARA-bound: external edit slot empty");
}

} // namespace

int main()
{
    testVst3ProfileExternalEditAvailability();
    testOtherProfilesUnaffected();
    testCompactAndOverflowTiersRespectAvailability();

    if (failures == 0)
    {
        std::puts("TransportBarExternalEditLayoutTests: all checks passed.");
        return 0;
    }

    std::fprintf(stderr, "TransportBarExternalEditLayoutTests: %d check(s) failed.\n", failures);
    return 1;
}
