#include "ThreePhaseSEPluginMP.h"
#include "WindowPlugin.h"
#include <dense/Matrix.h>
#include <mu/ScopedCLocale.h>
#include <td/StringUtils.h>
#include <fo/FileOperations.h>
#include <arch/MemoryOut.h>
#include <cnt/PushBackVector.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

class Plugin : public sc::IPlugin
{
    MemoryArchiveContainer _outArchives;
    WindowPlugin* _pWnd = nullptr;

public:
    Plugin()
    {
        for (size_t i = 0; i < size_t(ArchType::NA); ++i)
            _outArchives[i] = nullptr;
    }

    void show(gui::Window* parentWnd, MemoryArchiveContainer& archives, td::UINT4 wndID, const sc::IPlugin::Cleaner& cleaner, const sc::IPlugin::CallBack& onComplete) override final
    {
        for (size_t i = 0; i < size_t(ArchType::NA); ++i)
            _outArchives[i] = archives[i];

        if (_pWnd)
            _pWnd->setFocus();
        else
        {
            _pWnd = new WindowPlugin(parentWnd, this, onComplete, cleaner, wndID);
            _pWnd->open();
        }
    }

    td::String getMenuName() const override final
    {
        return "3F SE Rect Converter (Matpower)";
    }

    arch::MemoryOut* getArchive(sc::IPlugin::ArchType type) override final
    {
        auto iType = size_t(type);
        if (iType >= getMaxSupportedArchiveParts())
            return nullptr;
        return _outArchives[iType];
    }

    MemoryArchiveContainer& getArchives() override final
    {
        return _outArchives;
    }

    td::String getOutFileName() const override final
    {
        assert(_pWnd);
        return _pWnd->getOutFileName();
    }

    size_t getMaxSupportedArchiveParts() const override final
    {
        return size_t(ArchType::NA);
    }

    ModelType getModelType() const override final
    {
        return ModelType::MLE;
    }

    void onClosedPluginWindow()
    {
        _pWnd = nullptr;
    }
};

static Plugin s_plugin;

void onClosedPluginWindow()
{
    s_plugin.onClosedPluginWindow();
}

extern "C"
{
PLUGIN_API sc::IPlugin* getPluginInterface()
{
    return &s_plugin;
}
}

// ---------------------------------------------------------------------------
// Matpower (.m) three-phase parser. Two input layouts are accepted:
//
// 1) Custom "bus3ph" layout (the test files shipped with this plugin):
//
//      mpc.bus3ph = [
//          busId  Vm_A  Va_A  Vm_B  Va_B  Vm_C  Va_C;   // 7 columns
//          ...
//      ];
//
// 2) Standard Matpower / MATPOWER 3-phase "bus3p" layout (as produced by real
//    cases such as t_case3p_a.m, converted from OpenDSS):
//
//      mpc.bus3p = [
//          busid  type  basekV  Vm1  Vm2  Vm3  Va1  Va2  Va3;   // 9 columns
//          ...
//      ];
//
//    Here the three magnitudes are grouped first (Vm1..Vm3), then the three
//    angles (Va1..Va3); phase 1/2/3 == A/B/C.
//
//   Vm_x = voltage magnitude of phase x [p.u.]
//   Va_x = voltage angle of phase x    [degrees, Matpower convention]
//
// Whichever block is found first is used. All other Matpower blocks (mpc.bus,
// mpc.branch, mpc.gen, mpc.line3p, mpc.load3p, ...) are valid and may be present
// (so the file still loads in real Matpower/Octave) but are simply skipped.
// Internally every row is normalised to the canonical 7-column layout
// [busId, Vm_A, Va_A, Vm_B, Va_B, Vm_C, Va_C] so the rest of the converter
// does not care which input format was used.
// ---------------------------------------------------------------------------

namespace
{
    constexpr td::UINT4 kNumPhases = 3;
    constexpr td::UINT4 kBus3PhCols = 1 + 2 * kNumPhases; // canonical: busId + (Vm,Va) per phase = 7
    constexpr td::UINT4 kBus3pCols = 9;                   // standard Matpower bus3p: busid,type,basekV,Vm1..3,Va1..3
    constexpr td::UINT4 kMaxSrcCols = kBus3pCols;         // widest row we ever parse
    constexpr double kPi = 3.14159265358979323846;

