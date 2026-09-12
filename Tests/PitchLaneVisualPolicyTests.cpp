/**
 * PitchLaneVisualPolicyTests
 *
 * 纯标准库测试（无 JUCE 依赖），验证 PitchLaneVisualMode 视觉策略：
 *  - PianoKeys 模式下黑白角色不受 inScale 影响
 *  - ScaleAssist 模式下调内→WhiteKey，调外→BlackKey
 *  - D Major 下 C#（调内 accidental）→WhiteKey，C（调外 natural）→BlackKey
 *  - Chromatic 全部映射为 WhiteKey（所有半音在调内）
 *  - 角色集合只有两种：WhiteKey / BlackKey，不存在 InScale / OutOfScale
 *
 * 编译：纯 C++17，无框架，返回 0 表示全部通过。
 */

#include "../Source/Utils/PianoRollVisualPreferences.h"
#include "../Source/Utils/NoteGeneratorTypes.h"

#include <cstdio>

using namespace OpenTune;

// ============================================================================
// 测试用例
// ============================================================================

static int gFailures = 0;

static void check(bool condition, const char* msg)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++gFailures;
    }
}

// 1. PianoKeys 模式：物理黑白键角色，不受 inScale 影响
static void testPianoKeysRolesIgnoreInScale()
{
    // C# (midi=61) 是物理黑键，无论 inScale 是 true 还是 false
    auto roleInScale = classifyPitchRow(PitchLaneVisualMode::PianoKeys, 61, true);
    auto roleOutScale = classifyPitchRow(PitchLaneVisualMode::PianoKeys, 61, false);
    check(roleInScale == PitchRowVisualRole::BlackKey,
          "PianoKeys: C# should be BlackKey regardless of inScale=true");
    check(roleOutScale == PitchRowVisualRole::BlackKey,
          "PianoKeys: C# should be BlackKey regardless of inScale=false");
    check(roleInScale == roleOutScale,
          "PianoKeys: role should be identical for same midi regardless of inScale");

    // D (midi=62) 是物理白键，无论 inScale 是 true 还是 false
    auto roleDIn = classifyPitchRow(PitchLaneVisualMode::PianoKeys, 62, true);
    auto roleDOut = classifyPitchRow(PitchLaneVisualMode::PianoKeys, 62, false);
    check(roleDIn == PitchRowVisualRole::WhiteKey,
          "PianoKeys: D should be WhiteKey regardless of inScale=true");
    check(roleDOut == PitchRowVisualRole::WhiteKey,
          "PianoKeys: D should be WhiteKey regardless of inScale=false");
    check(roleDIn == roleDOut,
          "PianoKeys: D role should be identical regardless of inScale");
}

// 2. ScaleAssist D Major：C# 调内→WhiteKey，D 调内→WhiteKey，C 调外→BlackKey
static void testScaleAssistDMajor()
{
    // D Major: C# (pitch class 1) 调内，D (pitch class 2) 调内，C (pitch class 0) 调外
    const auto mask = buildPitchClassMask(ScaleMode::Major, 2);

    // C# = pitch class 1 — 调内
    check(mask[1] == true, "D Major: C# (pitch class 1) should be in scale");
    auto roleCs = classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 61, mask[1]);
    check(roleCs == PitchRowVisualRole::WhiteKey,
          "ScaleAssist D Major: C# in scale should be WhiteKey");

    // D = pitch class 2 — 调内
    check(mask[2] == true, "D Major: D (pitch class 2) should be in scale");
    auto roleD = classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 62, mask[2]);
    check(roleD == PitchRowVisualRole::WhiteKey,
          "ScaleAssist D Major: D in scale should be WhiteKey");

    // C = pitch class 0 — 调外
    check(mask[0] == false, "D Major: C (pitch class 0) should be out of scale");
    auto roleC = classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 60, mask[0]);
    check(roleC == PitchRowVisualRole::BlackKey,
          "ScaleAssist D Major: C out of scale should be BlackKey");
}

// 3. Chromatic：全部映射为 WhiteKey（所有半音在调内）
static void testChromaticAllWhiteKey()
{
    // Chromatic mask 全 true → classifyPitchRow 收到 inScale=true → 全部 WhiteKey
    const auto chromaticMask = buildPitchClassMask(ScaleMode::Chromatic, 0);
    for (int pc = 0; pc < 12; ++pc) {
        check(chromaticMask[pc] == true,
              "Chromatic mask: all pitch classes should be true");

        auto role = classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 60 + pc, chromaticMask[pc]);
        check(role == PitchRowVisualRole::WhiteKey,
              "Chromatic: all pitch classes should be WhiteKey in ScaleAssist");
    }
}

