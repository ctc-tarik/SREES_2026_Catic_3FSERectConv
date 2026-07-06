// Standalone reproduction of writeDigitalModelNoisy() text generation (no natID
// SDK), so the generated noisy-WLS .dmodl can be inspected/validated against the
// known-good Digital Twin/res/files/38.dmodl grammar before building the plugin.
//
//   g++ -std=c++17 -O2 preview_noisy_dmodl.cpp -o preview_noisy_dmodl.exe
//   ./preview_noisy_dmodl.exe > sample_noisy.dmodl
#include <cmath>
#include <cstdio>
#include <vector>

constexpr int kNumPhases = 3;
constexpr int kMeasPerComp = 2;
const double kPi = 3.14159265358979323846;

int main()
{
    // Tiny 2-node example: node1 Vm=1.00, node2 Vm=0.95, angles 0/-120/120.
    struct Node { double vm[3]; double va[3]; };
    std::vector<Node> nodes = {
        {{1.00, 1.00, 1.00}, {0, -120, 120}},
        {{0.95, 0.95, 0.95}, {0, -120, 120}},
    };
    int nodeCount = (int)nodes.size();
    int nStates = nodeCount * kNumPhases;
    double dev = 0.01, width = dev * 5.0;

    // Vr/Vi per node/phase.
    std::vector<std::vector<double>> Vr(nodeCount, std::vector<double>(3)),
                                     Vi(nodeCount, std::vector<double>(3));
    for (int n = 0; n < nodeCount; ++n)
        for (int p = 0; p < 3; ++p)
        {
            double rad = nodes[n].va[p] * kPi / 180.0;
            Vr[n][p] = nodes[n].vm[p] * std::cos(rad);
            Vi[n][p] = nodes[n].vm[p] * std::sin(rad);
        }

    printf("Header:\n\tmaxIter=20\n\treport=AllDetails\nend\n");
    printf("Model [type=WLS domain=real eps=1e-5 name=\"Noisy 3F SE\"]:\n");

    printf("Vars [out=true]:\n");
    for (int n = 0; n < nodeCount; ++n)
        for (int p = 0; p < 3; ++p)
        {
            int i = n * 3 + p + 1;
            printf("\te_%d=%.8f\n\tf_%d=%.8f\n", i, Vr[n][p], i, Vi[n][p]);
        }

    printf("Params:\n");
    for (int n = 0; n < nodeCount; ++n)
        for (int p = 0; p < 3; ++p)
        {
            int i = n * 3 + p + 1;
            printf("\teT_%d=%.8f\n\tfT_%d=%.8f\n\tw_%d=1.0\n", i, Vr[n][p], i, Vi[n][p], i);
            for (int m = 1; m <= kMeasPerComp; ++m)
                printf("\tzr%d_%d=%.8f\n\tzi%d_%d=%.8f\n", m, i, Vr[n][p], m, i, Vi[n][p]);
        }
    printf("\tiNode=1 [out=true]\n");
    printf("\tpltVrA [out=true]; pltViA [out=true]\n");
    printf("\tpltVrB [out=true]; pltViB [out=true]\n");
    printf("\tpltVrC [out=true]; pltViC [out=true]\n");
    printf("\tminErr [out=true]; maxErr [out=true]; avgErr [out=true]; devErr [out=true]\n");

    printf("Distribs:\n\tgMeas [type=Gauss mean=0 dev=%.6f width=%.6f]\n", dev, width);
    printf("Stats:\n\terrStat\n");

    printf("PreProc:\n");
    for (int i = 1; i <= nStates; ++i)
        for (int m = 1; m <= kMeasPerComp; ++m)
            printf("\tzr%d_%d = eT_%d + rnd(gMeas)\n\tzi%d_%d = fT_%d + rnd(gMeas)\n", m, i, i, m, i, i);

    printf("WLSEs:\n");
    for (int i = 1; i <= nStates; ++i)
    {
        for (int m = 1; m <= kMeasPerComp; ++m)
            printf("\t[w=w_%d]\te_%d = zr%d_%d\n", i, i, m, i);
        for (int m = 1; m <= kMeasPerComp; ++m)
            printf("\t[w=w_%d]\tf_%d = zi%d_%d\n", i, i, m, i);
        printf("\t[w=w_%d]\te_%d + 0.001*f_%d = zr1_%d + 0.001*zi1_%d\n", i, i, i, i, i);
    }

    printf("PostProc:\n");
    for (int i = 1; i <= nStates; ++i)
        printf("\terrStat << abs(e_%d-eT_%d) << abs(f_%d-fT_%d)\n", i, i, i, i);
    printf("\tminErr = smin(errStat)\n\tmaxErr = smax(errStat)\n\tavgErr = avg(errStat)\n\tdevErr = dev(errStat)\n");

    int nPlotNodes = nodeCount;
    for (int node = 1; node <= nPlotNodes; ++node)
    {
        int iA = (node - 1) * 3 + 1, iB = iA + 1, iC = iA + 2;
        printf("\tif (iNode>=%d) and (iNode<%d):\n\t\tpltVrA=e_%d; pltViA=f_%d; pltVrB=e_%d; pltViB=f_%d; pltVrC=e_%d; pltViC=f_%d\n\tend\n",
               node, node + 1, iA, iA, iB, iB, iC, iC);
    }
    printf("Repeats:\n\tif iNode < %d:\n\t\tiNode += 1\n\t\trepeat\n\tend\nend\n", nPlotNodes);
    return 0;
}