    // Which three-phase block layout was detected in the input file.
    enum class Bus3pFormat { None, Custom7, Standard9 };
}

struct InputModel
{
    // one row per three-phase node, columns: busId, Vm_A, Va_A, Vm_B, Va_B, Vm_C, Va_C
    dense::DblMatrix bus3ph;
    td::UINT4 nodeCount = 0;
};

// Strips a trailing '%' / '#' / "//" comment (whichever occurs first).
static td::String stripComment(const td::String& line)
{
    const char* pBuff = line.c_str();
    int cutAt = -1;
    int len = line.length();
    for (int i = 0; i < len; ++i)
    {
        char ch = pBuff[i];
        if (ch == '%' || ch == '#')
        {
            cutAt = i;
            break;
        }
        if (ch == '/' && i + 1 < len && pBuff[i + 1] == '/')
        {
            cutAt = i;
            break;
        }
    }
    if (cutAt < 0)
        return line;
    return line.left(cutAt);
}

// Extracts exactly nExpectedCols whitespace/comma-separated numbers from one
// row. Hand-rolled with strtod (instead of td::String::split) so that runs of
// multiple consecutive spaces/commas/tabs - used for column alignment in the
// generated test files - can never be miscounted as extra empty tokens.
static bool extractNumericRow(const td::String& row, td::UINT4 nExpectedCols,
                              cnt::PushBackVector<double>& flatValues,
                              td::String& status)
{
    const char* p = row.c_str();
    double values[kMaxSrcCols];
    td::UINT4 count = 0;

    for (;;)
    {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')
            ++p;
        if (*p == 0)
            break;

        char* pEnd = nullptr;
        double v = std::strtod(p, &pEnd);
        if (pEnd == p)
            break; // leftover non-numeric character: stop scanning this row

        if (count < nExpectedCols)
            values[count] = v;
        ++count;
        p = pEnd;
    }

    if (count == 0)
        return true; // blank/whitespace-only row (e.g. the closing '];' line)
    if (count != nExpectedCols)
    {
        if (nExpectedCols == kBus3pCols)
            status = "ERROR! mpc.bus3p row must have exactly 9 values: busid type basekV Vm1 Vm2 Vm3 Va1 Va2 Va3.";
        else
            status = "ERROR! mpc.bus3ph row must have exactly 7 values: busId Vm_A Va_A Vm_B Va_B Vm_C Va_C.";
        return false;
    }
    for (td::UINT4 i = 0; i < nExpectedCols; ++i)
        flatValues.push_back(values[i]);
    return true;
}

// Parses one or more ';'-separated numeric rows of exactly nExpectedCols values
// found in 'body' and appends them (row-major) to 'flatValues'.
static bool parseNumericRows(const td::String& body, td::UINT4 nExpectedCols,
                             cnt::PushBackVector<double>& flatValues,
                             cnt::PushBackVector<td::String>& rowTokens,
                             td::String& status)
{
    td::String cleaned = body.replace("[", " ");
    cleaned = cleaned.replace("]", " ");
    cleaned.split(";", rowTokens);

    for (td::UINT4 iRow = 0; iRow < td::UINT4(rowTokens.size()); ++iRow)
    {
        if (!extractNumericRow(rowTokens[iRow], nExpectedCols, flatValues, status))
            return false;
    }
    return true;
}

