// Faithful port of the loadMatpowerBus3Ph state machine + extractNumericRow
// from ThreePhaseSEPluginMP.cpp, using only the C++ standard library (no
// natID SDK types), so it can be compiled and run directly against the real
// generated .m test files to confirm the parser logic is correct.
//
// Build & run:
//   g++ -std=c++17 -O2 verify_parser.cpp -o verify_parser.exe
//   ./verify_parser.exe path/to/case3_mini_3ph.m path/to/case9_3ph.m ...
// (with no arguments it looks for the 4 case*.m files in the current directory)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

constexpr int kBus3PhCols = 7; // canonical: busId, Vm_A, Va_A, Vm_B, Va_B, Vm_C, Va_C
constexpr int kBus3pCols = 9;  // standard Matpower: busid, type, basekV, Vm1..3, Va1..3

enum class Bus3pFormat { None, Custom7, Standard9 };

static std::string stripComment(const std::string& line)
{
    for (size_t i = 0; i < line.size(); ++i)
    {
        char ch = line[i];
        if (ch == '%' || ch == '#')
            return line.substr(0, i);
        if (ch == '/' && i + 1 < line.size() && line[i + 1] == '/')
            return line.substr(0, i);
    }
    return line;
}

static const char* findFirstNonWhiteSpace(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r')
        ++p;
    return *p ? p : nullptr;
}

static bool extractNumericRow(const std::string& row, int nExpectedCols,
                              std::vector<double>& flatValues, std::string& status)
{
    const char* p = row.c_str();
    std::vector<double> values(nExpectedCols);
    int count = 0;

    for (;;)
    {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')
            ++p;
        if (*p == 0)
            break;

        char* pEnd = nullptr;
        double v = std::strtod(p, &pEnd);
        if (pEnd == p)
            break;

        if (count < nExpectedCols)
            values[count] = v;
        ++count;
        p = pEnd;
    }

    if (count == 0)
        return true;
    if (count != nExpectedCols)
    {
        status = "ERROR! mpc.bus3ph row must have exactly 7 values. Got " + std::to_string(count) +
                 " in row: [" + row + "]";
        return false;
    }
    for (int i = 0; i < nExpectedCols; ++i)
        flatValues.push_back(values[i]);
    return true;
}

static std::string replaceAll(std::string s, char from, char to)
{
    for (auto& c : s)
        if (c == from)
            c = to;
    return s;
}

static std::vector<std::string> splitChar(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        out.push_back(item);
    return out;
}

static bool parseNumericRows(const std::string& body, int nExpectedCols,
                             std::vector<double>& flatValues, std::string& status)
{
    std::string cleaned = replaceAll(replaceAll(body, '[', ' '), ']', ' ');
    for (auto& row : splitChar(cleaned, ';'))
    {
        if (!extractNumericRow(row, nExpectedCols, flatValues, status))
            return false;
    }
    return true;
}

static int countChar(const std::string& s, char c)
{
    return int(std::count(s.begin(), s.end(), c));
}

