// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppReadOps.h"

#include "OpsJson.h"
#include "OpsSupport.h"
#include "PrefsModels.h"

#include "Control/ControlLogTail.h"
#include "PluginProcessor.h"
#include "Utils/AppLogger.h"
#include "Utils/AppPreferences.h"

namespace OpenTune {

namespace {

/** Longest path (common.schema.json path). */
constexpr std::size_t kMaxPathCodePoints = 4096;
constexpr std::int64_t kLogDefaultLines = 200;
constexpr std::int64_t kLogMaxLines = 1000;

Result<JsonValue> runPrefsGet(ControlOpContext& context, const ControlNoParams&)
{
    auto& preferences = context.processor().getAppPreferences();
    // AppPreferences::getState(): a copy taken under the preferences' own mutex.
    const auto shared = preferences.getState().shared;

    // AppPreferences::getRecentProjects(): the most recently used project files, newest first (at most 10).
    // Whether a file still exists is a synchronous stat of each path on the message thread, as the
    // upstream recent-projects menu does (ProjectSession::getRecentProjects(), which drops the missing
    // ones when the editor fills the menu); a path on an unreachable network share can block both alike.
    auto recent = JsonValue::array();
    for (const auto& path : preferences.getRecentProjects()) {
        if (path.isEmpty())
            continue;
        auto entry = JsonValue::object();
        entry.appendMember("path", controlJsonText(path.toStdString(), kMaxPathCodePoints));
        entry.appendMember("exists", JsonValue::boolean(juce::File(path).existsAsFile()));
        recent.append(std::move(entry));
    }

    // processing.options is reported by prefs.get only (session.info shares controlProcessingJson() without it): what
    // the preferences window offers for the vocoder weight, the pitch model and the rendering priority.
    auto processing = controlProcessingJson(shared, nullptr);
    processing.appendMember("options", controlPrefsOptionsJson());

    auto result = JsonValue::object();
    result.appendMember("processing", std::move(processing));
    result.appendMember("onboardingShown", JsonValue::boolean(shared.onboardingShown));
    result.appendMember("language", JsonValue::string(controlLanguageTag(shared.language)));
    result.appendMember("recentProjects", std::move(recent));
    return result;
}

struct LogParams
{
    std::int64_t maxLines = kLogDefaultLines;
};

bool parseLogParams(const JsonValue& params, LogParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::int64_t> maxLines;
    if (!reader.onlyMembers({"maxLines"}) || !reader.optionalInteger("maxLines", 1, kLogMaxLines, maxLines)) {
        why = reader.why();
        return false;
    }
    out.maxLines = maxLines.value_or(kLogDefaultLines);
    return true;
}

/** Runs on the connection thread (x-otcp.mainThread false): it reads the log file and the facts snapshot
    only, never project state. */
Result<JsonValue> runGetLog(const ControlInstanceFacts& facts, const LogParams& params)
{
    // AppLogger::getCurrentLogFile(): the file the process-wide logger writes to, copied under the
    // logger's own lock, so any thread may ask (the logger lives as long as the process and is written
    // from worker threads too); empty until the logger was initialised.
    const auto file = AppLogger::getCurrentLogFile();
    ControlLogTail tail;
    if (file != juce::File())
        tail = controlReadLogTail(std::filesystem::u8path(file.getFullPathName().toStdString()),
                                  static_cast<std::size_t>(params.maxLines), facts.redactionSecrets,
                                  facts.homeDirectorySpellings);
    auto result = JsonValue::object();
    if (!tail.readable) {
        result.appendMember("lines", JsonValue());
        result.appendMember("truncated", JsonValue::boolean(false));
        auto nullReasons = JsonValue::object();
        nullReasons.appendMember("lines", JsonValue::string("unavailable"));
        result.appendMember("nullReasons", std::move(nullReasons));
        return result;
    }
    auto lines = JsonValue::array();
    for (const auto& line : tail.lines)
        lines.append(JsonValue::string(line));
    result.appendMember("lines", std::move(lines));
    result.appendMember("truncated", JsonValue::boolean(tail.truncated));
    return result;
}

} // namespace

void registerAppReadOps(ControlMethodTable& table)
{
    table.add(controlReadMethod<ControlNoParams>("prefs.get", controlParseNoParams, runPrefsGet));
    table.add(controlOffMainThreadReadMethod<LogParams>("app.getLog", parseLogParams, runGetLog));
}

} // namespace OpenTune