static bool loadMatpowerBus3Ph(const td::String& fileName, InputModel& model, td::String& status)
{
    fo::InFile inFile;
    if (!fo::openExistingBinaryFile(inFile, fileName))
    {
        status = "ERROR! Cannot open input file.";
        return false;
    }

    cnt::PushBackVector<double> flatValues;
    flatValues.reserve(kMaxSrcCols * 64);
    cnt::PushBackVector<td::String> rowTokens;
    rowTokens.reserve(64);

    // The number of columns per row depends on which block we lock onto: 7 for
    // the custom bus3ph layout, 9 for the standard Matpower bus3p layout.
    Bus3pFormat format = Bus3pFormat::None;
    td::UINT4 srcCols = kBus3PhCols;

    enum class State { Searching, SkippingBlock, InBus3Ph, Done };
    State state = State::Searching;
    int bracketDepth = 0;

    fo::LineNormal buffer;
    while (state != State::Done && fo::getLine(inFile, buffer))
    {
        td::String line = stripComment(td::String(buffer.c_str()));
        const char* pBuff = td::findFirstNonWhiteSpace(line.c_str());
        if (!pBuff || *pBuff == 0)
            continue;
        td::String str(pBuff);

        if (state == State::Searching)
        {
            // Looking for the three-phase bus block. "bus3ph" (custom, 7 cols) is
            // checked before "bus3p" (standard, 9 cols) because "bus3ph" also
            // contains the substring "bus3p".
            bool isCustom = strstr(str.c_str(), "bus3ph") != nullptr;
            bool isStandard = !isCustom && strstr(str.c_str(), "bus3p") != nullptr;
            if (str.countAppearance('=') > 0 && (isCustom || isStandard))
            {
                format = isCustom ? Bus3pFormat::Custom7 : Bus3pFormat::Standard9;
                srcCols = isCustom ? kBus3PhCols : kBus3pCols;

                cnt::PushBackVector<td::String> eqTokens;
                eqTokens.reserve(2);
                str.split('=', eqTokens);
                if (eqTokens.size() != 2)
                {
                    status = "ERROR! Malformed three-phase bus block declaration.";
                    return false;
                }
                td::String rhs = eqTokens[1];
                int opens = int(rhs.getCount('['));
                int closes = int(rhs.getCount(']'));
                bracketDepth = opens - closes;
                if (!parseNumericRows(rhs, srcCols, flatValues, rowTokens, status))
                    return false;
                state = (bracketDepth <= 0) ? State::Done : State::InBus3Ph;
            }
            else if (str.hasAny('['))
            {
                // Some other Matpower block (mpc.bus, mpc.gen, mpc.branch, ...): skip it.
                int opens = int(str.getCount('['));
                int closes = int(str.getCount(']'));
                bracketDepth = opens - closes;
                if (bracketDepth > 0)
                    state = State::SkippingBlock;
            }
            // scalar assignments (mpc.baseMVA = 100;), 'function ...', 'end', comments: ignored
        }
        else if (state == State::SkippingBlock)
        {
            int opens = int(str.getCount('['));
            int closes = int(str.getCount(']'));
            bracketDepth += opens - closes;
            if (bracketDepth <= 0)
                state = State::Searching;
        }
        else if (state == State::InBus3Ph)
        {
            int opens = int(str.getCount('['));
            int closes = int(str.getCount(']'));
            bracketDepth += opens - closes;
            if (!parseNumericRows(str, srcCols, flatValues, rowTokens, status))
                return false;
            if (bracketDepth <= 0)
                state = State::Done;
        }
    }

    if (format == Bus3pFormat::None || flatValues.size() == 0)
    {
        status = "ERROR! Could not find a non-empty 'mpc.bus3ph' or 'mpc.bus3p' block in the input file.";
        return false;
    }
    if (flatValues.size() % srcCols != 0)
    {
        status = "ERROR! Three-phase bus block row/column mismatch while parsing.";
        return false;
    }

    model.nodeCount = td::UINT4(flatValues.size() / srcCols);
    model.bus3ph.reserve(model.nodeCount, kBus3PhCols);
    auto mat = model.bus3ph.getManipulator();
    for (td::UINT4 iRow = 0; iRow < model.nodeCount; ++iRow)
    {
        const double* src = &flatValues[size_t(iRow) * srcCols];
        // Normalise every source layout to the canonical 7-column row:
        //   [busId, Vm_A, Va_A, Vm_B, Va_B, Vm_C, Va_C].
        if (format == Bus3pFormat::Standard9)
        {
            // src = [busid, type, basekV, Vm1, Vm2, Vm3, Va1, Va2, Va3]
            mat(iRow, 0) = src[0];           // busId
            mat(iRow, 1) = src[3];           // Vm_A = Vm1
            mat(iRow, 2) = src[6];           // Va_A = Va1
            mat(iRow, 3) = src[4];           // Vm_B = Vm2
            mat(iRow, 4) = src[7];           // Va_B = Va2
            mat(iRow, 5) = src[5];           // Vm_C = Vm3
            mat(iRow, 6) = src[8];           // Va_C = Va3
        }
        else
        {
            // src already in canonical order.
            for (td::UINT4 iCol = 0; iCol < kBus3PhCols; ++iCol)
                mat(iRow, iCol) = src[iCol];
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Polar -> rectangular conversion (the actual conversion task):
//   Vr_phase = Vm_phase * cos(Va_phase[rad])   (real / "in-phase" component)
//   Vi_phase = Vm_phase * sin(Va_phase[rad])   (imaginary / "Vm" component, per
//              the project's e/f rectangular-coordinate naming convention)
// performed once per phase, per node, for phases A, B, C.
// ---------------------------------------------------------------------------

// Output sink that appends straight to the .dmodl/.vmodl file. The generated
// model is written directly to the file rather than through the plugin's
// persistent MemoryOut archive: that archive is never cleared between runs, so
// converting more than once in a single dTwin session would concatenate several
// complete models into one file - an invalid .dmodl that dTwin refuses to open.
// (The archive is not needed for our flow anyway: the plugin never fires the
// onComplete callback, so dTwin only ever loads the file the user opens.)
// The put() overloads mirror arch::MemoryOut so the writer functions are
// unchanged apart from being templated on the sink type.
struct FileSink
{
    std::ofstream& f;
    void put(const char* p, size_t n) { f.write(p, std::streamsize(n)); }
    void put(const char* p) { f.write(p, std::streamsize(std::strlen(p))); }
};

template <class Sink>
static void writeHeader(Sink& out, const ConverterOptions& options, td::MutableString& str)
{
    td::String modelName = options.modelName;
    modelName.replace('\"', '\'');
    str.appendFormat("Header:\n\tmaxIter=%d\n\treport=AllDetails\nend\n", options.maxIter);
    str.appendFormat("Model [type=WLS domain=real eps=1e-5 name=\"%s\"]:\n", modelName.c_str());
    out.put(str.c_str(), str.length());
    str.reset();
}

// Flushes 'str' to 'out' every kFlushEvery iterations instead of letting it
// grow across the whole (potentially many-thousand-state) loop. Large models
// (e.g. the 1500-node test case) made this buffer balloon to several hundred
// KB in one piece, which is the likely cause of a crash during conversion -
// keeping it bounded removes that risk regardless of node count.
static constexpr td::UINT4 kFlushEvery = 200;

// Cap on how many nodes the visual model sweeps through (see the PostProc /
// Repeats block built in writeDigitalModel below). A chart with more points
// than this is unreadable anyway, and bounding it keeps the Repeats-driven
// re-solve loop cheap regardless of model size.
static constexpr td::UINT4 kMaxPlotNodes = 20;

// Emits the shared per-node plot machinery (PostProc node selection + Repeats
// sweep) used by both the clean and the noisy model. 'str' is expected empty on
// entry and is left flushed on return.
template <class Sink>
static void writePlotSweep(Sink& out, td::MutableString& str, td::UINT4 nPlotNodes)
{
    // PostProc: after each (re-)solve, copy the currently selected node's
    // Vr/Vi into the plt* params. Repeats: bump the node index and resolve
    // again, so the .vmodl can plot one point per node via @x << iNode.
    // Pattern verified against Digital Twin/res/files/34.dmodl + 38.dmodl.
    for (td::UINT4 node = 1; node <= nPlotNodes; ++node)
    {
        td::UINT4 iA = (node - 1) * kNumPhases + 1;
        td::UINT4 iB = iA + 1;
        td::UINT4 iC = iA + 2;
        str.appendFormat("\tif (iNode>=%u) and (iNode<%u):\n\t\tpltVrA=e_%u; pltViA=f_%u; pltVrB=e_%u; pltViB=f_%u; pltVrC=e_%u; pltViC=f_%u\n\tend\n",
                         node, node + 1, iA, iA, iB, iB, iC, iC);
        if (node % kFlushEvery == 0)
        {
            out.put(str.c_str(), str.length());
            str.reset();
        }
    }
    out.put(str.c_str(), str.length());
    str.reset();

    str.appendFormat("Repeats:\n\tif iNode < %u:\n\t\tiNode += 1\n\t\trepeat\n\tend\nend\n", nPlotNodes);
    out.put(str.c_str(), str.length());
    str.reset();
}

// Real state-estimation model: each voltage component gets kMeasPerComp
// INDEPENDENT noisy measurements, so the WLS system is genuinely overdetermined
// and the estimate is the (noise-averaged) best fit rather than a copy of one
// measurement. Estimation-error statistics are accumulated over the Repeats
// (Monte-Carlo) sweep. Block layout mirrors Digital Twin/res/files/38.dmodl.
template <class Sink>
static void writeDigitalModelNoisy(Sink& out,
                                   const InputModel& model,
                                   const dense::DblMatrix& Vr,
                                   const dense::DblMatrix& Vi,
                                   const ConverterOptions& options)
{
    constexpr td::UINT4 kMeasPerComp = 2; // independent noisy measurements per e_i and per f_i

    td::MutableString str;
    str.reserve(8192);
    writeHeader(out, options, str);

    td::UINT4 nStates = model.nodeCount * kNumPhases;
    td::UINT4 nPlotNodes = model.nodeCount < kMaxPlotNodes ? model.nodeCount : kMaxPlotNodes;
    double dev = options.noiseDev > 0.0f ? double(options.noiseDev) : 0.01;
    double width = dev * 5.0; // Gauss truncation width (cf. 38.dmodl: dev 0.03 / width 0.1)

    auto rVr = Vr.getManipulator1();
    auto rVi = Vi.getManipulator1();

    // States (initialised at the true converted values).
    out.put("Vars [out=true]:\n");
    for (td::UINT4 node = 1; node <= model.nodeCount; ++node)
    {
        for (td::UINT4 phase = 1; phase <= kNumPhases; ++phase)
        {
            td::UINT4 i = (node - 1) * kNumPhases + phase;
            str.appendFormat("\te_%u=%.8f\n\tf_%u=%.8f\n", i, rVr(node, phase), i, rVi(node, phase));
        }
        if (node % kFlushEvery == 0) { out.put(str.c_str(), str.length()); str.reset(); }
    }
    out.put(str.c_str(), str.length()); str.reset();

    // Params: the true value (eT/fT), the redundant measurement slots (filled in
    // PreProc), and the per-measurement weight.
    out.put("Params:\n");
    for (td::UINT4 node = 1; node <= model.nodeCount; ++node)
    {
        for (td::UINT4 phase = 1; phase <= kNumPhases; ++phase)
        {
            td::UINT4 i = (node - 1) * kNumPhases + phase;
            double vr = rVr(node, phase);
            double vi = rVi(node, phase);
            str.appendFormat("\teT_%u=%.8f\n\tfT_%u=%.8f\n\tw_%u=1.0\n", i, vr, i, vi, i);
            for (td::UINT4 m = 1; m <= kMeasPerComp; ++m)
                str.appendFormat("\tzr%u_%u=%.8f\n\tzi%u_%u=%.8f\n", m, i, vr, m, i, vi);
        }
        if (node % kFlushEvery == 0) { out.put(str.c_str(), str.length()); str.reset(); }
    }
    // Plot params + estimation-error output scalars.
    str.append("\tiNode=1 [out=true]\n");
    str.append("\tpltVrA [out=true]; pltViA [out=true]\n");
    str.append("\tpltVrB [out=true]; pltViB [out=true]\n");
    str.append("\tpltVrC [out=true]; pltViC [out=true]\n");
    str.append("\tminErr [out=true]; maxErr [out=true]; avgErr [out=true]; devErr [out=true]\n");
    out.put(str.c_str(), str.length()); str.reset();

    // Gauss measurement-noise distribution and the error accumulator.
    str.appendFormat("Distribs:\n\tgMeas [type=Gauss mean=0 dev=%.6f width=%.6f]\n", dev, width);
    str.append("Stats:\n\terrStat\n");
    out.put(str.c_str(), str.length()); str.reset();

    // PreProc: contaminate every measurement slot with an INDEPENDENT Gauss draw
    // around the true value (fresh each repeat -> Monte-Carlo over noise).
    out.put("PreProc:\n");
    for (td::UINT4 i = 1; i <= nStates; ++i)
    {
        for (td::UINT4 m = 1; m <= kMeasPerComp; ++m)
            str.appendFormat("\tzr%u_%u = eT_%u + rnd(gMeas)\n\tzi%u_%u = fT_%u + rnd(gMeas)\n", m, i, i, m, i, i);
        if (i % kFlushEvery == 0) { out.put(str.c_str(), str.length()); str.reset(); }
    }
    out.put(str.c_str(), str.length()); str.reset();

    // WLSEs: several independent measurements of the same state -> overdetermined
    // system, so the estimate is the weighted average and the noise is filtered.
    // The last (e + 0.001*f) equation is kept identical to the clean model on
    // purpose: it couples e_i and f_i so the per-node 2x2 block is never fully
    // decoupled (a fully block-diagonal Jacobian is what the solver chokes on).
    out.put("WLSEs:\n\t// Redundant noisy voltage measurements; the WLS estimate averages out the noise.\n");
    for (td::UINT4 i = 1; i <= nStates; ++i)
    {
        for (td::UINT4 m = 1; m <= kMeasPerComp; ++m)
            str.appendFormat("\t[w=w_%u]\te_%u = zr%u_%u\n", i, i, m, i);
        for (td::UINT4 m = 1; m <= kMeasPerComp; ++m)
            str.appendFormat("\t[w=w_%u]\tf_%u = zi%u_%u\n", i, i, m, i);
        str.appendFormat("\t[w=w_%u]\te_%u + 0.001*f_%u = zr1_%u + 0.001*zi1_%u\n", i, i, i, i, i);
        if (i % kFlushEvery == 0) { out.put(str.c_str(), str.length()); str.reset(); }
    }
    out.put(str.c_str(), str.length()); str.reset();

    // PostProc: accumulate |estimate - true| for every state into errStat, then
    // report the error statistics; finally select the plotted node's values.
    out.put("PostProc:\n");
    for (td::UINT4 i = 1; i <= nStates; ++i)
    {
        str.appendFormat("\terrStat << abs(e_%u-eT_%u) << abs(f_%u-fT_%u)\n", i, i, i, i);
        if (i % kFlushEvery == 0) { out.put(str.c_str(), str.length()); str.reset(); }
    }
    str.append("\tminErr = smin(errStat)\n\tmaxErr = smax(errStat)\n\tavgErr = avg(errStat)\n\tdevErr = dev(errStat)\n");
    out.put(str.c_str(), str.length()); str.reset();

    writePlotSweep(out, str, nPlotNodes);
}

template <class Sink>
static void writeDigitalModelClean(Sink& out,
                                   const InputModel& model,
                                   const dense::DblMatrix& Vr,
                                   const dense::DblMatrix& Vi,
                                   const ConverterOptions& options)
{
    td::MutableString str;
    str.reserve(8192);
    writeHeader(out, options, str);

    td::UINT4 nStates = model.nodeCount * kNumPhases;

    out.put("Vars [out=true]:\n");
    auto rVr = Vr.getManipulator1();
    auto rVi = Vi.getManipulator1();
    for (td::UINT4 node = 1; node <= model.nodeCount; ++node)
    {
        for (td::UINT4 phase = 1; phase <= kNumPhases; ++phase)
        {
            td::UINT4 i = (node - 1) * kNumPhases + phase;
            str.appendFormat("\te_%u=%.8f\n\tf_%u=%.8f\n", i, rVr(node, phase), i, rVi(node, phase));
        }
        if (node % kFlushEvery == 0)
        {
            out.put(str.c_str(), str.length());
            str.reset();
        }
    }
    out.put(str.c_str(), str.length());
    str.reset();

    // Measurements: each phase's converted Vr/Vi is fed back as a redundant
    // pseudo-measurement (weight = 1), so the WLS state estimate matches the
    // converted rectangular values and the system stays overdetermined.
    out.put("Params:\n");
    for (td::UINT4 node = 1; node <= model.nodeCount; ++node)
    {
        for (td::UINT4 phase = 1; phase <= kNumPhases; ++phase)
        {
            td::UINT4 i = (node - 1) * kNumPhases + phase;
            str.appendFormat("\tzr_%u=%.8f\n\tzi_%u=%.8f\n\tw_%u=1.0\n", i, rVr(node, phase), i, rVi(node, phase), i);
        }
        if (node % kFlushEvery == 0)
        {
            out.put(str.c_str(), str.length());
            str.reset();
        }
    }
    // Plot support: PostProc/Repeats below sweep through the first
    // nPlotNodes nodes one at a time, each repeat exposing that node's three
    // phase Vr/Vi pairs as plt* params for the .vmodl line plot to pick up.
    td::UINT4 nPlotNodes = model.nodeCount < kMaxPlotNodes ? model.nodeCount : kMaxPlotNodes;
    str.append("\tiNode=1 [out=true]\n");
    str.append("\tpltVrA [out=true]; pltViA [out=true]\n");
    str.append("\tpltVrB [out=true]; pltViB [out=true]\n");
    str.append("\tpltVrC [out=true]; pltViC [out=true]\n");
    out.put(str.c_str(), str.length());
    str.reset();

    out.put("WLSEs:\n\t// Rectangular-coordinate WLS equations fed by the polar -> rectangular conversion.\n");
    out.put("\t// Each phase has 3 redundant voltage-component measurements to keep the WLS system overdetermined.\n");
    for (td::UINT4 i = 1; i <= nStates; ++i)
    {
        str.appendFormat("\t[w=w_%u]\te_%u = zr_%u\n", i, i, i);
        str.appendFormat("\t[w=w_%u]\tf_%u = zi_%u\n", i, i, i);
        str.appendFormat("\t[w=w_%u]\te_%u + 0.001*f_%u = zr_%u + 0.001*zi_%u\n", i, i, i, i, i);
        if (i % kFlushEvery == 0)
        {
            out.put(str.c_str(), str.length());
            str.reset();
        }
    }
    out.put(str.c_str(), str.length());
    str.reset();

    // PostProc: after each (re-)solve, copy the currently selected node's
    // Vr/Vi into the plt* params. Repeats: bump the node index and resolve
    // again, so the .vmodl can plot one point per node via @x << iNode.
    out.put("PostProc:\n");
    writePlotSweep(out, str, nPlotNodes);
}

// Dispatches to the clean (exact-reproduction) or the noisy (real estimation)
// model generator depending on the user's option.
template <class Sink>
static void writeDigitalModel(Sink& out,
                              const InputModel& model,
                              const dense::DblMatrix& Vr,
                              const dense::DblMatrix& Vi,
                              const ConverterOptions& options)
{
    if (options.addNoise)
        writeDigitalModelNoisy(out, model, Vr, Vi, options);
    else
        writeDigitalModelClean(out, model, Vr, Vi, options);
}

// Earlier version plotted against a 'repNo' pseudo variable that does not
// exist anywhere in this project's real .vmodl files and crashed dTwin when
// opened. The model now drives its own small Repeats sweep (see PostProc /
// Repeats in writeDigitalModel above), so the plot below uses the same
// @x << <param>, @y << <param>, @cond -> solState# == @solOk# idiom found in
// Digital Twin/res/files/38.vmodl (paired with the same WLS domain=real
// model type as this converter), instead of any invented pseudo variable.
template <class Sink>
static void writePlotVisualModel(Sink& out)
{
    out.put("Header:\n\tnewTab = false\n\tdrawPlots = true\nend\n");
    out.put("Model [name=\"Three-phase SE results (rectangular)\"]:\n");
    out.put("Plots [backColor=auto]:\n");
    out.put("\tlinePlot [xLabel=\"Node index\" yLabel=\"Voltage components [p.u.]\" "
            "name=\"Per-node phase voltages (rectangular)\" anchor=TR legend=true nCols=2 anchorX=130 anchorY=35]:\n");
    out.put("\t\t@x << iNode\n");
    out.put("\t\t@y << pltVrA [width=2 colorL=black colorD=cyan name=\"Vr A\"]\n");
    out.put("\t\t@y << pltViA [width=2 colorL=darkGray colorD=lightBlue name=\"Vi A\"]\n");
    out.put("\t\t@y << pltVrB [width=2 colorL=darkGreen colorD=green name=\"Vr B\"]\n");
    out.put("\t\t@y << pltViB [width=2 colorL=darkBlue colorD=blue name=\"Vi B\"]\n");
    out.put("\t\t@y << pltVrC [width=2 colorL=darkRed colorD=red name=\"Vr C\"]\n");
    out.put("\t\t@y << pltViC [width=2 colorL=magenta colorD=magenta name=\"Vi C\"]\n");
    out.put("\t\t@cond -> solState# == @solOk#\n");
    out.put("\tend\n");
    out.put("end\n");
}

bool createModel(const td::String& inputFileName,
                 const td::String& outFileName,
                 sc::IPlugin* pIPlugin,
                 const ConverterOptions& options,
                 const ProgressCallback& onProgress,
                 td::String& status)
{
    mu::ScopedCLocale scopedLocale;

    InputModel model;
    if (!loadMatpowerBus3Ph(inputFileName, model, status))
        return false;

    dense::DblMatrix Vr;
    dense::DblMatrix Vi;
    Vr.reserve(model.nodeCount, kNumPhases);
    Vi.reserve(model.nodeCount, kNumPhases);

    td::UINT4 totalSteps = model.nodeCount * kNumPhases;

    // Thread-safe progress reporting: the worker thread only ever writes
    // 'processedSteps' (single writer) and the progress thread only reads it
    // (single reader), so std::atomic gives a data-race-free handoff without
    // a mutex. 'workerDone'/'workerOk' use the same single-writer pattern.
    std::atomic<td::UINT4> processedSteps { 0 };
    std::atomic_bool workerDone { false };
    std::atomic_bool workerOk { true };

    // Thread 2: real-time progress indicator (reports while thread 1 computes).
    std::thread progressThread([&]() {
        while (!workerDone.load())
        {
            double progress = totalSteps > 0 ? double(processedSteps.load()) / double(totalSteps) : 1.0;
            if (onProgress)
                onProgress(progress);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (onProgress)
            onProgress(workerOk.load() ? 1.0 : 0.0);
    });

    // Thread 1: polar -> rectangular conversion (the actual computation).
    std::thread workerThread([&]() {
        try
        {
            auto bus = model.bus3ph.getManipulator1();
            auto outVr = Vr.getManipulator1();
            auto outVi = Vi.getManipulator1();

            for (td::UINT4 node = 1; node <= model.nodeCount; ++node)
            {
                for (td::UINT4 phase = 0; phase < kNumPhases; ++phase)
                {
                    td::UINT4 colVm = 2 + phase * 2; // 1=busId, 2=Vm_A, 3=Va_A, 4=Vm_B, ...
                    td::UINT4 colVa = 3 + phase * 2;
                    double Vm = bus(node, colVm);
                    double VaDeg = bus(node, colVa);
                    double VaRad = VaDeg * kPi / 180.0;

                    outVr(node, phase + 1) = Vm * std::cos(VaRad);
                    outVi(node, phase + 1) = Vm * std::sin(VaRad);

                    processedSteps.fetch_add(1);
                }
            }
        }
        catch (...)
        {
            workerOk.store(false);
        }
        workerDone.store(true);
    });

    workerThread.join();
    progressThread.join();

    if (!workerOk.load())
    {
        status = "ERROR! Worker thread failed.";
        return false;
    }

    // Write the digital model straight to the output file. We deliberately do
    // NOT route this through pIPlugin->getArchive(): that MemoryOut archive is
    // owned by the plugin and never cleared, so a second conversion in the same
    // session would append a second model and produce a file with two Header/
    // Model blocks - which dTwin cannot open. Writing to a fresh ofstream each
    // time guarantees exactly one self-contained model per file.
    std::ofstream fDigital;
    if (!fo::createTextFile(fDigital, outFileName))
    {
        status = "ERROR! Cannot create output file.";
        return false;
    }
    FileSink digitalSink { fDigital };
    writeDigitalModel(digitalSink, model, Vr, Vi, options);
    fDigital.close();

    td::String visualFileName = fo::replaceFileExtension<false>(outFileName, ".vmodl");
    std::ofstream fVisual;
    if (!fo::createTextFile(fVisual, visualFileName))
    {
        status = "ERROR! Cannot create visual output file.";
        return false;
    }
    FileSink visualSink { fVisual };
    writePlotVisualModel(visualSink);
    fVisual.close();

    status = "INFO! Conversion completed.";
    return true;
}