// 4. 角色集合互斥：只有 WhiteKey / BlackKey，没有 InScale / OutOfScale
static void testOnlyTwoRoles()
{
    // PianoKeys 模式只返回 WhiteKey 或 BlackKey
    for (int midi = 21; midi <= 108; ++midi) {
        auto role = classifyPitchRow(PitchLaneVisualMode::PianoKeys, midi, true);
        check(role == PitchRowVisualRole::WhiteKey || role == PitchRowVisualRole::BlackKey,
              "PianoKeys: role must be WhiteKey or BlackKey only");
    }

    // ScaleAssist 模式也只返回 WhiteKey 或 BlackKey
    for (int midi = 21; midi <= 108; ++midi) {
        auto roleIn = classifyPitchRow(PitchLaneVisualMode::ScaleAssist, midi, true);
        auto roleOut = classifyPitchRow(PitchLaneVisualMode::ScaleAssist, midi, false);
        check(roleIn == PitchRowVisualRole::WhiteKey,
              "ScaleAssist: inScale=true must return WhiteKey");
        check(roleOut == PitchRowVisualRole::BlackKey,
              "ScaleAssist: inScale=false must return BlackKey");
    }

    // 两种模式返回的角色集合完全相同
    for (int midi = 21; midi <= 108; ++midi) {
        auto pkRole = classifyPitchRow(PitchLaneVisualMode::PianoKeys, midi, true);
        auto saRole = classifyPitchRow(PitchLaneVisualMode::ScaleAssist, midi, true);
        // 角色类型必须相同（都是 WhiteKey 或 BlackKey），不要求值相等
        check(pkRole == PitchRowVisualRole::WhiteKey || pkRole == PitchRowVisualRole::BlackKey,
              "Both modes: role must be WhiteKey or BlackKey");
        check(saRole == PitchRowVisualRole::WhiteKey || saRole == PitchRowVisualRole::BlackKey,
              "Both modes: role must be WhiteKey or BlackKey");
    }
}

// 5. PianoKeys 物理映射锁定：验证所有 12 pitch class 的物理黑白属性
static void testPianoKeysPhysicalMapping()
{
    // pitch class 1,3,6,8,10 是黑键（暗）
    // pitch class 0,2,4,5,7,9,11 是白键（亮）
    static constexpr bool kExpectedBlack[12] = {
        false, true, false, true, false, false,
        true, false, true, false, true, false
    };
    for (int pc = 0; pc < 12; ++pc) {
        int midi = 60 + pc; // C4 + pc
        auto role = classifyPitchRow(PitchLaneVisualMode::PianoKeys, midi, true);
        if (kExpectedBlack[pc]) {
            check(role == PitchRowVisualRole::BlackKey,
                  "PianoKeys: black key pitch class should be BlackKey");
        } else {
            check(role == PitchRowVisualRole::WhiteKey,
                  "PianoKeys: white key pitch class should be WhiteKey");
        }
    }
}

// 6. 物理角色 vs 视觉角色分离合同：
//    D Major + ScaleAssist 下，C#（调内 accidental）的 physicalRole 仍然是 BlackKey
//    但 visualRole 是 WhiteKey；C（调外 natural）的 physicalRole 是 WhiteKey 但
//    visualRole 是 BlackKey。geometry 不能作为 mode role 本身。
static void testPhysicalVsVisualRoleSeparation()
{
    // PianoKeys 模式下 classifyPitchRow 始终返回物理角色（忽略 inScale）
    // 用于表示 physicalRole。
    auto physicalRole = [](int midiNote) {
        return classifyPitchRow(PitchLaneVisualMode::PianoKeys, midiNote, true);
    };

    const auto mask = buildPitchClassMask(ScaleMode::Major, 2); // D Major

    // C# (midi=61, pitch class 1) — 调内
    check(mask[1] == true, "D Major: C# should be in scale");
    check(physicalRole(61) == PitchRowVisualRole::BlackKey,
          "D Major: C# physicalRole must be BlackKey (physical black key)");
    check(classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 61, mask[1]) == PitchRowVisualRole::WhiteKey,
          "D Major: C# visualRole must be WhiteKey (in-scale, bright palette)");

    // C (midi=60, pitch class 0) — 调外
    check(mask[0] == false, "D Major: C should be out of scale");
    check(physicalRole(60) == PitchRowVisualRole::WhiteKey,
          "D Major: C physicalRole must be WhiteKey (physical white key)");
    check(classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 60, mask[0]) == PitchRowVisualRole::BlackKey,
          "D Major: C visualRole must be BlackKey (out-of-scale, dark palette)");

    // 验证 physicalRole 与 visualRole 可能不同（geometry ≠ palette）
    check(physicalRole(61) != classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 61, mask[1]),
          "D Major: C# physical and visual roles must differ (BlackKey vs WhiteKey)");
    check(physicalRole(60) != classifyPitchRow(PitchLaneVisualMode::ScaleAssist, 60, mask[0]),
          "D Major: C physical and visual roles must differ (WhiteKey vs BlackKey)");
}

int main()
{
    testPianoKeysRolesIgnoreInScale();
    testScaleAssistDMajor();
    testChromaticAllWhiteKey();
    testOnlyTwoRoles();
    testPianoKeysPhysicalMapping();
    testPhysicalVsVisualRoleSeparation();

    if (gFailures == 0) {
        std::printf("All PitchLaneVisualPolicy tests passed.\n");
        return 0;
    }

    std::fprintf(stderr, "%d test(s) FAILED.\n", gFailures);
    return 1;
}
