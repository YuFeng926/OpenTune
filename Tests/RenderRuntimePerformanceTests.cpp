#include <cstdio>

int main()
{
    // The plan requires user-approved baseline samples and thresholds.  No
    // numeric limit is invented here; this remains a hard, machine-readable
    // non-acceptance until the parameter table is supplied.
    std::fprintf(stderr,
                 "{\"event\":\"render_runtime_performance\",\"result\":\"failure\","
                 "\"reason\":\"approved_baseline_and_thresholds_unavailable\"}\n");
    return 1;
}