static bool load(const std::string& fname, int& nodeCount, std::string& status,
                 std::vector<double>* canonicalOut = nullptr)
{
    std::ifstream f(fname);
    if (!f)
    {
        status = "cannot open file";
        return false;
    }

    std::vector<double> flatValues;
    Bus3pFormat format = Bus3pFormat::None;
    int srcCols = kBus3PhCols;
    enum class State { Searching, SkippingBlock, InBus3Ph, Done };
    State state = State::Searching;
    int bracketDepth = 0;

    std::string raw;
    while (state != State::Done && std::getline(f, raw))
    {
        std::string line = stripComment(raw);
        const char* pBuff = findFirstNonWhiteSpace(line.c_str());
        if (!pBuff)
            continue;
        std::string str(pBuff);

        if (state == State::Searching)
        {
            bool isCustom = str.find("bus3ph") != std::string::npos;
            bool isStandard = !isCustom && str.find("bus3p") != std::string::npos;
            if (countChar(str, '=') > 0 && (isCustom || isStandard))
            {
                format = isCustom ? Bus3pFormat::Custom7 : Bus3pFormat::Standard9;
                srcCols = isCustom ? kBus3PhCols : kBus3pCols;
                auto eq = splitChar(str, '=');
                if (eq.size() != 2)
                {
                    status = "Malformed three-phase bus block declaration";
                    return false;
                }
                std::string rhs = eq[1];
                int opens = countChar(rhs, '[');
                int closes = countChar(rhs, ']');
                bracketDepth = opens - closes;
                if (!parseNumericRows(rhs, srcCols, flatValues, status))
                    return false;
                state = (bracketDepth <= 0) ? State::Done : State::InBus3Ph;
            }
            else if (str.find('[') != std::string::npos)
            {
                int opens = countChar(str, '[');
                int closes = countChar(str, ']');
                bracketDepth = opens - closes;
                if (bracketDepth > 0)
                    state = State::SkippingBlock;
            }
        }
        else if (state == State::SkippingBlock)
        {
            int opens = countChar(str, '[');
            int closes = countChar(str, ']');
            bracketDepth += opens - closes;
            if (bracketDepth <= 0)
                state = State::Searching;
        }
        else if (state == State::InBus3Ph)
        {
            int opens = countChar(str, '[');
            int closes = countChar(str, ']');
            bracketDepth += opens - closes;
            if (!parseNumericRows(str, srcCols, flatValues, status))
                return false;
            if (bracketDepth <= 0)
                state = State::Done;
        }
    }

    if (format == Bus3pFormat::None || flatValues.empty())
    {
        status = "no bus3ph/bus3p rows found";
        return false;
    }
    if (flatValues.size() % srcCols != 0)
    {
        status = "row/column mismatch";
        return false;
    }
    nodeCount = int(flatValues.size() / srcCols);

    if (canonicalOut)
    {
        canonicalOut->clear();
        for (int r = 0; r < nodeCount; ++r)
        {
            const double* s = &flatValues[size_t(r) * srcCols];
            if (format == Bus3pFormat::Standard9)
            {
                canonicalOut->insert(canonicalOut->end(),
                    { s[0], s[3], s[6], s[4], s[7], s[5], s[8] });
            }
            else
            {
                canonicalOut->insert(canonicalOut->end(), s, s + kBus3PhCols);
            }
        }
    }
    return true;
}

int main(int argc, char** argv)
{
    std::vector<std::string> files;
    if (argc > 1)
    {
        for (int i = 1; i < argc; ++i)
            files.push_back(argv[i]);
    }
    else
    {
        files = {
            "case3_mini_3ph.m", "case9_3ph.m", "case14_3ph.m", "case_large_3ph.m"
        };
    }

    int failures = 0;
    for (const auto& fn : files)
    {
        int nodeCount = -1;
        std::string status;
        std::vector<double> canon;
        bool ok = load(fn, nodeCount, status, &canon);
        if (ok)
        {
            std::printf("OK   %-25s nodeCount=%d\n", fn.c_str(), nodeCount);
            // Print first node's canonical row + rectangular conversion as a sanity check.
            if (nodeCount > 0)
            {
                const double* n0 = &canon[0];
                std::printf("       node1 canonical: busId=%.0f  A(%.5f,%.4f)  B(%.5f,%.4f)  C(%.5f,%.4f)\n",
                            n0[0], n0[1], n0[2], n0[3], n0[4], n0[5], n0[6]);
                const double kPi = 3.14159265358979323846;
                for (int ph = 0; ph < 3; ++ph)
                {
                    double Vm = n0[1 + ph * 2];
                    double VaRad = n0[2 + ph * 2] * kPi / 180.0;
                    std::printf("         phase %c: Vr=%+.8f  Vi=%+.8f\n",
                                'A' + ph, Vm * std::cos(VaRad), Vm * std::sin(VaRad));
                }
            }
        }
        else
        {
            std::printf("FAIL %-25s %s\n", fn.c_str(), status.c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
