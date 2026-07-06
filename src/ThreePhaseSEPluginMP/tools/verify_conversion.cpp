// Standalone harness that mirrors the worker/progress threading and the
// polar -> rectangular math in ThreePhaseSEPluginMP.cpp, without any natID
// SDK dependency, so it can be compiled and actually executed to check:
//   1) the conversion formula is correct,
//   2) the two-thread / std::atomic progress handoff has no data race.
//
// Build & run (g++/MinGW or any C++17 compiler with thread support):
//   g++ -std=c++17 -O2 -pthread verify_conversion.cpp -o verify_conversion.exe
//   ./verify_conversion.exe
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

constexpr int kNumPhases = 3;
constexpr double kPi = 3.14159265358979323846;

struct Bus3Ph
{
    int busId;
    double Vm[3]; // p.u.
    double Va[3]; // degrees
};

struct Result
{
    bool ok = false;
    std::vector<std::array<double, 3>> Vr;
    std::vector<std::array<double, 3>> Vi;
    int finalProgressSteps = -1;
    int maxObservedSteps = 0;
    bool monotonic = true;
};

static Result convert(const std::vector<Bus3Ph>& buses, int progressPollMs)
{
    Result result;
    size_t nodeCount = buses.size();
    result.Vr.assign(nodeCount, {0, 0, 0});
    result.Vi.assign(nodeCount, {0, 0, 0});

    unsigned totalSteps = unsigned(nodeCount) * kNumPhases;

    std::atomic<unsigned> processedSteps{0};
    std::atomic_bool workerDone{false};
    std::atomic_bool workerOk{true};

    int lastSeen = 0;
    std::thread progressThread([&]() {
        while (!workerDone.load())
        {
            unsigned p = processedSteps.load();
            if (int(p) < lastSeen)
                result.monotonic = false; // would indicate a torn/raced read
            lastSeen = int(p);
            if (int(p) > result.maxObservedSteps)
                result.maxObservedSteps = int(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(progressPollMs));
        }
        result.finalProgressSteps = int(processedSteps.load());
    });

    std::thread workerThread([&]() {
        for (size_t node = 0; node < nodeCount; ++node)
        {
            for (int phase = 0; phase < kNumPhases; ++phase)
            {
                double Vm = buses[node].Vm[phase];
                double VaDeg = buses[node].Va[phase];
                double VaRad = VaDeg * kPi / 180.0;
                result.Vr[node][phase] = Vm * std::cos(VaRad);
                result.Vi[node][phase] = Vm * std::sin(VaRad);
                processedSteps.fetch_add(1);
            }
        }
        workerDone.store(true);
    });

    workerThread.join();
    progressThread.join();

    result.ok = workerOk.load();
    (void) totalSteps;
    return result;
}

static bool nearlyEqual(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) < eps;
}

