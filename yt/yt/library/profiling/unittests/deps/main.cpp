#include <yt/yt/library/profiling/sensor.h>
#include <yt/yt/library/profiling/producer.h>

////////////////////////////////////////////////////////////////////////////////

using namespace NYT::NProfiling;

////////////////////////////////////////////////////////////////////////////////

int main()
{
    NYT::NProfiling::TProfiler profiler{"/foo"};
    auto gauge = profiler.Gauge("/bar");
    gauge.Update(1.0);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
