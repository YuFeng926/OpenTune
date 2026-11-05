// SPDX-License-Identifier: AGPL-3.0-or-later
#include "JobOps.h"

#include "OpsSupport.h"

#include "Control/ControlJobs.h"

namespace OpenTune {

namespace {

/** The longest hold of job.get (methods/job.get.schema.json, limits.jobWaitMaxMs). */
constexpr std::int64_t kJobWaitMaxMs = 30000;

/** common.schema.json#/$defs/jobId: j:<8 lowercase hex>:<seq without leading zero>. */
bool isJobId(const std::string& text)
{
    if (text.size() < 12 || text.compare(0, 2, "j:") != 0 || text[10] != ':')
        return false;
    for (std::size_t i = 2; i < 10; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    if (text[11] == '0')
        return false;
    for (std::size_t i = 11; i < text.size(); ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

bool readJobId(ControlParamReader& reader, std::string& out)
{
    const JsonValue* value = reader.member("jobId");
    if (value == nullptr)
        return reader.fail("jobId is required");
    if (!value->isString() || !isJobId(value->text()))
        return reader.fail("jobId does not have the form of a job id");
    out = value->text();
    return true;
}

Error jobNotFound()
{
    return controlOpError("NOT_FOUND", "job_not_found", "no job with this id is retained by this instance");
}

Error shuttingDown()
{
    return controlOpError("SHUTTING_DOWN", "", "the control API is stopping");
}

// ---- job.get --------------------------------------------------------------------------------------

struct JobGetParams
{
    std::string jobId;
    int waitMs = 0;
};

bool parseJobGet(const JsonValue& params, JobGetParams& out, std::string& why)
{
    ControlParamReader reader(params);
    std::optional<std::int64_t> waitMs;
    if (!reader.onlyMembers({"jobId", "waitMs"}) || !readJobId(reader, out.jobId)
        || !reader.optionalInteger("waitMs", 0, kJobWaitMaxMs, waitMs)) {
        why = reader.why();
        return false;
    }
    out.waitMs = static_cast<int>(waitMs.value_or(0));
    return true;
}

void runJobGet(ControlJobs& jobs, const JobGetParams& params, ControlMethod::Reply reply)
{
    // ControlJobs::waitAsync(): answers at once, or later from the registry's waiter thread when the job
    // becomes terminal, waitMs passes or the registry stops. The connection thread does not wait.
    jobs.waitAsync(params.jobId, params.waitMs, [reply = std::move(reply)](ControlJobs::Lookup lookup, JsonValue job) {
        switch (lookup) {
            case ControlJobs::Lookup::Found: {
                auto result = JsonValue::object();
                result.appendMember("job", std::move(job));
                reply(std::move(result));
                return;
            }
            case ControlJobs::Lookup::NotFound:
                reply(jobNotFound());
                return;
            case ControlJobs::Lookup::Stopped:
                reply(shuttingDown());
                return;
        }
    });
}

// ---- job.cancel -----------------------------------------------------------------------------------

struct JobCancelParams
{
    std::string jobId;
};

bool parseJobCancel(const JsonValue& params, JobCancelParams& out, std::string& why)
{
    ControlParamReader reader(params);
    if (!reader.onlyMembers({"jobId"}) || !readJobId(reader, out.jobId)) {
        why = reader.why();
        return false;
    }
    return true;
}

void runJobCancel(ControlJobs& jobs, const JobCancelParams& params, ControlMethod::Reply reply)
{
    // PROTOCOL.md section 12.3 rules 1 to 5: the registry decides under its mutex; the job's work sees a
    // request at its next step on the message thread.
    JsonValue job;
    bool requested = false;
    switch (jobs.cancel(params.jobId, job)) {
        case ControlJobs::Cancel::NotFound:
            reply(jobNotFound());
            return;
        case ControlJobs::Cancel::Stopped:
            reply(shuttingDown());
            return;
        case ControlJobs::Cancel::NotCancellable:
            reply(controlOpError("UNSUPPORTED", "job_not_cancellable",
                                 "the job cannot be cancelled now; it continues unchanged"));
            return;
        case ControlJobs::Cancel::Terminal:
            requested = false;
            break;
        case ControlJobs::Cancel::Requested:
        case ControlJobs::Cancel::AlreadyRequested:
            requested = true;
            break;
    }
    auto result = JsonValue::object();
    result.appendMember("cancelRequested", JsonValue::boolean(requested));
    result.appendMember("job", std::move(job));
    reply(std::move(result));
}

} // namespace

void registerJobOps(ControlMethodTable& table)
{
    table.add(controlJobMethod<JobGetParams>("job.get", parseJobGet, runJobGet));
    table.add(controlJobMethod<JobCancelParams>("job.cancel", parseJobCancel, runJobCancel));
}

} // namespace OpenTune