int main()
{
    int failures = 0;

    // --- Test 1: known angles, hand-verifiable rectangular components ---
    {
        std::vector<Bus3Ph> buses = {
            {1, {1.00, 1.00, 1.00}, {0.0, -120.0, 120.0}},
            {2, {0.98, 0.98, 0.98}, {0.0, -120.0, 120.0}},
        };
        auto r = convert(buses, 1);

        struct Expect { double Vr, Vi; };
        Expect e[3] = {
            {1.0, 0.0},
            {-0.5, -0.8660254037844386},
            {-0.5, 0.8660254037844386},
        };
        for (int phase = 0; phase < 3; ++phase)
        {
            if (!nearlyEqual(r.Vr[0][phase], e[phase].Vr) || !nearlyEqual(r.Vi[0][phase], e[phase].Vi))
            {
                std::printf("FAIL test1 node0 phase%d: got Vr=%.10f Vi=%.10f expected Vr=%.10f Vi=%.10f\n",
                            phase, r.Vr[0][phase], r.Vi[0][phase], e[phase].Vr, e[phase].Vi);
                ++failures;
            }
        }
        // node 1 (Vm=0.98) must scale linearly with magnitude, same angles
        for (int phase = 0; phase < 3; ++phase)
        {
            double expVr = 0.98 * e[phase].Vr;
            double expVi = 0.98 * e[phase].Vi;
            if (!nearlyEqual(r.Vr[1][phase], expVr) || !nearlyEqual(r.Vi[1][phase], expVi))
            {
                std::printf("FAIL test1 node1 phase%d: got Vr=%.10f Vi=%.10f expected Vr=%.10f Vi=%.10f\n",
                            phase, r.Vr[1][phase], r.Vi[1][phase], expVr, expVi);
                ++failures;
            }
        }
        if (r.finalProgressSteps != 6)
        {
            std::printf("FAIL test1 progress: expected 6 final steps, got %d\n", r.finalProgressSteps);
            ++failures;
        }
        if (!r.monotonic)
        {
            std::printf("FAIL test1: progress counter observed going backwards (race)\n");
            ++failures;
        }
        std::printf("test1 (known angles): %s\n", failures == 0 ? "OK" : "FAILED");
    }

    // --- Test 2: |V*e^(j*theta)| must equal Vm exactly (round-trip magnitude) ---
    {
        std::vector<Bus3Ph> buses;
        for (int i = 1; i <= 200; ++i)
        {
            Bus3Ph b;
            b.busId = i;
            b.Vm[0] = 0.9 + 0.0007 * i;
            b.Vm[1] = 0.95 + 0.0003 * i;
            b.Vm[2] = 1.0 - 0.0002 * i;
            b.Va[0] = -1.5 * (i % 7);
            b.Va[1] = -120.0 + 0.3 * (i % 11);
            b.Va[2] = 120.0 - 0.25 * (i % 9);
            buses.push_back(b);
        }
        auto r = convert(buses, 0);
        int magFailures = 0;
        for (size_t node = 0; node < buses.size(); ++node)
        {
            for (int phase = 0; phase < 3; ++phase)
            {
                double mag = std::sqrt(r.Vr[node][phase] * r.Vr[node][phase] + r.Vi[node][phase] * r.Vi[node][phase]);
                if (!nearlyEqual(mag, buses[node].Vm[phase], 1e-9))
                    ++magFailures;
            }
        }
        if (magFailures > 0)
        {
            std::printf("FAIL test2: %d/%zu magnitude round-trip mismatches\n", magFailures, buses.size() * 3);
            failures += magFailures;
        }
        if ((unsigned) r.finalProgressSteps != buses.size() * 3)
        {
            std::printf("FAIL test2 progress: expected %zu final steps, got %d\n", buses.size() * 3, r.finalProgressSteps);
            ++failures;
        }
        std::printf("test2 (200-node magnitude round-trip): %s\n", magFailures == 0 ? "OK" : "FAILED");
    }

    // --- Test 3: repeated stress runs to shake out timing-dependent races ---
    {
        int raceObservations = 0;
        const int kRuns = 500;
        for (int run = 0; run < kRuns; ++run)
        {
            std::vector<Bus3Ph> buses;
            int n = 50 + (run % 30);
            for (int i = 1; i <= n; ++i)
                buses.push_back({i, {1.0, 1.0, 1.0}, {0.0, -120.0, 120.0}});
            auto r = convert(buses, 0); // poll as fast as possible: most likely to expose a race
            if (!r.monotonic)
                ++raceObservations;
            if ((unsigned) r.finalProgressSteps != buses.size() * 3)
                ++raceObservations;
            for (size_t node = 0; node < buses.size(); ++node)
                if (!nearlyEqual(r.Vr[node][0], 1.0) || !nearlyEqual(r.Vi[node][1], -0.8660254037844386))
                    ++raceObservations;
        }
        if (raceObservations > 0)
        {
            std::printf("FAIL test3: %d anomalies across %d stress runs\n", raceObservations, kRuns);
            failures += raceObservations;
        }
        std::printf("test3 (%d stress runs, fast polling): %s\n", kRuns, raceObservations == 0 ? "OK" : "FAILED");
    }

    std::printf("\n%s (%d failing assertions)\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
