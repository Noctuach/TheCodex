// SineLab: sine-wave stress test of the Codex DSP modules (and the whole processor) against expected values that
// the test derives itself from the published formulas (RBJ cookbook, Butterworth, bilinear one-poles, the polyphase
// half-band recursions, the static curves, the compressor's static curve ...).
//
//   CodexSineLab <outdir> [--suites osc,os,dist,eq,sat,comp,clip,rev,proc,ext,den] [--threads N] [--quick]
//
// Every check has a fixed tolerance (set before the first run; see the comments at each check and the
// "tolerance notes" printed in the summary). One line per test group is printed: PASS / FAIL / INFO, the number of
// sine runs that fed the group, the number of checks, the worst |error|, its tolerance and the margin (tol / err),
// and the input of the worst case. Each suite writes <outdir>/<suite>.csv with one row per check:
//   module, check, rate, block, f, A, params, expected, measured, err, tol, pass
// plus <outdir>/summary.txt and <outdir>/os_coeffs.json (for tools/sinelab_analyse.py, which cross-checks a
// subset of the expected values in Python and draws the error plots).
//
// Modules are driven through their public APIs only, exactly as shipped (no test hooks in product code).
// Module tests run under juce::ScopedNoDenormals, as processBlock does; the denormal suite also runs without it.
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include "../Source/DSP/KickVoice.h"
#include "../Source/DSP/StereoOversampling.h"
#include "../Source/DSP/ClipDistortion.h"
#include "../Source/DSP/CurveSeed.h"
#include "../Source/DSP/EqEngine.h"
#include "../Source/DSP/MultibandSaturator.h"
#include "../Source/DSP/Compressor.h"
#include "../Source/DSP/OutputClipper.h"
#include "../Source/DSP/SpaceReverb.h"
#include "../Source/DSP/Chain.h"
#include <atomic>
#include <chrono>
#include <complex>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
 #include <time.h>
#endif

namespace sl
{
using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr int NA = 65536;   // coherent analysis length (rectangular window, odd bins)
const std::vector<double> kRates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
int gThreads = 4;
bool gQuick = false;

//==============================================================================
// Reporting
static std::string num (double v, int prec = 4)
{
    std::ostringstream o;
    o << std::setprecision (prec) << v;
    return o.str();
}

struct Ctx
{
    double rate = 0.0;
    int block = 0;
    double f = 0.0, A = 0.0;
    std::string params;
    long long sine = -1;
};

std::atomic<long long> gSineCounter { 0 };
long long newSine() { return gSineCounter.fetch_add (1); }

struct Group
{
    std::string suite, name, unit;
    int kind = 0;   // 0: |err| <= tol; 1: measured <= bound; 2: measured >= bound
    bool signedInfo = false;   // informational rows: track the largest signed value instead of the largest |value|
    std::mutex m;
    long checks = 0, fails = 0, infos = 0;
    std::unordered_set<long long> sines;
    double worstRatio = -1.0, worstErr = 0.0, worstTol = 0.0;
    std::string worstWhere;
    double infoWorst = -1.0, infoVal = 0.0;
    std::string infoWhere;
};

struct Suite
{
    std::string name;
    std::ofstream csv;
    std::mutex csvLock, groupLock;
    std::vector<std::unique_ptr<Group>> groups;
    double seconds = 0.0;
    bool ran = false;

    Group& g (const std::string& n, const std::string& unit = "")
    {
        const std::lock_guard<std::mutex> l (groupLock);
        for (auto& x : groups)
            if (x->name == n) return *x;
        groups.push_back (std::make_unique<Group>());
        groups.back()->suite = name;
        groups.back()->name = n;
        groups.back()->unit = unit;
        return *groups.back();
    }
};

std::map<std::string, std::unique_ptr<Suite>> gSuites;
std::vector<std::string> gSuiteOrder;
std::mutex gNotesLock;
std::vector<std::string> gNotes, gTolNotes, gNotDone;
juce::File gOut;

void note (const std::string& s)    { const std::lock_guard<std::mutex> l (gNotesLock); gNotes.push_back (s); }
void tolNote (const std::string& s) { const std::lock_guard<std::mutex> l (gNotesLock); gTolNotes.push_back (s); }
void notDone (const std::string& s) { const std::lock_guard<std::mutex> l (gNotesLock); gNotDone.push_back (s); }

Suite& S (const std::string& name)
{
    auto it = gSuites.find (name);
    if (it != gSuites.end()) return *it->second;
    auto s = std::make_unique<Suite>();
    s->name = name;
    s->csv.open (gOut.getChildFile (name + ".csv").getFullPathName().toStdString());
    s->csv << "module,check,rate,block,f,A,params,expected,measured,err,tol,pass\n";
    gSuiteOrder.push_back (name);
    return *(gSuites[name] = std::move (s));
}

static std::string where (const Ctx& c)
{
    std::ostringstream o;
    if (c.rate > 0) o << "fs=" << c.rate << " ";
    if (c.block != 0) o << "blk=" << c.block << " ";
    if (c.f != 0) o << "f=" << num (c.f, 7) << " ";
    if (c.A != 0) o << "A=" << num (c.A, 4) << " ";
    o << c.params;
    return o.str();
}

/** One check. tol < 0 (or NaN) = informational (reported, no pass/fail). err is compared as |err| <= tol. */
void rec (Group& g, const Ctx& c, double expected, double measured, double err, double tol)
{
    const bool info = ! (tol >= 0.0);
    const bool pass = info || (std::isfinite (err) && std::abs (err) <= tol);
    {
        const std::lock_guard<std::mutex> l (g.m);
        if (c.sine >= 0) g.sines.insert (c.sine);
        if (info)
        {
            ++g.infos;
            const double key = g.signedInfo ? err : std::abs (err);
            if (g.infos == 1 || key > g.infoWorst) { g.infoWorst = key; g.infoVal = err; g.infoWhere = where (c); }
        }
        else
        {
            ++g.checks;
            if (! pass) ++g.fails;
            const double ratio = std::isfinite (err) ? std::abs (err) / std::max (tol, 1e-300) : 1e300;
            if (ratio > g.worstRatio) { g.worstRatio = ratio; g.worstErr = err; g.worstTol = tol; g.worstWhere = where (c); }
        }
    }
    auto& s = S (g.suite);
    std::string p = c.params, gn = g.name;
    for (auto& ch : p) if (ch == ',') ch = ';';
    for (auto& ch : gn) if (ch == ',') ch = ';';
    std::ostringstream o;
    o << std::setprecision (12) << g.suite << "," << gn << "," << c.rate << "," << c.block << "," << c.f << "," << c.A << ","
      << p << "," << expected << "," << measured << "," << err << "," << (info ? std::string ("") : num (tol, 6)) << ","
      << (info ? "INFO" : (pass ? "PASS" : "FAIL")) << "\n";
    const std::lock_guard<std::mutex> l (s.csvLock);
    s.csv << o.str();
}

/** One-sided check: measured <= bound (upper) or measured >= bound (lower). */
void recBound (Group& g, const Ctx& c, double measured, double bound, bool upper)
{
    const double excess = upper ? measured - bound : bound - measured;
    const bool pass = std::isfinite (measured) && excess <= 0.0;
    {
        const std::lock_guard<std::mutex> l (g.m);
        g.kind = upper ? 1 : 2;
        if (c.sine >= 0) g.sines.insert (c.sine);
        ++g.checks;
        if (! pass) ++g.fails;
        const double e = std::isfinite (measured) ? excess : 1e300;
        if (g.worstRatio < 0 && g.checks == 1) g.worstRatio = -1e300;
        if (e > g.worstRatio) { g.worstRatio = e; g.worstErr = measured; g.worstTol = bound; g.worstWhere = where (c); }
    }
    auto& s = S (g.suite);
    std::string p = c.params, gn = g.name;
    for (auto& ch : p) if (ch == ',') ch = ';';
    for (auto& ch : gn) if (ch == ',') ch = ';';
    std::ostringstream o;
    o << std::setprecision (12) << g.suite << "," << gn << "," << c.rate << "," << c.block << "," << c.f << "," << c.A << ","
      << p << "," << bound << "," << measured << "," << excess << "," << (upper ? "<=" : ">=") << num (bound, 6) << ","
      << (pass ? "PASS" : "FAIL") << "\n";
    const std::lock_guard<std::mutex> l (s.csvLock);
    s.csv << o.str();
}

//==============================================================================
// Threads
template <typename Fn>
void parallelFor (int n, Fn&& fn, bool ftz = true)
{
    std::atomic<int> next { 0 };
    auto work = [&]
    {
        std::unique_ptr<juce::ScopedNoDenormals> nd;
        if (ftz) nd = std::make_unique<juce::ScopedNoDenormals>();
        for (;;)
        {
            const int i = next.fetch_add (1);
            if (i >= n) break;
            fn (i);
        }
    };
    const int nt = std::max (1, std::min (gThreads, n));
    std::vector<std::thread> th;
    for (int t = 0; t < nt; ++t) th.emplace_back (work);
    for (auto& t : th) t.join();
}

double threadCpu()
{
   #if JUCE_LINUX || JUCE_MAC || JUCE_BSD
    timespec ts {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double) ts.tv_sec + 1.0e-9 * (double) ts.tv_nsec;
   #else
    return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
   #endif
}

//==============================================================================
// Signals and block patterns
/** Coherent sine: x[n] = A sin (2 pi k (n0 + n) / N + phi0), phase computed from (k n) mod N, so it is exactly periodic. */
std::vector<float> coherentSine (long long k, int N, double A, double phi0, int len, long long n0 = 0)
{
    std::vector<float> x ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        const long long m = ((k % N) * ((n0 + i) % N)) % N;
        x[(size_t) i] = (float) (A * std::sin (kTwoPi * (double) m / (double) N + phi0));
    }
    return x;
}

/** Odd coherent bin closest to f (1 <= k < N/2). */
long long oddBin (double f, double fs, int N)
{
    long long k = (long long) std::llround (f * N / fs);
    if (k % 2 == 0) k += (f * N / fs >= (double) k) ? 1 : -1;
    k = std::max (1LL, std::min (k, (long long) N / 2 - 1));
    if (k % 2 == 0) --k;
    return k;
}

/** count log-spaced targets in [lo, hi], snapped to distinct odd bins. */
std::vector<long long> logBins (double lo, double hi, int count, double fs, int N)
{
    std::vector<long long> v;
    for (int i = 0; i < count; ++i)
    {
        const double f = lo * std::pow (hi / lo, count > 1 ? (double) i / (count - 1) : 0.0);
        const long long k = oddBin (f, fs, N);
        if (v.empty() || k > v.back()) v.push_back (k);
        else if (v.back() + 2 < N / 2) v.push_back (v.back() + 2);
    }
    return v;
}

/** 24 frequencies: 12 log-spaced over 10 Hz .. 0.45 fs and 12 dense around f0 (f0 / 2 .. 2 f0). */
std::vector<long long> denseBins (double f0, double fs, int N, int n = 24)
{
    std::set<long long> s;
    for (auto k : logBins (10.0, 0.45 * fs, n / 2, fs, N)) s.insert (k);
    const double lo = std::max (10.0, f0 / 2.0), hi = std::min (0.45 * fs, f0 * 2.0);
    for (auto k : logBins (lo, std::max (lo * 1.01, hi), n / 2, fs, N)) s.insert (k);
    return { s.begin(), s.end() };
}

const std::vector<int> kBlockGrid { 1, 2, 7, 16, 31, 64, 128, 255, 256, 257, 512, 1000, 2048, -1 };

/** Block size for run index i (cycles the grid; -1 = seeded random 1..maxBlock), never above maxBlock. */
int blockFor (int i, int maxBlock)
{
    std::vector<int> v;
    for (int b : kBlockGrid) if (b <= maxBlock) v.push_back (b);
    return v[(size_t) (i % (int) v.size())];
}

using ProcFn = std::function<void (float*, float*, int)>;

void runBlocks (const ProcFn& p, float* l, float* r, int len, int block, int maxBlock, int seed = 1)
{
    juce::Random rng (seed);
    for (int pos = 0; pos < len;)
    {
        int n = block > 0 ? block : 1 + rng.nextInt (maxBlock);
        n = std::min ({ n, maxBlock, len - pos });
        p (l + pos, r + pos, n);
        pos += n;
    }
}

//==============================================================================
// Analysis
struct Twiddle
{
    std::vector<double> c, s;
    Twiddle() : c ((size_t) NA), s ((size_t) NA)
    {
        for (int i = 0; i < NA; ++i) { c[(size_t) i] = std::cos (kTwoPi * i / NA); s[(size_t) i] = std::sin (kTwoPi * i / NA); }
    }
};
const Twiddle& tw() { static Twiddle t; return t; }

/** X[k] of N samples (N a power of two <= NA), double accumulation. */
cd bin (const float* x, int N, long long k)
{
    const auto& t = tw();
    const long long step = NA / N;
    k %= N;
    if (k < 0) k += N;
    long long idx = 0;
    double re = 0.0, im = 0.0;
    for (int n = 0; n < N; ++n)
    {
        const size_t j = (size_t) (idx * step);
        re += x[n] * t.c[j];
        im -= x[n] * t.s[j];
        idx += k;
        if (idx >= N) idx -= N;
    }
    return { re, im };
}

void fft (std::vector<cd>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = kTwoPi / (double) len * (inverse ? 1.0 : -1.0);
        for (size_t i = 0; i < n; i += len)
            for (size_t j = 0; j < len / 2; ++j)
            {
                const cd w = std::polar (1.0, ang * (double) j);
                const cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
            }
    }
    if (inverse) for (auto& v : a) v /= (double) n;
}

/** Complex amplitude of the component at w (rad/sample), 7-term Blackman-Harris window (-180 dB side lobes):
    for x = A cos (w n + phi) it returns A e^{j phi} (phase referred to n = 0 of this segment). */
cd toneBH (const float* x, int len, double w)
{
    static const double a[7] = { 0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803,
                                 0.01081174209837, 0.00077658482522, 0.00001388721735 };
    double sw = 0.0;
    cd acc = 0.0;
    const cd rot = std::polar (1.0, -w);
    const double dt = kTwoPi / (len - 1);
    const cd wrot = std::polar (1.0, dt);
    cd p = 1.0, q = 1.0;
    for (int n = 0; n < len; ++n)
    {
        if ((n & 1023) == 0) { p = std::polar (1.0, -w * n); q = std::polar (1.0, dt * n); }
        // cos (k t) by the Chebyshev recurrence from cos t = Re q
        const double c1 = q.real();
        double cm2 = 1.0, cm1 = c1, win = a[0] - a[1] * c1;
        for (int k = 2; k < 7; ++k)
        {
            const double ck = 2.0 * c1 * cm1 - cm2;
            win += ((k & 1) ? -a[k] : a[k]) * ck;
            cm2 = cm1; cm1 = ck;
        }
        sw += win;
        acc += win * (double) x[n] * p;
        p *= rot;
        q *= wrot;
    }
    return 2.0 * acc / sw;
}

double dB (double v) { return 20.0 * std::log10 (std::max (std::abs (v), 1e-20)); }   // floor -400 dB
double dBc (cd v) { return dB (std::abs (v)); }
double wrapDeg (double d) { while (d > 180.0) d -= 360.0; while (d <= -180.0) d += 360.0; return d; }
double phaseDeg (cd v) { return std::arg (v) * 180.0 / kPi; }
double ulpOf (double v)
{
    const float f = (float) std::max (std::abs (v), 1.0e-30);
    return (double) std::nextafter (f, std::numeric_limits<float>::infinity()) - (double) f;
}
bool allFinite (const std::vector<float>& v) { for (float x : v) if (! std::isfinite (x)) return false; return true; }

//==============================================================================
// Analytic models (written from the published formulas; independent of the module code)
struct Biq
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    cd H (double w) const { const cd z1 = std::polar (1.0, -w), z2 = z1 * z1; return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2); }
    double poleRadius() const
    {
        const cd d = std::sqrt (cd (a1 * a1 - 4.0 * a2, 0.0));
        return std::max (std::abs ((-a1 + d) / 2.0), std::abs ((-a1 - d) / 2.0));
    }
};
using Casc = std::vector<Biq>;
cd Hc (const Casc& c, double w) { cd h = 1.0; for (auto& b : c) h *= b.H (w); return h; }

/** Settle time (samples): >= 20 time constants of the slowest pole, and >= 0.25 s. */
int settleFor (const Casc& c, double fs)
{
    double tau = 0.0;
    for (auto& b : c)
    {
        const double r = b.poleRadius();
        if (r > 0.0 && r < 1.0) tau = std::max (tau, -1.0 / std::log (r));
    }
    return (int) std::max (20.0 * tau, 0.25 * fs);
}

namespace ref
{
    Biq norm (double b0, double b1, double b2, double a0, double a1, double a2) { return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 }; }
    double fcl (double f, double fs) { return std::min (std::max (f, 5.0), 0.49 * fs); }
    double qcl (double q) { return std::max (0.05, q); }

    // bilinear first order (K = tan (pi fc / fs))
    Biq lp1 (double fc, double fs) { const double K = std::tan (kPi * fcl (fc, fs) / fs); return { K / (1 + K), K / (1 + K), 0, (K - 1) / (K + 1), 0 }; }
    Biq hp1 (double fc, double fs) { const double K = std::tan (kPi * fcl (fc, fs) / fs); return { 1 / (1 + K), -1 / (1 + K), 0, (K - 1) / (K + 1), 0 }; }
    // RBJ cookbook
    struct W { double cw, sw; };
    W w0 (double f, double fs) { const double w = kTwoPi * fcl (f, fs) / fs; return { std::cos (w), std::sin (w) }; }
    Biq lp2 (double f, double q, double fs) { auto w = w0 (f, fs); const double al = w.sw / (2 * qcl (q)); return norm ((1 - w.cw) / 2, 1 - w.cw, (1 - w.cw) / 2, 1 + al, -2 * w.cw, 1 - al); }
    Biq hp2 (double f, double q, double fs) { auto w = w0 (f, fs); const double al = w.sw / (2 * qcl (q)); return norm ((1 + w.cw) / 2, -(1 + w.cw), (1 + w.cw) / 2, 1 + al, -2 * w.cw, 1 - al); }
    Biq bell (double f, double g, double q, double fs)
    {
        auto w = w0 (f, fs); const double A = std::pow (10.0, g / 40.0), al = w.sw / (2 * qcl (q));
        return norm (1 + al * A, -2 * w.cw, 1 - al * A, 1 + al / A, -2 * w.cw, 1 - al / A);
    }
    Biq notch (double f, double q, double fs) { auto w = w0 (f, fs); const double al = w.sw / (2 * qcl (q)); return norm (1, -2 * w.cw, 1, 1 + al, -2 * w.cw, 1 - al); }
    Biq bp0 (double f, double q, double fs) { auto w = w0 (f, fs); const double al = w.sw / (2 * qcl (q)); return norm (al, 0, -al, 1 + al, -2 * w.cw, 1 - al); }
    Biq ap2 (double f, double q, double fs) { auto w = w0 (f, fs); const double al = w.sw / (2 * qcl (q)); return norm (1 - al, -2 * w.cw, 1 + al, 1 + al, -2 * w.cw, 1 - al); }
    Biq lowShelf (double f, double g, double q, double fs)
    {
        auto w = w0 (f, fs); const double A = std::pow (10.0, g / 40.0), b = 2 * std::sqrt (A) * w.sw / (2 * qcl (q));
        return norm (A * ((A + 1) - (A - 1) * w.cw + b), 2 * A * ((A - 1) - (A + 1) * w.cw), A * ((A + 1) - (A - 1) * w.cw - b),
                     (A + 1) + (A - 1) * w.cw + b, -2 * ((A - 1) + (A + 1) * w.cw), (A + 1) + (A - 1) * w.cw - b);
    }
    Biq highShelf (double f, double g, double q, double fs)
    {
        auto w = w0 (f, fs); const double A = std::pow (10.0, g / 40.0), b = 2 * std::sqrt (A) * w.sw / (2 * qcl (q));
        return norm (A * ((A + 1) + (A - 1) * w.cw + b), -2 * A * ((A - 1) + (A + 1) * w.cw), A * ((A + 1) + (A - 1) * w.cw - b),
                     (A + 1) - (A - 1) * w.cw + b, 2 * ((A - 1) - (A + 1) * w.cw), (A + 1) - (A - 1) * w.cw - b);
    }
    /** Butterworth of any order: pole-pair Qs 1 / (2 cos ((2k+1) pi / 2N)) (even) or a first-order section plus
        1 / (2 cos (k pi / N)) (odd); order 2 uses the user's Q (as specified for the EQ). */
    void butter (Casc& c, bool hp, int order, double fc, double userQ, double fs)
    {
        if (order <= 1) { c.push_back (hp ? hp1 (fc, fs) : lp1 (fc, fs)); return; }
        if (order == 2) { c.push_back (hp ? hp2 (fc, userQ, fs) : lp2 (fc, userQ, fs)); return; }
        if (order % 2 == 1)
        {
            c.push_back (hp ? hp1 (fc, fs) : lp1 (fc, fs));
            for (int k = 1; k <= (order - 1) / 2; ++k)
            {
                const double q = 1.0 / (2.0 * std::cos (k * kPi / order));
                c.push_back (hp ? hp2 (fc, q, fs) : lp2 (fc, q, fs));
            }
        }
        else
            for (int k = 0; k < order / 2; ++k)
            {
                const double q = 1.0 / (2.0 * std::cos ((2 * k + 1) * kPi / (2.0 * order)));
                c.push_back (hp ? hp2 (fc, q, fs) : lp2 (fc, q, fs));
            }
    }

    const int slopeDb[] = { 6, 12, 18, 24, 36, 48, 72, 96 };

    /** The EQ band, from the plan's formulas. */
    Casc design (const eq::BandSettings& s, double fs)
    {
        Casc c;
        if (! s.on) return c;
        const double f = fcl ((double) s.freq, fs), Q = qcl ((double) s.q), G = (double) s.gainDb;
        const int order = slopeDb[std::clamp (s.slope, 0, 7)] / 6;
        switch (s.type)
        {
            case eq::BandType::LowCut:    butter (c, true, order, f, Q, fs); break;
            case eq::BandType::HighCut:   butter (c, false, order, f, Q, fs); break;
            case eq::BandType::Bell:      c.push_back (bell (f, G, Q, fs)); break;
            case eq::BandType::Notch:     c.push_back (notch (f, Q, fs)); break;
            case eq::BandType::LowShelf:  c.push_back (lowShelf (f, G, Q, fs)); break;
            case eq::BandType::HighShelf: c.push_back (highShelf (f, G, Q, fs)); break;
            case eq::BandType::BandPass:
                if (order <= 2) c.push_back (bp0 (f, Q, fs));
                else
                {
                    const double bw = std::pow (2.0, 1.0 / (2.0 * Q));
                    const int side = std::min (8, (order + 1) / 2);
                    butter (c, true, side, f / bw, 0.70710678, fs);
                    butter (c, false, side, f * bw, 0.70710678, fs);
                    const double m = std::abs (Hc (c, kTwoPi * f / fs));
                    if (m > 1e-6) { c[0].b0 /= m; c[0].b1 /= m; c[0].b2 /= m; }
                }
                break;
            case eq::BandType::TiltShelf:
                c.push_back (lowShelf (f, -0.5 * G, Q * 0.7, fs));
                c.push_back (highShelf (f, 0.5 * G, Q * 0.7, fs));
                break;
            case eq::BandType::FlatTilt:
                for (double m : { 1.0 / 16.0, 1.0, 16.0 })
                {
                    c.push_back (lowShelf (f * m, -G / 6.0, 0.5, fs));
                    c.push_back (highShelf (f * m, G / 6.0, 0.5, fs));
                }
                break;
            case eq::BandType::AllPass: c.push_back (ap2 (f, Q, fs)); break;
        }
        return c;
    }
}

//==============================================================================
// Polyphase half-band oversampler model: each section y = a x + s, s = x - a y -> (a + z^-1) / (1 + a z^-1) at the
// stage's low rate; up: H_up(z) = D(z^2) + z^-1 E(z^2); down: H_dn(z) = (D(z^2) + z^-1 E(z^2)) / 2.
struct OsStage { std::vector<double> upD, upE, dnD, dnE; };

std::vector<OsStage> makeOsStages (int numStages)
{
    using FD = juce::dsp::FilterDesign<float>;
    std::vector<OsStage> v;
    for (int n = 0; n < numStages; ++n)
    {
        const float twUp = 0.10f * (n == 0 ? 0.5f : 1.0f), twDown = 0.12f * (n == 0 ? 0.5f : 1.0f);
        auto su = FD::designIIRLowpassHalfBandPolyphaseAllpassMethod (twUp, -90.0f + 10.0f * (float) n);
        auto sd = FD::designIIRLowpassHalfBandPolyphaseAllpassMethod (twDown, -75.0f + 10.0f * (float) n);
        OsStage s;
        for (int i = 0; i < su.directPath.size(); ++i)  s.upD.push_back (su.directPath.getObjectPointer (i)->coefficients[0]);
        for (int i = 1; i < su.delayedPath.size(); ++i) s.upE.push_back (su.delayedPath.getObjectPointer (i)->coefficients[0]);
        for (int i = 0; i < sd.directPath.size(); ++i)  s.dnD.push_back (sd.directPath.getObjectPointer (i)->coefficients[0]);
        for (int i = 1; i < sd.delayedPath.size(); ++i) s.dnE.push_back (sd.delayedPath.getObjectPointer (i)->coefficients[0]);
        v.push_back (s);
    }
    return v;
}

std::vector<std::vector<OsStage>> gOs;   // [numStages] (index 0 unused), built in main before any thread starts
const std::vector<OsStage>& osStages (int numStages) { return gOs[(size_t) numStages]; }

cd apc (const std::vector<double>& a, cd zi) { cd p = 1.0; for (double al : a) p *= (al + zi) / (1.0 + al * zi); return p; }
cd hUp (const OsStage& s, double th) { const cd z1 = std::polar (1.0, -th), z2 = z1 * z1; return apc (s.upD, z2) + z1 * apc (s.upE, z2); }
cd hDn (const OsStage& s, double th) { const cd z1 = std::polar (1.0, -th), z2 = z1 * z1; return 0.5 * (apc (s.dnD, z2) + z1 * apc (s.dnE, z2)); }

using RespFn = std::function<cd (double)>;

/** Base-rate response of up -> inner (at the highest rate) -> down: G(w) = 1/2 sum_k H_up(t_k) G_next(t_k) H_dn(t_k),
    t_k = (w + 2 pi k) / 2, nested over the stages. With inner = 1 this is the oversampler's own G0. */
cd osG (const std::vector<OsStage>& st, double w, const RespFn& inner, size_t level = 0)
{
    if (level >= st.size()) return inner ? inner (w) : cd (1.0);
    cd sum = 0.0;
    for (int k = 0; k < 2; ++k)
    {
        const double th = (w + kTwoPi * k) / 2.0;
        sum += hUp (st[level], th) * osG (st, th, inner, level + 1) * hDn (st[level], th);
    }
    return 0.5 * sum;
}

/** Group delay (samples) of a response at w -> 0. */
double groupDelayAt0 (const std::function<cd (double)>& H)
{
    const double w1 = 1e-4, w2 = 2e-4;
    double d = std::arg (H (w2)) - std::arg (H (w1));
    while (d > kPi) d -= kTwoPi;
    while (d < -kPi) d += kTwoPi;
    return -d / (w2 - w1);
}

// Small building blocks of the distortion / saturator / compressor paths
cd zinv (double w) { return std::polar (1.0, -w); }
cd adaaA (double w) { return 0.5 * (1.0 + zinv (w)); }                                     // first-order ADAA on a linear curve
cd thiran (double w, double a) { const cd z = zinv (w); return (a + z) / (1.0 + a * z); }  // first-order all-pass
cd dcHP (double w, double R) { const cd z = zinv (w); return (1.0 - z) / (1.0 - R * z); }
float dcRfor (double fs) { return (float) std::exp (-kTwoPi * 10.0 / fs); }
/** TPT one-pole low-pass with coefficient G = g / (1 + g): H = G (1 + z^-1) / (1 - (1 - 2G) z^-1). */
cd tptLP (double w, double G) { const cd z = zinv (w); return G * (1.0 + z) / (1.0 - (1.0 - 2.0 * G) * z); }
float tptG (double rate, double hz)
{
    const double t = std::tan (kPi * std::min (std::max (hz, 5.0), rate * 0.49) / rate);
    return (float) (t / (1.0 + t));
}

//==============================================================================
// Expected static curves (double precision, from the documented formulas)
double myShape (int m, double x)
{
    auto clamp1 = [] (double v) { return std::min (1.0, std::max (-1.0, v)); };
    switch (m)
    {
        case ClipDistortion::HardClip:  return clamp1 (x);
        case ClipDistortion::SoftClip:  return x >= 1 ? 1.0 : (x <= -1 ? -1.0 : 1.5 * (x - x * x * x / 3.0));
        case ClipDistortion::Tanh:      return std::tanh (x);
        case ClipDistortion::Overdrive: return 2.0 / kPi * std::atan (1.5 * x);
        case ClipDistortion::Tube:      return x >= 0 ? std::tanh (x) : 0.75 * std::tanh (1.6 * x);
        // expm1 / exact period reduction: no cancellation near 0 in the reference itself
        case ClipDistortion::Diode:     return x >= 0 ? -std::expm1 (-1.5 * x) : 0.6 * std::expm1 (1.2 * x);
        case ClipDistortion::Fuzz:      return (x >= 0 ? 1.0 : -1.0) * -std::expm1 (-4.0 * std::abs (x));
        case ClipDistortion::SineFold:  return std::sin (kPi * x / 2.0);
        case ClipDistortion::TriFold:   { const double u = x - 4.0 * std::floor ((x + 1.0) / 4.0); return u <= 1.0 ? u : 2.0 - u; }
        case ClipDistortion::Rectify:   return std::tanh (std::abs (x));
        case ClipDistortion::Bitcrush:  return std::round (8.0 * clamp1 (x)) / 8.0;
        case ClipDistortion::AsymClip:  return std::min (1.0, std::max (-0.55, x));
        case ClipDistortion::Diode2:    return x / std::pow (1.0 + std::pow (std::abs (x), 2.5), 0.4);
        case ClipDistortion::ZeroSquare:{ const double c = clamp1 (x); return c * std::abs (c); }
        case ClipDistortion::Downsample: return clamp1 (x);
        case ClipDistortion::XShaper:
        case ClipDistortion::XShaperAsym: return clamp1 (x);   // default (identity) points: Catmull-Rom reproduces y = x, flat beyond 1
        default: return x;
    }
}
double myHarden (double y, double chr) { if (chr <= 0) return y; return std::min (1.0, std::max (-1.0, y * (1.0 + 40.0 * chr * chr))); }
double myTransfer (int mode, double x, double g, double off, double chr)
{
    if (mode == ClipDistortion::Downsample) g = 1.0;
    return myHarden (myShape (mode, x * g + off) - myShape (mode, off), chr);
}

/** Error bound of the float static curve (dist 3a1 tolerance note): 4 * (ulp(y) + ulp(f(v)) + ulp(f(off)) + |f'(v)| * delta)
    * (1 + 40 chr^2), delta = ulp(x g) + ulp(x g + off) (argument rounding of the float product and sum). */
double staticTol (int mode, double x, double g, double off, double chr)
{
    if (mode == ClipDistortion::Downsample) g = 1.0;
    const double v = x * g + off, yD = myTransfer (mode, x, g, off, chr);
    const double delta = ulpOf (x * g) + ulpOf (v);
    const double h = 1e-6 * std::max (1.0, std::abs (v));
    const double fp = std::min (1e3, std::abs (myShape (mode, v + h) - myShape (mode, v - h)) / (2 * h));
    return 4.0 * (ulpOf (yD) + ulpOf (myShape (mode, v)) + ulpOf (myShape (mode, off)) + fp * delta) * (1.0 + 40.0 * chr * chr);
}

/** Fourier sine/cosine amplitude of harmonic m of g(theta) = fn (A sin theta) (2^16-point FFT, double). */
std::vector<double> harmonicAmps (const std::function<double (double)>& fn, double A, int maxM, int L = 65536)
{
    std::vector<cd> a ((size_t) L);
    for (int i = 0; i < L; ++i) a[(size_t) i] = fn (A * std::sin (kTwoPi * i / L));
    fft (a, false);
    std::vector<double> c ((size_t) maxM + 1);
    for (int m = 0; m <= maxM; ++m) c[(size_t) m] = (m == 0 ? 1.0 : 2.0) * std::abs (a[(size_t) m]) / L;
    return c;
}

//==============================================================================
// 1. KickVoice oscillator
struct VoiceOut { std::vector<float> out, sub, hit; };

KickVoice::Settings synthSettings (int wave, float tune, float phaseDeg)
{
    KickVoice::Settings s;
    s.engine = KickVoice::SynthEngine;
    s.wave = wave;
    s.pitchAmountSemis = 0.0f;
    s.clickLevel = 0.0f;
    s.tuneSemis = tune;
    s.startPhaseDeg = phaseDeg;
    return s;
}

VoiceOut renderVoice (double fs, const KickVoice::Settings& s, float pitchNote, int len, int block = 512, bool wantSub = false)
{
    KickVoice v;
    v.prepare (fs);
    v.setSettings (s);   // after prepare: setSettings reads the sample rate
    v.pitchEnv.setParameters (0.0f, 1.0f, 0.0f, 1.0f, 1.0f);
    v.ampEnv.setParameters (0.0f, 1.0f, 1.0f, 1000.0f, 1.0f);
    v.velocityGain = 1.0f;
    v.hitVelocity = 1.0f;
    v.seedNoise (1);
    VoiceOut o;
    o.out.assign ((size_t) len, 0.0f);
    o.sub.assign ((size_t) len, 0.0f);
    o.hit.assign ((size_t) len, 0.0f);
    v.noteOn (60, pitchNote);
    juce::Random rng (7);
    for (int pos = 0; pos < len;)
    {
        const int n = std::min (block > 0 ? block : 1 + rng.nextInt (1024), len - pos);
        v.subOut = wantSub ? o.sub.data() + pos : nullptr;
        v.hitTimeOut = o.hit.data() + pos;
        v.render (o.out.data() + pos, n);
        pos += n;
    }
    return o;
}

/** The phase sequence the documented formula gives: f = min (440 * 2^((note - 69) / 12) * 2^(tune / 12), 0.45 fs),
    accumulated in double with a wrap, starting at phi / 360 (value before each sample's advance). */
std::vector<double> idealPhase (double fs, float pitchNote, float tune, float phaseDeg, int len)
{
    const double base = 440.0 * std::pow (2.0, ((double) pitchNote - 69.0) / 12.0);
    const double f = std::min (base * std::exp2 ((double) tune / 12.0), 0.45 * fs);
    std::vector<double> ph ((size_t) len);
    double p = (double) phaseDeg / 360.0;
    for (int n = 0; n < len; ++n) { ph[(size_t) n] = p; p += f / fs; p -= std::floor (p); }
    return ph;
}

double expectedVoiceFreq (double fs, float pitchNote, float tune)
{
    return std::min (440.0 * std::pow (2.0, ((double) pitchNote + (double) tune - 69.0) / 12.0), 0.45 * fs);
}

/** Frequency from phase regression: complex amplitudes of 4 equal segments at the expected frequency (BH7 window),
    the phase residual against the expected frequency is fitted with a line; returns f_meas / f_exp - 1 and the
    mean amplitude. Segments must hold >= 8 cycles (side lobes of the negative-frequency image < -180 dB). */
struct FreqFit { double rel = 0.0, amp = 0.0; };
FreqFit fitFreq (const float* x, int len, double fs, double fExp, int segs = 4)
{
    const int L = len / segs;
    const double w = kTwoPi * fExp / fs;
    std::vector<double> t, r;
    double amp = 0.0, prev = 0.0;
    for (int s = 0; s < segs; ++s)
    {
        const cd c = toneBH (x + (size_t) s * L, L, w);
        amp += std::abs (c) / segs;
        const double n0 = (double) s * L;
        double res = std::arg (c) - std::fmod (w * n0, kTwoPi);
        if (s > 0) { while (res - prev > kPi) res -= kTwoPi; while (res - prev < -kPi) res += kTwoPi; }
        prev = res;
        t.push_back (n0);
        r.push_back (res);
    }
    double mt = 0, mr = 0;
    for (int s = 0; s < segs; ++s) { mt += t[(size_t) s] / segs; mr += r[(size_t) s] / segs; }
    double num_ = 0, den = 0;
    for (int s = 0; s < segs; ++s) { num_ += (t[(size_t) s] - mt) * (r[(size_t) s] - mr); den += (t[(size_t) s] - mt) * (t[(size_t) s] - mt); }
    return { (num_ / den) / w, amp };
}

int segLenFor (double f, double fs) { return (int) (std::max (0.25, 8.5 / f) * fs); }

void suiteOsc()
{
    auto& su = S ("osc");
    auto& gFreq = su.g ("sine.pitch.freq_rel", "rel");
    auto& gAmp = su.g ("sine.pitch.amp", "dB");
    auto& gThd = su.g ("sine.pitch.thdn", "dBc");
    auto& gMir = su.g ("sine.mirror_sinf", "abs");
    auto& gTune = su.g ("tune_and_clamp.freq_rel", "rel");
    auto& gTri = su.g ("triangle.per_sample", "abs");
    auto& gWarm = su.g ("warmsine.harmonics", "dB");
    auto& g909 = su.g ("909.tail_freq_rel", "rel");
    auto& gSubA = su.g ("sub.per_sample(float clock)", "abs");
    auto& gSubB = su.g ("sub.per_sample(ideal clock)", "abs");
    auto& gSp0 = su.g ("startphase.vs_sinf", "abs");
    auto& gSp1 = su.g ("startphase.vs_double", "abs");
    auto& gHit = su.g ("hitclock.drift_ms", "ms");

    tolNote ("osc: sub-layer and start-phase references in double get an added angle-rounding allowance of 1.5 ulp of the "
             "float angle 2*pi*p + phi (the float products/sums inside sin() round the angle; plan tolerance 3e-7 / 1 ulp "
             "alone does not cover it). Fixed before the first run.");

    // --- pitch (notes 0..127 at all rates) and tune / clamp (49 tunes x notes 12/60/108)
    struct Job { double fs; float note, tune; bool tuneJob; };
    std::vector<Job> jobs;
    for (double fs : kRates)
    {
        for (int n = 0; n <= 127; n += (gQuick ? 9 : 1)) jobs.push_back ({ fs, (float) n, 0.0f, false });
        for (int note : { 12, 60, 108 })
            for (int t = -24; t <= 24; t += (gQuick ? 8 : 1)) jobs.push_back ({ fs, (float) note, (float) t, true });
    }
    parallelFor ((int) jobs.size(), [&] (int i)
    {
        const auto& j = jobs[(size_t) i];
        const double fExp = expectedVoiceFreq (j.fs, j.note, j.tune);
        const int seg = segLenFor (fExp, j.fs), len = 16 + 4 * seg;
        const auto o = renderVoice (j.fs, synthSettings (KickVoice::Sine, j.tune, 0.0f), j.note, len, blockFor (i, 1024));
        Ctx c { j.fs, blockFor (i, 1024), fExp, 1.0, "note=" + num (j.note) + " tune=" + num (j.tune)
                + ((440.0 * std::pow (2.0, (j.note + j.tune - 69.0) / 12.0) > 0.45 * j.fs) ? " clamped" : ""), newSine() };
        const auto fit = fitFreq (o.out.data() + 16, len - 16, j.fs, fExp);
        // 1e-9: double increment + wrap drift < 1e-15 / sample; float phase conversion is bounded jitter, not drift
        rec (j.tuneJob ? gTune : gFreq, c, fExp, fExp * (1.0 + fit.rel), fit.rel, 1e-9);
        if (j.tuneJob) return;
        rec (gAmp, c, 0.0, dB (fit.amp), dB (fit.amp), 1e-5);
        // THD+N against the ideal sine of the ideal phase (double), and the exact float expression (mirror)
        const auto ph = idealPhase (j.fs, j.note, j.tune, 0.0f, len);
        double e2 = 0.0, mirrorWorst = 0.0;
        for (int n = 16; n < len; ++n)
        {
            const double y = o.out[(size_t) n];
            e2 += (y - std::sin (kTwoPi * ph[(size_t) n])) * (y - std::sin (kTwoPi * ph[(size_t) n]));
            const float m = std::sin (juce::MathConstants<float>::twoPi * (float) ph[(size_t) n]);
            mirrorWorst = std::max (mirrorWorst, std::abs (y - (double) m) / ulpOf (m));
        }
        const double thdn = 10.0 * std::log10 (std::max (e2 / (len - 16), 1e-300) / 0.5);
        recBound (gThd, c, thdn, -120.0, true);
        rec (gMir, c, 0.0, mirrorWorst, mirrorWorst, 2.0);   // in ulps: same float expression evaluated in the test
    });

    // --- triangle and warm sine (24 notes x 5 rates x 2)
    const auto warmC = harmonicAmps ([] (double u) { return std::tanh (2.0 * u) / std::tanh (2.0); }, 1.0, 40);
    struct WJob { double fs; float note; int wave; };
    std::vector<WJob> wj;
    for (double fs : kRates)
        for (int k = 0; k < 24; k += (gQuick ? 6 : 1))
            for (int wave : { (int) KickVoice::Triangle, (int) KickVoice::WarmSine })
                wj.push_back ({ fs, (float) (24 + 2 * k), wave });
    parallelFor ((int) wj.size(), [&] (int i)
    {
        const auto& j = wj[(size_t) i];
        const double f = expectedVoiceFreq (j.fs, j.note, 0.0f);
        const int len = 16 + (int) j.fs;
        const auto o = renderVoice (j.fs, synthSettings (j.wave, 0.0f, 0.0f), j.note, len, 256);
        Ctx c { j.fs, 256, f, 1.0, "note=" + num (j.note), newSine() };
        if (j.wave == KickVoice::Triangle)
        {
            const auto ph = idealPhase (j.fs, j.note, 0.0f, 0.0f, len);
            double worst = 0.0;
            for (int n = 0; n < len; ++n)
            {
                const double p = (double) (float) ph[(size_t) n];
                const double t = p < 0.25 ? 4 * p : (p < 0.75 ? 2 - 4 * p : 4 * p - 4);
                worst = std::max (worst, std::abs ((double) o.out[(size_t) n] - t));
            }
            rec (gTri, c, 0.0, worst, worst, 2.4e-7);   // 2 ulp at amplitude 1 (float 4p, 2 - 4p, 4p - 4)
        }
        else
        {
            const double w = kTwoPi * f / j.fs;
            for (int m = 1; m <= 39; m += 2)
            {
                if (m * f >= 0.45 * j.fs) break;
                const double expDb = dB (warmC[(size_t) m] / warmC[1]);
                if (expDb < -100.0) break;
                const double meas = std::abs (toneBH (o.out.data() + 16, len - 16, m * w));
                Ctx cm = c; cm.params += " m=" + std::to_string (m);
                rec (gWarm, cm, dB (warmC[(size_t) m]), dB (meas), dB (meas) - dB (warmC[(size_t) m]), 0.001);
            }
        }
    });

    // --- 909 tail: after both sweeps clamp to zero the frequency is exactly the root
    {
        struct J { int model; int note; };
        std::vector<J> jj;
        for (int m = 0; m < 10; ++m) for (int n = 24; n < 36; n += (gQuick ? 4 : 1)) jj.push_back ({ m, n });
        parallelFor ((int) jj.size(), [&] (int i)
        {
            const double fs = 48000.0;
            KickVoice::Settings s;
            s.engine = KickVoice::Engine909;
            s.model909 = jj[(size_t) i].model;
            s.hold909 = true;        // no body decay while held: constant amplitude for the phase fit
            s.attack909 = 0.0f;      // no pulse / noise
            const auto& md = kick909Models()[(size_t) jj[(size_t) i].model];
            const double f = expectedVoiceFreq (fs, (float) jj[(size_t) i].note, 0.0f);
            const int t0 = (int) ((3.0 * md.slowMs + 10.0) * 0.001 * fs), seg = segLenFor (f, fs);
            const auto o = renderVoice (fs, s, (float) jj[(size_t) i].note, t0 + 4 * seg, 512);
            const auto fit = fitFreq (o.out.data() + t0, 4 * seg, fs, f);
            rec (g909, { fs, 512, f, fit.amp, std::string ("model=") + md.name + " note=" + std::to_string (jj[(size_t) i].note), newSine() },
                 f, f * (1 + fit.rel), fit.rel, 1e-9);
        });
    }

    // --- sub layer, start phase, hit clock
    struct SJob { double fs; int note; float subPh; };
    std::vector<SJob> sj;
    for (double fs : kRates) for (int note : { 33, 45 }) for (float sp : { 0.0f, 90.0f, 217.5f }) sj.push_back ({ fs, note, sp });
    parallelFor ((int) sj.size(), [&] (int i)
    {
        const auto& j = sj[(size_t) i];
        auto s = synthSettings (KickVoice::Sine, 0.0f, 0.0f);
        s.subFadeMs = 60.0f;
        s.subPhaseDeg = j.subPh;
        const int len = (int) (0.3 * j.fs);
        const auto o = renderVoice (j.fs, s, (float) j.note, len, 128, true);
        const auto ph = idealPhase (j.fs, (float) j.note, 0.0f, 0.0f, len);
        const float msPer = (float) (1000.0 / j.fs);
        float tF = 0.0f;
        double wA = 0.0, wB = 0.0, ratioA = 0.0;
        for (int n = 0; n < len; ++n)
        {
            const double p = (double) (float) ph[(size_t) n];
            const double ang = kTwoPi * p + (double) j.subPh * kPi / 180.0;
            const double uA = std::min (1.0, (double) (tF / std::max (0.1f, s.subFadeMs)));
            tF += msPer;
            const double uB = std::min (1.0, (n * 1000.0 / j.fs) / 60.0);
            const double refA = std::sin (ang) * (0.5 - 0.5 * std::cos (kPi * uA));
            const double refB = std::sin (ang) * (0.5 - 0.5 * std::cos (kPi * uB));
            const double tol = 3e-7 + 1.5 * ulpOf (ang);
            const double eA = std::abs ((double) o.sub[(size_t) n] - refA);
            if (eA / tol > ratioA) { ratioA = eA / tol; wA = eA; }
            wB = std::max (wB, std::abs ((double) o.sub[(size_t) n] - refB));
        }
        Ctx c { j.fs, 128, expectedVoiceFreq (j.fs, (float) j.note, 0.0f), 1.0, "note=" + std::to_string (j.note) + " subPhase=" + num (j.subPh), newSine() };
        rec (gSubA, c, 0.0, wA, wA, wA / std::max (ratioA, 1e-30));
        rec (gSubB, c, 0.0, wB, wB, -1.0);   // informational: effect of the float hit clock on the fade (P7)
    });
    for (double fs : kRates)
        for (float phi : { 0.0f, 1.0f, 45.0f, 90.0f, 179.0f, 270.0f, 359.5f })
        {
            const auto o = renderVoice (fs, synthSettings (KickVoice::Sine, 0.0f, phi), 33.0f, 4, 4);
            const float p = (float) ((double) phi / 360.0);
            const float m = std::sin (juce::MathConstants<float>::twoPi * p);
            const double ex = std::sin (kTwoPi * (double) phi / 360.0);
            Ctx c { fs, 4, 0, 1, "phi=" + num (phi), newSine() };
            rec (gSp0, c, m, o.out[0], (o.out[0] - (double) m), ulpOf (m));
            rec (gSp1, c, ex, o.out[0], (o.out[0] - ex), ulpOf (ex) + 1.5 * ulpOf (kTwoPi * p));
        }
    for (double fs : kRates)
    {
        const int len = (int) fs + 1;
        const auto o = renderVoice (fs, synthSettings (KickVoice::Sine, 0.0f, 0.0f), 33.0f, len, 512);
        double bound = 0.0;
        for (int n = 1; n < len; ++n) bound += 0.5 * ulpOf (o.hit[(size_t) n]);
        const double ideal = (len - 1) * 1000.0 / fs, got = o.hit[(size_t) len - 1];
        Ctx c { fs, 512, 0, 0, "t=1s", newSine() };
        rec (gHit, c, ideal, got, got - ideal, bound);
        if (fs == 48000.0)
            note ("P7 hitClock (float accumulation): after 1 s at 48 kHz t = " + num (got, 9) + " ms vs " + num (ideal, 9)
                  + " ms, drift " + num (got - ideal, 4) + " ms (" + num (100.0 * (got - ideal) / ideal, 3) + " %); bound sum(ulp/2) = "
                  + num (bound, 4) + " ms");
    }
}

//==============================================================================
// 2. StereoOversampling
void osProcess (StereoOversampling& os, float* l, float* r, int n)
{
    const float* in[] = { l, r };
    juce::dsp::AudioBlock<const float> ib (in, 2, (size_t) n);
    os.processSamplesUp (ib);
    float* out[] = { l, r };
    juce::dsp::AudioBlock<float> ob (out, 2, (size_t) n);
    os.processSamplesDown (ob);
}

/** Checks a measured complex gain against the expected one: dB and degrees where |He| > thrDb, else |dH| absolute. */
void checkH (Group& gm, Group& gp, Group& ga, const Ctx& c, cd Hm, cd He, double tolDb, double tolDeg, double thrDb, double absTol)
{
    if (dBc (He) > thrDb)
    {
        rec (gm, c, dBc (He), dBc (Hm), dBc (Hm) - dBc (He), tolDb);
        rec (gp, c, phaseDeg (He), phaseDeg (Hm), wrapDeg (phaseDeg (Hm) - phaseDeg (He)), tolDeg);
    }
    else
        rec (ga, c, std::abs (He), std::abs (Hm), std::abs (Hm - He), absTol);
}

void suiteOs()
{
    auto& su = S ("os");
    auto& gM = su.g ("linear.mag", "dB");
    auto& gP = su.g ("linear.phase", "deg");
    auto& gA = su.g ("linear.below-40dB", "abs");
    auto& gLat = su.g ("latency.reported_vs_analytic", "samples");
    auto& gLatM = su.g ("latency.phase_delay_20Hz_vs_reported", "samples");
    auto& gImg = su.g ("image_rejection(up)", "dB");
    auto& gImgI = su.g ("image_rejection(up).below-110dB", "dB");
    auto& gAli = su.g ("alias_rejection(down)", "dB");
    auto& gAliI = su.g ("alias_rejection(down).below-110dB", "dB");
    auto& gBit = su.g ("bitexact_vs_juce_Oversampling", "ulp");
    const int maxBlock = 2048;

    // passband edge (analytic, informational)
    for (int st = 1; st <= 4; ++st)
    {
        double edge = 0.0;
        for (double x = 0.001; x < 0.5; x += 0.0005)
            if (std::abs (dBc (osG (osStages (st), kTwoPi * x, nullptr))) > 0.01) { edge = x; break; }
        note ("os " + std::to_string (1 << st) + "x: analytic -0.01 dB passband edge at " + num (edge, 4) + " fs (" + num (edge * 44100, 5)
              + " Hz at 44.1k, " + num (edge * 48000, 5) + " Hz at 48k); reported latency "
              + num (StereoOversampling (st).getLatencyInSamples(), 7) + " samples");
    }

    struct Job { int st; double fs; long long k; double A; int blk; };
    std::vector<Job> jobs;
    int idx = 0;
    for (int st = 1; st <= 4; ++st)
        for (double fs : kRates)
        {
            auto ks = logBins (10.0, 0.45 * fs, gQuick ? 12 : 48, fs, NA);
            for (size_t i = 0; i < ks.size(); ++i) { jobs.push_back ({ st, fs, ks[i], (i % 2) ? 1.0 : 0.5, blockFor (idx, maxBlock) }); ++idx; }
        }
    parallelFor ((int) jobs.size(), [&] (int i)
    {
        const auto& j = jobs[(size_t) i];
        StereoOversampling os (j.st);
        os.initProcessing ((size_t) maxBlock);
        const int settle = 4096, len = settle + NA;
        auto l = coherentSine (j.k, NA, j.A, 0.3, len), r = l;
        const auto x = l;
        runBlocks ([&] (float* a, float* b, int n) { osProcess (os, a, b, n); }, l.data(), r.data(), len, j.blk, maxBlock, i);
        const cd Hm = bin (l.data() + settle, NA, j.k) / bin (x.data() + settle, NA, j.k);
        const double w = kTwoPi * (double) j.k / NA;
        const cd He = osG (osStages (j.st), w, nullptr);
        Ctx c { j.fs, j.blk, j.k * j.fs / NA, j.A, "factor=" + std::to_string (1 << j.st), newSine() };
        // 0.001 dB / 0.01 deg: float all-pass cascade (<= 32 sections) rounding ~2e-6 relative
        checkH (gM, gP, gA, c, Hm, He, 0.001, 0.01, -40.0, 1e-5);
        bool same = true;
        for (int n = settle; n < len; ++n) same = same && l[(size_t) n] == r[(size_t) n];
        if (! same) note ("os: L and R differ for identical inputs (factor " + std::to_string (1 << j.st) + ")");
    });

    // latency: reported vs analytic group delay at DC, and measured phase delay at 20 Hz
    for (int st = 1; st <= 4; ++st)
        for (double fs : kRates)
        {
            StereoOversampling os (st);
            os.initProcessing ((size_t) maxBlock);
            const double rep = os.getLatencyInSamples();
            const double an = groupDelayAt0 ([&] (double w) { return osG (osStages (st), w, nullptr); });
            Ctx c { fs, 512, 0, 0, "factor=" + std::to_string (1 << st), newSine() };
            rec (gLat, c, an, rep, rep - an, 1e-3);
            const long long k = oddBin (20.0, fs, NA);
            const int len = 4096 + NA;
            auto l = coherentSine (k, NA, 0.5, 0.0, len), r = l;
            const auto x = l;
            runBlocks ([&] (float* a, float* b, int n) { osProcess (os, a, b, n); }, l.data(), r.data(), len, 512, maxBlock);
            const cd H = bin (l.data() + 4096, NA, k) / bin (x.data() + 4096, NA, k);
            const double pd = -std::arg (H) / (kTwoPi * (double) k / NA);
            c.f = k * fs / NA;
            rec (gLatM, c, rep, pd, pd - rep, 0.01);
        }

    // image rejection (up only): base N = NA / M, the up output is NA samples; images of bin k at q Nb +- k
    struct IJob { int st; double fs; long long k; };
    std::vector<IJob> ij;
    for (int st = 1; st <= 4; ++st)
        for (double fs : kRates)
        {
            const int Nb = NA >> st;
            for (auto k : logBins (20.0, 0.45 * fs, gQuick ? 6 : 24, fs, Nb)) ij.push_back ({ st, fs, k });
        }
    parallelFor ((int) ij.size(), [&] (int i)
    {
        const auto& j = ij[(size_t) i];
        const int M = 1 << j.st, Nb = NA / M, settle = 4096;
        StereoOversampling os (j.st);
        os.initProcessing (512);
        auto x = coherentSine (j.k, Nb, 0.5, 0.1, settle + Nb);
        std::vector<float> up ((size_t) NA);
        for (int pos = 0; pos < settle + Nb; pos += 512)
        {
            const int n = std::min (512, settle + Nb - pos);
            const float* in[] = { x.data() + pos, x.data() + pos };
            auto blk = os.processSamplesUp (juce::dsp::AudioBlock<const float> (in, 2, (size_t) n));
            if (pos >= settle) std::copy (blk.getChannelPointer (0), blk.getChannelPointer (0) + n * M, up.begin() + (size_t) (pos - settle) * M);
        }
        // images of f at j fs +- f (j = 1 .. M/2) in the up output, relative to the fundamental there
        const cd F = bin (up.data(), NA, j.k);
        const double f0 = j.k * j.fs / Nb;
        const auto& st = osStages (j.st);
        const long long sid = newSine();
        for (int q = 1; q <= M / 2; ++q)
            for (int sgn : { -1, 1 })
            {
                const double fImg = q * j.fs + sgn * f0;
                if (fImg <= 0.0 || fImg >= M * j.fs / 2.0) continue;
                const long long kb = (long long) q * Nb + sgn * j.k;
                const double meas = dBc (bin (up.data(), NA, kb)) - dBc (F);
                double ex = 0.0;
                for (int s = 0; s < j.st; ++s)
                {
                    const double rate = j.fs * (1 << (s + 1));
                    ex += dBc (hUp (st[(size_t) s], kTwoPi * fImg / rate)) - dBc (hUp (st[(size_t) s], kTwoPi * f0 / rate));
                }
                Ctx c { j.fs, 512, f0, 0.5, "factor=" + std::to_string (M) + " image=" + num (fImg, 7), sid };
                // |meas - analytic| <= 1 dB (float noise floor near -100 dB); below -110 dB: reported only
                if (ex > -110.0) rec (gImg, c, ex, meas, meas - ex, 1.0);
                else rec (gImgI, c, ex, meas, meas - ex, -1.0);
            }
    });

    // alias rejection (down only): a high-rate sine written into the up buffer, folded result at base bin
    struct AJob { int st; double fs; long long K; };
    std::vector<AJob> aj;
    for (int st = 1; st <= 4; ++st)
        for (double fs : kRates)
        {
            const int M = 1 << st;
            for (auto K : logBins (0.52 * fs, 0.98 * M * fs / 2, gQuick ? 6 : 24, M * fs, NA)) aj.push_back ({ st, fs, K });
        }
    parallelFor ((int) aj.size(), [&] (int i)
    {
        const auto& j = aj[(size_t) i];
        const int M = 1 << j.st, Nb = NA / M, settleB = 4096, tot = settleB + Nb;
        StereoOversampling os (j.st);
        os.initProcessing (512);
        const auto hi = coherentSine (j.K, NA, 0.5, 0.2, tot * M);
        std::vector<float> zeros (512, 0.0f), out ((size_t) tot), outR ((size_t) tot);
        for (int pos = 0; pos < tot; pos += 512)
        {
            const int n = std::min (512, tot - pos);
            const float* in[] = { zeros.data(), zeros.data() };
            auto blk = os.processSamplesUp (juce::dsp::AudioBlock<const float> (in, 2, (size_t) n));
            for (int c = 0; c < 2; ++c) std::copy (hi.begin() + (size_t) pos * M, hi.begin() + (size_t) (pos + n) * M, blk.getChannelPointer ((size_t) c));
            float* o[] = { out.data() + pos, outR.data() + pos };
            juce::dsp::AudioBlock<float> ob (o, 2, (size_t) n);
            os.processSamplesDown (ob);
        }
        long long kb = j.K % Nb;
        if (kb > Nb / 2) kb = Nb - kb;
        const double meas = dBc (bin (out.data() + settleB, Nb, kb)) - dB (0.5 * Nb / 2.0);
        double F = j.K * (M * j.fs) / NA, ex = 0.0;
        const auto& st = osStages (j.st);
        for (int s = j.st - 1; s >= 0; --s)
        {
            const double rateIn = j.fs * (1 << (s + 1)), rateOut = rateIn / 2;
            ex += dBc (hDn (st[(size_t) s], kTwoPi * F / rateIn));
            F = std::fmod (F, rateOut);
            if (F > rateOut / 2) F = rateOut - F;
        }
        Ctx c { j.fs, 512, j.K * (M * j.fs) / NA, 0.5, "factor=" + std::to_string (M) + " alias_at=" + num (kb * j.fs / Nb, 7), newSine() };
        if (ex > -110.0) rec (gAli, c, ex, meas, meas - ex, 1.0);
        else rec (gAliI, c, ex, meas, meas - ex, -1.0);
    });

    // bit-exactness against juce::dsp::Oversampling (polyphase IIR, max quality, no integer latency)
    parallelFor (20, [&] (int i)
    {
        const int st = 1 + i % 4;
        const double fs = kRates[(size_t) (i / 4)];
        StereoOversampling a (st);
        a.initProcessing (512);
        juce::dsp::Oversampling<float> b (2, (size_t) st, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        b.initProcessing (512);
        const int len = 48000;
        auto l = coherentSine (oddBin (997.0, fs, NA), NA, 0.7, 0.0, len), r = coherentSine (oddBin (3001.0, fs, NA), NA, 0.4, 1.0, len);
        auto l2 = l, r2 = r;
        juce::Random rng (i + 11);
        double worst = 0.0;
        for (int pos = 0; pos < len;)
        {
            const int n = std::min (1 + rng.nextInt (512), len - pos);
            osProcess (a, l.data() + pos, r.data() + pos, n);
            float* ch[] = { l2.data() + pos, r2.data() + pos };
            juce::dsp::AudioBlock<float> blk (ch, 2, (size_t) n);
            auto up = b.processSamplesUp (blk);
            juce::ignoreUnused (up);
            b.processSamplesDown (blk);
            pos += n;
        }
        for (int n = 0; n < len; ++n)
            worst = std::max ({ worst, std::abs ((double) l[(size_t) n] - l2[(size_t) n]) / ulpOf (l2[(size_t) n]),
                                std::abs ((double) r[(size_t) n] - r2[(size_t) n]) / ulpOf (r2[(size_t) n]) });
        rec (gBit, { fs, -1, 0, 0, "factor=" + std::to_string (1 << st) + " random blocks", newSine() }, 0, worst, worst, 0.0);
    });
}

//==============================================================================
// 3. ClipDistortion
ClipDistortion::Settings distBase (int mode, float driveDb, int os)
{
    ClipDistortion::Settings s;
    s.enabled = true;
    s.mode = mode;
    s.driveDb = driveDb;
    s.mix = 1.0f;
    s.outDb = 0.0f;
    s.dcFilter = false;
    s.oversampling = os;
    return s;
}

std::unique_ptr<ClipDistortion> makeDist (double fs, const ClipDistortion::Settings& s, int maxBlock, const FreeCurve* custom = nullptr)
{
    auto d = std::make_unique<ClipDistortion>();
    if (custom != nullptr) { d->publishCustomCurve (custom); d->setCustomFadeMs (0.0f); }
    d->setSettings (s);
    d->prepare (fs, maxBlock);   // prepare snaps every smoother and filter glide (applySettings (true))
    return d;
}

int osFactorOf (int osIdx) { return (osIdx >= 1 && osIdx <= 4) ? (1 << osIdx) : 1; }
const char* osName (int osIdx) { static const char* n[] = { "Off", "2x", "4x", "8x", "16x", "Raw" }; return n[osIdx]; }

/** Base-rate response of a unit whose curve-rate response is `inner` (OS Off / Raw: inner itself). */
cd distExpected (int osIdx, double w, const RespFn& inner)
{
    if (osIdx >= 1 && osIdx <= 4) return osG (osStages (osIdx), w, inner);
    return inner ? inner (w) : cd (1.0);
}

struct MeasH { cd L, R; std::vector<float> l, r, x; };
MeasH measureStereo (const ProcFn& p, long long k, double A, int settle, int block, int maxBlock, int seed, bool keep = false)
{
    const int len = settle + NA;
    MeasH m;
    m.l = coherentSine (k, NA, A, 0.3, len);
    m.r = m.l;
    m.x = m.l;
    runBlocks (p, m.l.data(), m.r.data(), len, block, maxBlock, seed);
    const cd X = bin (m.x.data() + settle, NA, k);
    m.L = bin (m.l.data() + settle, NA, k) / X;
    m.R = bin (m.r.data() + settle, NA, k) / X;
    if (! keep) { m.l.clear(); m.r.clear(); m.x.clear(); }
    return m;
}

ProcFn distFn (ClipDistortion& d) { return [&d] (float* l, float* r, int n) { d.process (l, r, n); }; }

void suiteDist()
{
    auto& su = S ("dist");
    const int maxBlock = 2048;

    //--------------------------------------------------------------------------
    // 3a-1 static transfer() against the independent double formulas
    {
        auto& g = su.g ("3a1.static_transfer", "abs");
        auto& gStep = su.g ("3a1.static_transfer(step/kink bracket)", "abs");
        auto& gC = su.g ("3a1.continuity", "abs");
        tolNote ("dist 3a1: the static-curve tolerance adds the cancellation and argument-rounding terms that the plan's "
                 "4 ulp(|y|) leaves out: 4 * (ulp(y) + ulp(f(x g + off)) + ulp(f(off)) + |f'| * (ulp(x g) + ulp(x g + off))) "
                 "* (1 + 40 chr^2); at a step or kink the float result must lie within the double curve evaluated at the "
                 "two ends of that argument-rounding interval (same tolerance). Fixed before the first run.");
        tolNote ("dist reference curves: Diode / Fuzz use expm1 and Tri Fold an exact period reduction in the double reference; the first "
                 "run's 1 - exp(.) and (x + 1) / 4 forms cancelled near 0 in double, so the reference itself was off by ~3e-16 at tiny outputs "
                 "(corrected after the first run; listed here). The Raw per-sample failures that remain are the code's own float forms at inputs "
                 "~1e-16 (the sin(pi k) samples of the coherent sine): 1 - expf(.) and (x + 1) * 0.25 lose all relative precision there "
                 "(absolute error < 1e-15).");
        tolNote ("dist 3b: the per-sample Raw tolerance includes the 3a1 static-curve bound as documented; the first run's code had "
                 "left out its |f'| * delta and (1 + 40 chr^2) terms (a harness bug, corrected after the first run; listed here).");
        parallelFor (ClipDistortion::numModes, [&] (int mode)
        {
            for (float drv : { 0.0f, 6.0f, 18.0f, 48.0f })
                for (float off : { -1.0f, -0.3f, 0.0f, 0.5f, 1.0f })
                    for (float chr : { 0.0f, 0.5f, 1.0f })
                    {
                        const float gain = juce::Decibels::decibelsToGain (drv);
                        double worst = 0.0, worstTol = 1.0, wRatio = -1, wx = 0, stepWorst = 0, stepTol = 1, sRatio = -1;
                        int stepCases = 0;
                        for (int i = 0; i <= 4000; ++i)
                        {
                            const float x = (float) (-20.0 + 40.0 * i / 4000.0);
                            const double gg = mode == ClipDistortion::Downsample ? 1.0 : (double) gain;
                            const float yF = ClipDistortion::transfer (mode, x, gain, off, chr);
                            const double v = (double) x * gg + off, yD = myTransfer (mode, x, gg, off, chr);
                            const double delta = ulpOf ((double) x * gg) + ulpOf (v);
                            const double h = 1e-6 * std::max (1.0, std::abs (v));
                            const double fp = std::min (1e3, std::abs (myShape (mode, v + h) - myShape (mode, v - h)) / (2 * h));
                            const double tol = 4.0 * (ulpOf (yD) + ulpOf (myShape (mode, v)) + ulpOf (myShape (mode, off)) + fp * delta)
                                               * (1.0 + 40.0 * chr * chr);
                            const double y1 = myHarden (myShape (mode, v - delta) - myShape (mode, off), chr);
                            const double y2 = myHarden (myShape (mode, v + delta) - myShape (mode, off), chr);
                            if (std::abs (y2 - y1) > 10.0 * tol)
                            {
                                ++stepCases;
                                const double lo = std::min ({ y1, y2, yD }) - tol, hi = std::max ({ y1, y2, yD }) + tol;
                                const double e = yF < lo ? lo - yF : (yF > hi ? yF - hi : 0.0);
                                if (e / tol > sRatio) { sRatio = e / tol; stepWorst = e; stepTol = tol; }
                            }
                            else
                            {
                                const double e = std::abs ((double) yF - yD);
                                if (e / tol > wRatio) { wRatio = e / tol; worst = e; worstTol = tol; wx = x; }
                            }
                        }
                        const std::string p = std::string ("mode=") + ClipDistortion::modeNames()[mode].toStdString() + " drive=" + num (drv)
                                              + " off=" + num (off) + " chr=" + num (chr);
                        rec (g, { 0, 0, 0, 0, p + " worst_x=" + num (wx), -1 }, 0, worst, worst, worstTol);
                        if (stepCases > 0) rec (gStep, { 0, 0, 0, 0, p + " cases=" + std::to_string (stepCases), -1 }, 0, stepWorst, stepWorst, stepTol);
                    }
            // continuity at the piece boundaries (not Bitcrush: steps by design)
            std::vector<float> b;
            switch (mode)
            {
                case ClipDistortion::HardClip: case ClipDistortion::SoftClip: case ClipDistortion::ZeroSquare:
                case ClipDistortion::Downsample: case ClipDistortion::XShaper: case ClipDistortion::XShaperAsym: b = { -1.0f, 1.0f }; break;
                case ClipDistortion::Tube: case ClipDistortion::Diode: case ClipDistortion::Fuzz: case ClipDistortion::Rectify: b = { 0.0f }; break;
                case ClipDistortion::TriFold: b = { -3.0f, -1.0f, 1.0f, 3.0f }; break;
                case ClipDistortion::AsymClip: b = { -0.55f, 1.0f }; break;
                default: break;
            }
            for (float x : b)
            {
                const float a = ClipDistortion::shapeFor (mode, std::nextafter (x, -100.0f)), c = ClipDistortion::shapeFor (mode, std::nextafter (x, 100.0f));
                rec (gC, { 0, 0, 0, 0, std::string ("mode=") + ClipDistortion::modeNames()[mode].toStdString() + " at=" + num (x), -1 }, 0, c - a, c - a, 1e-6);
            }
        });
    }

    //--------------------------------------------------------------------------
    // 3a-2 DC levels through process(): constant input -> exactly the static curve (ADAA dv -> 0 branch)
    {
        auto& g = su.g ("3a2.dc_levels_vs_static", "abs");
        struct J { int mode, os; };
        std::vector<J> jobs;
        for (int m = 0; m < ClipDistortion::numModes; ++m) for (int os : { 5, 0, 1, 4 }) jobs.push_back ({ m, os });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            const double fs = 48000.0;
            auto d = makeDist (fs, distBase (j.mode, 0.0f, j.os), 256);
            std::vector<float> l (4096), r (4096);
            for (int lv = 0; lv <= 200; lv += (gQuick ? 10 : 1))
            {
                const float level = (float) (-16.0 + 32.0 * lv / 200.0);
                std::fill (l.begin(), l.end(), level);
                std::fill (r.begin(), r.end(), level);
                runBlocks (distFn (*d), l.data(), r.data(), 4096, 256, 256);
                const double ex = myTransfer (j.mode, level, 1.0, 0.0, 0.0);
                double worst = 0.0;
                for (int n = 4096 - 1024; n < 4096; ++n) worst = std::max (worst, std::abs ((double) l[(size_t) n] - ex));
                // 1e-5 * max(1,|y|): OS DC gain rounding through <= 32 float sections (~2e-6) plus 4 ulp of curve error
                rec (g, { fs, 256, 0, level, std::string ("mode=") + ClipDistortion::modeNames()[j.mode].toStdString() + " os=" + osName (j.os), -1 },
                     ex, l[4095], worst, 1e-5 * std::max (1.0, std::abs (ex)));
            }
        });
    }

    //--------------------------------------------------------------------------
    // 3b Raw: per-sample exactness, y = (dry + 1 * (transfer (x) - dry)) * og; Downsample hold pattern index-exact
    {
        auto& g = su.g ("3b.raw_per_sample", "abs");
        struct J { double fs; int mode; double A; float drv; long long k; float off, chr; };
        std::vector<J> jobs;
        const std::pair<double, float> ad[] = { { 0.1, 0.0f }, { 0.5, 0.0f }, { 0.9, 6.0f }, { 1.5, 12.0f }, { 4.0, 0.0f }, { 12.0, 24.0f } };
        int cnt = 0;
        for (double fs : kRates)
            for (int m = 0; m < ClipDistortion::numModes; ++m)
                for (auto [A, drv] : ad)
                    for (auto k : logBins (30.0, 0.4 * fs, 8, fs, 8192))
                        for (float off : { 0.0f, 0.3f })
                            for (float chr : { 0.0f, 0.5f })
                            {
                                ++cnt;
                                if (fs != 48000.0 && cnt % 4 != 0) continue;
                                if (gQuick && cnt % 16 != 0) continue;
                                jobs.push_back ({ fs, m, A, drv, k, off, chr });
                            }
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (j.mode, j.drv, ClipDistortion::rawOversampling);
            s.offset = j.off;
            s.character = j.chr;
            auto d = makeDist (j.fs, s, maxBlock);
            const int len = 8192, blk = blockFor (i, maxBlock);
            auto l = coherentSine (j.k, 8192, j.A, 0.0, len), r = l;
            const auto x = l;
            runBlocks (distFn (*d), l.data(), r.data(), len, blk, maxBlock, i);
            const float gain = juce::Decibels::decibelsToGain (j.drv);
            const int hold = std::max (1, (int) (ClipDistortion::downsampleFactor (j.drv) * 1.0f));
            int holdCount = 0;
            float held = 0.0f;
            double worst = 0.0, ratio = -1.0, wTol = 1.0;
            for (int n = 0; n < len; ++n)
            {
                float w = x[(size_t) n];
                if (j.mode == ClipDistortion::Downsample) { if (--holdCount <= 0) { held = w; holdCount = hold; } w = held; }
                const double wD = myTransfer (j.mode, w, gain, j.off, j.chr);
                const float dry = x[(size_t) n];
                const double ex = (double) dry + 1.0 * (wD - dry);
                // 4 ulp(|w|) + 2 ulp(max(|dry|,|w|)) (cancellation in dry + (w - dry)), plus the static-curve bound of 3a1
                const double tol = 4 * ulpOf (wD) + 2 * ulpOf (std::max (std::abs ((double) dry), std::abs (wD)))
                                   + staticTol (j.mode, w, gain, j.off, j.chr);
                double e = std::abs ((double) l[(size_t) n] - ex);
                // a float/double disagreement exactly at a step of Bitcrush (rounding tie) counts as the either-side value
                if (j.mode == ClipDistortion::Bitcrush && e > tol)
                {
                    const double alt1 = myTransfer (j.mode, std::nextafter (w, -1e9f), gain, j.off, j.chr), alt2 = myTransfer (j.mode, std::nextafter (w, 1e9f), gain, j.off, j.chr);
                    e = std::min ({ e, std::abs (l[(size_t) n] - ((double) dry + (alt1 - dry))), std::abs (l[(size_t) n] - ((double) dry + (alt2 - dry))) });
                }
                if (e / tol > ratio) { ratio = e / tol; worst = e; wTol = tol; }
            }
            rec (g, { j.fs, blk, j.k * j.fs / 8192, j.A, std::string ("mode=") + ClipDistortion::modeNames()[j.mode].toStdString() + " drive=" + num (j.drv)
                      + " off=" + num (j.off) + " chr=" + num (j.chr), newSine() }, 0, worst, worst, wTol);
        });
    }

    //--------------------------------------------------------------------------
    // 3c ADAA in the linear region: wet = d A(z), A = (1 + z^-1) / 2 at the curve rate; dry = Thiran; mix {1, 0, 0.5}
    {
        auto& gM = su.g ("3c.adaa_linear.mag", "dB");
        auto& gP = su.g ("3c.adaa_linear.phase", "deg");
        auto& gA = su.g ("3c.adaa_linear.below-40dB", "abs");
        auto& gDip = su.g ("3c.mix0.5_vs_|0.5(1+d cos(w/2))|", "dB");
        auto& gT = su.g ("3c.thiran_analytic", "abs");
        const double aT = (double) (1.0f / 3.0f);
        rec (gT, { 0, 0, 0, 0, "|H_T(w)| - 1 max over w", -1 }, 1.0, 1.0, [&] { double m = 0; for (double w = 0.001; w < kPi; w += 0.01) m = std::max (m, std::abs (std::abs (thiran (w, aT)) - 1.0)); return m; }(), 1e-6);
        rec (gT, { 0, 0, 0, 0, "phase delay at DC (samples)", -1 }, 0.5, -std::arg (thiran (1e-5, aT)) / 1e-5, -std::arg (thiran (1e-5, aT)) / 1e-5 - 0.5, 1e-4);
        struct J { int os; double fs; long long k; double A; float d; float mix; int blk; };
        std::vector<J> jobs;
        int idx = 0;
        for (int os : { 0, 1, 2, 3, 4 })
            for (double fs : kRates)
            {
                const auto ks = logBins (10.0, 0.45 * fs, gQuick ? 8 : 48, fs, NA);
                for (size_t i = 0; i < ks.size(); ++i)
                    for (float mix : { 1.0f, 0.0f, 0.5f })
                    {
                        const bool lowA = (i % 2) == 0;
                        jobs.push_back ({ os, fs, ks[i], lowA ? 0.1 : 0.5, lowA ? 6.0f : 1.5f, mix, blockFor (idx++, maxBlock) });
                    }
            }
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            const float driveDb = juce::Decibels::gainToDecibels (j.d);
            auto s = distBase (ClipDistortion::HardClip, driveDb, j.os);
            s.mix = j.mix;
            auto dist = makeDist (j.fs, s, maxBlock);
            const double d = juce::Decibels::decibelsToGain (driveDb);
            const auto m = measureStereo (distFn (*dist), j.k, j.A, 4096, j.blk, maxBlock, i);
            const double mix = j.mix;
            const RespFn inner = [=] (double th) { return (1.0 - mix) * thiran (th, aT) + mix * d * adaaA (th); };
            const double w = kTwoPi * (double) j.k / NA;
            const cd He = distExpected (j.os, w, inner);
            Ctx c { j.fs, j.blk, j.k * j.fs / NA, j.A, std::string ("os=") + osName (j.os) + " d=" + num (d) + " mix=" + num (j.mix), newSine() };
            // 0.001 dB / 0.01 deg: as section 2 (the double ADAA table is exact for a linear curve)
            checkH (gM, gP, gA, c, m.L, He, 0.001, 0.01, -40.0, 1e-5);
            if (j.mix == 0.5f)
            {
                const cd ref = distExpected (j.os, w, [=] (double th) { return cd (0.5 * (1.0 + d * std::cos (th / 2.0))); });
                rec (gDip, c, dBc (ref), dBc (m.L), dBc (m.L) - dBc (ref), -1.0);
            }
        });
        // P2: analytic wet-path loss of the ADAA average
        std::ostringstream o;
        o << "P2 ADAA wet loss (analytic, d = 1, base rate incl. OS filters): ";
        for (int os : { 0, 1, 2, 4 })
        {
            const auto H = [&] (double w) { return distExpected (os, w, [] (double th) { return adaaA (th); }); };
            o << osName (os) << ": fs/4 " << num (dBc (H (kPi / 2)), 3) << " dB, 0.45fs " << num (dBc (H (0.9 * kPi)), 3) << " dB (ADAA alone "
              << num (dB (std::cos (0.45 * kPi / osFactorOf (os))), 3) << ");  ";
        }
        note (o.str() + "dry path (Thiran) |H| = 1.");
    }

    //--------------------------------------------------------------------------
    // 3d static nonlinearity with ADAA and OS: harmonic levels vs Fourier coefficients x ADAA kernel x |G0|; aliasing
    {
        auto& gH1 = su.g ("3d.harmonics(>-60dBc)", "dB");
        auto& gH2 = su.g ("3d.harmonics(-60..-90dBc)", "dB");
        auto& gHi = su.g ("3d.harmonics(nonsmooth m>30, info)", "dB");
        auto& gAl = su.g ("3d.alias_energy", "dBc");
        auto& gMono = su.g ("3d.alias_monotonic(Raw>=Off>=2x..16x, 1dB slack)", "dB");
        auto& gAdaa = su.g ("3d.alias_ADAA_Off_beats_Raw_by_6dB", "dB");
        std::vector<int> modes;
        for (int m = 0; m < ClipDistortion::numModes; ++m) if (m != ClipDistortion::Bitcrush && m != ClipDistortion::Downsample) modes.push_back (m);
        auto nonsmooth = [] (int m) { return m == ClipDistortion::HardClip || m == ClipDistortion::SineFold || m == ClipDistortion::TriFold || m == ClipDistortion::Fuzz; };
        const double A = 0.5;
        std::map<std::pair<int, int>, std::vector<double>> coef;   // (mode, drive) -> c_m
        for (int m : modes) for (int drv : { 0, 12, 24 })
        {
            const double d = juce::Decibels::decibelsToGain ((float) drv);
            coef[{ m, drv }] = harmonicAmps ([m] (double u) { return myShape (m, u) - myShape (m, 0.0); }, A * d, 400);
        }
        struct J { int mode, os, drv; double fs, f0; };
        std::vector<J> jobs;
        for (int m : modes)
            for (double f0 : { 50.0, 200.0, 1000.0, 5000.0 })
                for (int drv : { 0, 12, 24 })
                {
                    for (int os : { 5, 0, 1, 2, 3, 4 }) jobs.push_back ({ m, os, drv, 48000.0, f0 });
                    if (! gQuick) for (double fs : { 44100.0, 88200.0, 96000.0, 192000.0 }) jobs.push_back ({ m, 2, drv, fs, f0 });
                }
        std::mutex am;
        std::map<std::tuple<int, int, int>, std::map<int, double>> alias;   // (mode, f0, drv) -> os -> alias dBc (48k)
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto dist = makeDist (j.fs, distBase (j.mode, (float) j.drv, j.os), maxBlock);
            const long long k = oddBin (j.f0, j.fs, NA);
            const int settle = 4096, len = settle + NA;
            auto l = coherentSine (k, NA, A, 0.0, len), r = l;
            runBlocks (distFn (*dist), l.data(), r.data(), len, 512, maxBlock);
            std::vector<cd> Y ((size_t) NA);
            for (int n = 0; n < NA; ++n) Y[(size_t) n] = l[(size_t) (settle + n)];
            fft (Y, false);
            auto amp = [&] (long long m) { long long b = (m * k) % NA; if (b > NA / 2) b = NA - b; return 2.0 * std::abs (Y[(size_t) b]) / NA; };
            const double f0 = k * j.fs / NA, w0 = kTwoPi * k / NA;
            const auto& c = coef[{ j.mode, j.drv }];
            const int M = osFactorOf (j.os);
            // reference: the strongest expected in-band component (the fundamental, except for Rectify: no fundamental)
            double c1 = 0.0;
            for (int m = 1; m <= 400 && m * f0 < 0.4 * j.fs; ++m)
                c1 = std::max (c1, c[(size_t) m] * (j.os == 5 ? 1.0 : std::abs (adaaA (m * w0 / M))) * std::abs (distExpected (j.os, m * w0, nullptr)));
            const long long sid = newSine();
            for (int m = 1; m <= 400 && m * f0 < 0.4 * j.fs; ++m)
            {
                const double ex = c[(size_t) m] * (j.os == 5 ? 1.0 : std::abs (adaaA (m * w0 / M))) * std::abs (distExpected (j.os, m * w0, nullptr));
                const double exc = dB (ex / c1);
                if (exc < -90.0) continue;
                const double meas = amp (m);
                Ctx cx { j.fs, 512, f0, A, std::string ("mode=") + ClipDistortion::modeNames()[j.mode].toStdString() + " os=" + osName (j.os)
                         + " drive=" + std::to_string (j.drv) + " m=" + std::to_string (m) + " exp_dBr=" + num (exc, 4), sid };
                if (nonsmooth (j.mode) && m > 30) rec (gHi, cx, dB (ex), dB (meas), dB (meas) - dB (ex), -1.0);
                else if (exc > -60.0) rec (gH1, cx, dB (ex), dB (meas), dB (meas) - dB (ex), 0.1);
                else rec (gH2, cx, dB (ex), dB (meas), dB (meas) - dB (ex), 1.5);
            }
            double pa = 0.0, pref = 0.0;
            for (long long m = 1; m < NA / 2; ++m)
            {
                const double a = amp (m);
                if (m * f0 > 0.5 * j.fs) pa += a * a; else pref = std::max (pref, a * a);
            }
            const double al = 10.0 * std::log10 (std::max (pa, 1e-300) / std::max (pref, 1e-300));
            rec (gAl, { j.fs, 512, f0, A, std::string ("mode=") + ClipDistortion::modeNames()[j.mode].toStdString() + " os=" + osName (j.os)
                        + " drive=" + std::to_string (j.drv), sid }, 0, al, al, -1.0);
            if (j.fs == 48000.0) { const std::lock_guard<std::mutex> lk (am); alias[{ j.mode, (int) j.f0, j.drv }][j.os] = al; }
        });
        for (auto& [key, mp] : alias)
        {
            const auto [mode, f0, drv] = key;
            const std::string p = std::string ("mode=") + ClipDistortion::modeNames()[mode].toStdString() + " f0=" + std::to_string (f0) + " drive=" + std::to_string (drv);
            const int order[] = { 5, 0, 1, 2, 3, 4 };
            for (int q = 1; q < 6; ++q)
            {
                if (! mp.count (order[q - 1]) || ! mp.count (order[q])) continue;
                const double prev = mp[order[q - 1]], cur = mp[order[q]];
                if (prev < -150.0 && cur < -150.0) continue;   // both at the float floor
                recBound (gMono, { 48000.0, 512, (double) f0, A, p + " " + osName (order[q - 1]) + "->" + osName (order[q]), -1 }, cur - prev, 1.0, true);
            }
            const bool needs = (mode == ClipDistortion::HardClip || mode == ClipDistortion::SoftClip || mode == ClipDistortion::Tanh || mode == ClipDistortion::Fuzz)
                               && f0 >= 1000 && drv >= 12;
            if (needs && mp.count (5) && mp.count (0))
                recBound (gAdaa, { 48000.0, 512, (double) f0, A, p, -1 }, mp[5] - mp[0], 6.0, false);
        }
        note ("dist 3d: harmonic levels and the alias energy are referred to the strongest in-band component (the fundamental for every mode but Rectify, whose fundamental is zero).");
        note ("dist 3d: alias metric floor = harmonics of order ~65 000 (coherent N = 2^16, k odd), i.e. <= -96 dBc for hard clip before OS/ADAA.");
    }

    //--------------------------------------------------------------------------
    // 3e linear filters inside the unit (curve in its linear region)
    {
        auto& gM = su.g ("3e.lr2_lo_hi+dry_allpass.mag", "dB");
        auto& gP = su.g ("3e.lr2_lo_hi+dry_allpass.phase", "deg");
        auto& gA = su.g ("3e.lr2_lo_hi+dry_allpass.below-40dB", "abs");
        auto& gN = su.g ("3e.lr2.mix0.5_no_notch", "dB");
        auto& gId = su.g ("3e.lr2.phase_identity_analytic", "deg");
        // analytic identity: arg (LP1^2) == arg (2 LP1 - 1) == arg (-(1 - LP1)^2)
        {
            double worst = 0.0;
            for (double G : { 0.001, 0.02, 0.2, 0.45 })
                for (double w = 0.001; w < kPi; w += 0.003)
                {
                    const cd lp = tptLP (w, G);
                    worst = std::max ({ worst, std::abs (wrapDeg (phaseDeg (lp * lp) - phaseDeg (2.0 * lp - 1.0))),
                                        std::abs (wrapDeg (phaseDeg (-(1.0 - lp) * (1.0 - lp)) - phaseDeg (2.0 * lp - 1.0))) });
                }
            rec (gId, { 0, 0, 0, 0, "max over G, w", -1 }, 0, worst, worst, 1e-9);
        }
        struct J { double fs; float lo, hi, mix; long long k; int blk; };
        std::vector<J> jobs;
        int idx = 0;
        for (double fs : { 48000.0, 192000.0 })
            for (float lo : { 200.0f, 1000.0f, 5000.0f, 15000.0f })
                for (float hi : { 30.0f, 150.0f, 2000.0f })
                    for (float mix : { 0.0f, 0.5f, 1.0f })
                        for (auto k : logBins (10.0, 0.45 * fs, gQuick ? 6 : 24, fs, NA)) jobs.push_back ({ fs, lo, hi, mix, k, blockFor (idx++, maxBlock) });
        const double aT = (double) (1.0f / 3.0f);
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::HardClip, 0.0f, 2);
            s.loFilterHz = j.lo; s.hiFilterHz = j.hi; s.mix = j.mix;
            auto dist = makeDist (j.fs, s, maxBlock);
            const int settle = std::max (4096, (int) (20.0 * j.fs / (kTwoPi * j.hi)));
            const auto m = measureStereo (distFn (*dist), j.k, 0.5, settle, j.blk, maxBlock, i);
            const double rate = 4.0 * j.fs, Gl = tptG (rate, j.lo), Gh = tptG (rate, j.hi), mix = j.mix;
            const RespFn wet = [=] (double th) { const cd a = tptLP (th, Gl), b = tptLP (th, Gh); return adaaA (th) * a * a * (-(1.0 - b) * (1.0 - b)); };
            const RespFn dry = [=] (double th) { return (2.0 * tptLP (th, Gl) - 1.0) * (2.0 * tptLP (th, Gh) - 1.0) * thiran (th, aT); };
            const double w = kTwoPi * (double) j.k / NA;
            const cd He = distExpected (2, w, [=] (double th) { return dry (th) + mix * (wet (th) - dry (th)); });
            Ctx c { j.fs, j.blk, j.k * j.fs / NA, 0.5, "lo=" + num (j.lo) + " hi=" + num (j.hi) + " mix=" + num (j.mix), newSine() };
            // 0.002 dB / 0.02 deg: float one-pole states (~1e-6 rounding noise)
            checkH (gM, gP, gA, c, m.L, He, 0.002, 0.02, -40.0, 1e-5);
            if (j.mix == 0.5f)
            {
                const double mw = std::abs (distExpected (2, w, wet)), md = std::abs (distExpected (2, w, dry));
                recBound (gN, c, dBc (m.L), dB (0.5 * std::min (mw, md)) - 0.01, false);
            }
        });
    }
    {
        auto& gM = su.g ("3e.svf.mag", "dB");
        auto& gP = su.g ("3e.svf.phase", "deg");
        auto& gA = su.g ("3e.svf.below-40dB", "abs");
        auto& gPk = su.g ("3e.svf.LP12_res1_peak_vs_20log10(Q*resComp)", "dB");
        struct J { int type, pos, os; float fc, res; long long k; int blk; };
        std::vector<J> jobs;
        int idx = 0;
        const double fs = 48000.0;
        for (int type = 0; type < 6; ++type)
            for (float fc : { 100.0f, 1000.0f, 8000.0f })
                for (float res : { 0.0f, 0.2f, 1.0f })
                    for (int pos : { 1, 2 })
                        for (int os : { 0, 2 })
                            for (auto k : denseBins (fc, fs, NA, gQuick ? 6 : 24)) jobs.push_back ({ type, pos, os, fc, res, k, blockFor (idx++, maxBlock) });
        auto svfResp = [] (int type, float fc, float res, double rate)
        {
            const float hz = std::min (juce::jlimit (20.0f, (float) (rate * 0.45), fc), (float) (rate * 0.45));
            const double g = std::tan (juce::MathConstants<float>::pi * hz / (float) rate);
            const float kf = 1.0f / (0.5f + 17.5f * res * res);
            const double k = kf, rc = 1.0f / std::sqrt (juce::jmax (1.0f, 1.0f / kf) / 4.0f + 0.75f), k2 = 1.41421356f;
            return [=] (double th) -> cd
            {
                const cd s = cd (0.0, std::tan (th / 2.0) / g);
                auto D = [&] (double kk) { return s * s + kk * s + 1.0; };
                switch (type)
                {
                    case 0: return rc / D (k);
                    case 1: return rc / (D (k2) * D (k));
                    case 2: return k * s / D (k);
                    case 3: return rc * s * s / D (k);
                    case 4: return rc * s * s * s * s / (D (k2) * D (k));
                    default: return (1.0 + s * s) / D (k);
                }
            };
        };
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::HardClip, 0.0f, j.os);
            s.filterPos = j.pos; s.filterType = j.type; s.filterHz = j.fc; s.filterRes = j.res;
            auto dist = makeDist (fs, s, maxBlock);
            const double Q = 0.5 + 17.5 * j.res * j.res;
            const int settle = std::max ((int) (0.25 * fs), (int) (20.0 * Q / (kPi * j.fc) * fs) + 4096);
            const auto m = measureStereo (distFn (*dist), j.k, 0.063, settle, j.blk, maxBlock, i);
            const double rate = fs * osFactorOf (j.os);
            const auto svf = svfResp (j.type, j.fc, j.res, rate);
            const double w = kTwoPi * (double) j.k / NA;
            const cd He = distExpected (j.os, w, [=] (double th) { return adaaA (th) * svf (th); });
            Ctx c { fs, j.blk, j.k * fs / NA, 0.063, "type=" + ClipDistortion::filterTypeNames()[j.type].toStdString() + " fc=" + num (j.fc) + " res=" + num (j.res)
                    + " pos=" + std::string (j.pos == 1 ? "pre" : "post") + " os=" + osName (j.os), newSine() };
            // 0.005 dB / 0.05 deg: Q up to 18 raises the float state noise gain by about Q
            checkH (gM, gP, gA, c, m.L, He, 0.005, 0.05, -40.0, 1e-5);
        });
        // peak spot check: LP12, res 1, fc 1 kHz, post, OS Off: measured at the analytic peak vs 20 log10 (Q * resComp)
        {
            const auto svf = svfResp (0, 1000.0f, 1.0f, fs);
            double best = 0, bw = 0;
            for (double f = 900; f < 1100; f += 0.01) { const double v = std::abs (svf (kTwoPi * f / fs) * adaaA (kTwoPi * f / fs)); if (v > best) { best = v; bw = f; } }
            const long long k = oddBin (bw, fs, NA);
            auto s = distBase (ClipDistortion::HardClip, 0.0f, 0);
            s.filterPos = 2; s.filterType = 0; s.filterHz = 1000.0f; s.filterRes = 1.0f;
            auto dist = makeDist (fs, s, maxBlock);
            const auto m = measureStereo (distFn (*dist), k, 0.063, (int) fs * 2, 512, maxBlock, 3);
            const float kf = 1.0f / (0.5f + 17.5f);
            const double rc = 1.0f / std::sqrt (juce::jmax (1.0f, 1.0f / kf) / 4.0f + 0.75f), Q = 1.0 / kf;
            const double formula = dB (Q * rc);
            const double measCorr = dBc (m.L) - dBc (adaaA (kTwoPi * k / NA));   // remove the ADAA average at this frequency
            rec (gPk, { fs, 512, k * fs / NA, 0.063, "Q=" + num (Q) + " resComp=" + num (rc, 6), newSine() }, formula, measCorr, measCorr - formula, 0.01);
            note ("dist SVF: resComp = 1/sqrt(max(1,Q)/4 + 0.75) = " + num (rc, 5) + " at Q = 18, so the LP12 resonant peak is "
                  + num (formula, 4) + " dB (the plan predicted resComp 0.2325 / +12.4 dB; the code comment says ~+12 dB); measured "
                  + num (measCorr, 5) + " dB.");
        }
    }
    {
        auto& gM = su.g ("3e.high_shelf.mag", "dB");
        auto& gP = su.g ("3e.high_shelf.phase", "deg");
        auto& gA = su.g ("3e.high_shelf.below-40dB", "abs");
        struct J { double fs; float hz, db; long long k; };
        std::vector<J> jobs;
        for (double fs : { 48000.0, 96000.0 })
            for (float hz : { 2000.0f, 8000.0f })
                for (float db : { -12.0f, 12.0f })
                    for (auto k : denseBins (hz, fs, NA, gQuick ? 6 : 24)) jobs.push_back ({ fs, hz, db, k });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::HardClip, 0.0f, 2);
            s.shelfHz = j.hz; s.shelfDb = j.db;
            auto dist = makeDist (j.fs, s, maxBlock);
            const auto m = measureStereo (distFn (*dist), j.k, 0.12, 8192, 512, maxBlock, i);
            const auto hs = ref::highShelf (j.hz, j.db, (double) 0.707f, 4.0 * j.fs);
            const cd He = distExpected (2, kTwoPi * (double) j.k / NA, [=] (double th) { return adaaA (th) * hs.H (th); });
            checkH (gM, gP, gA, { j.fs, 512, j.k * j.fs / NA, 0.12, "shelfHz=" + num (j.hz) + " shelfDb=" + num (j.db), newSine() }, m.L, He, 0.002, 0.02, -40.0, 1e-5);
        });
    }
    {
        auto& gM = su.g ("3e.dc_filter.mag", "dB");
        auto& gP = su.g ("3e.dc_filter.phase", "deg");
        auto& gA = su.g ("3e.dc_filter.below-40dB", "abs");
        struct J { double fs; long long k; int blk; };
        std::vector<J> jobs;
        int idx = 0;
        for (double fs : kRates) for (auto k : logBins (10.0, 0.45 * fs, gQuick ? 8 : 48, fs, NA)) jobs.push_back ({ fs, k, blockFor (idx++, maxBlock) });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::HardClip, 0.0f, ClipDistortion::rawOversampling);
            s.dcFilter = true;
            auto dist = makeDist (j.fs, s, maxBlock);
            const double R = dcRfor (j.fs);
            const int settle = std::max ((int) (0.25 * j.fs), (int) (20.0 / -std::log (R)));
            const auto m = measureStereo (distFn (*dist), j.k, 0.5, settle, j.blk, maxBlock, i);
            const cd He = dcHP (kTwoPi * (double) j.k / NA, R);
            // 1e-4 dB / 0.001 deg: a single first-order float recursion
            checkH (gM, gP, gA, { j.fs, j.blk, j.k * j.fs / NA, 0.5, "Raw", newSine() }, m.L, He, 1e-4, 0.001, -40.0, 1e-5);
        });
        const double R = dcRfor (48000.0);
        auto h = [&] (double f) { return dcHP (kTwoPi * f / 48000.0, R); };
        note ("dist DC filter (48 kHz, analytic): " + num (dBc (h (10)), 4) + " dB at 10 Hz, " + num (dBc (h (30)), 4) + " dB / "
              + num (phaseDeg (h (30)), 4) + " deg at 30 Hz, " + num (dBc (h (50)), 4) + " dB / " + num (phaseDeg (h (50)), 4) + " deg at 50 Hz.");
    }
    {
        auto& g = su.g ("3e.gain_boost_drive_out", "dB");
        auto& gS = su.g ("3e.spatial_LR_ratio", "rel");
        int idx = 0;
        struct J { double fs; float boost, drive, out, sp; };
        std::vector<J> jobs;
        for (double fs : kRates)
            for (float boost : { 0.0f, 6.0f })
                for (float drive : { 0.0f, 12.0f })
                    for (float out : { -6.0f, 0.0f, 3.0f })
                        for (float sp : { 0.0f, 0.5f, 1.0f })
                            if (! gQuick || (idx++ % 5) == 0) jobs.push_back ({ fs, boost, drive, out, sp });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::HardClip, j.drive, ClipDistortion::rawOversampling);
            s.boostDb = j.boost; s.outDb = j.out; s.spatial = j.sp;
            auto dist = makeDist (j.fs, s, maxBlock);
            const long long k = oddBin (1000.0, j.fs, NA);
            const auto m = measureStereo (distFn (*dist), k, 0.02, 4096, 256, maxBlock, i);
            const double ex = j.boost + j.drive + j.out + dB (1.0 + 0.6 * j.sp);
            Ctx c { j.fs, 256, k * j.fs / NA, 0.02, "boost=" + num (j.boost) + " drive=" + num (j.drive) + " out=" + num (j.out) + " sp=" + num (j.sp), newSine() };
            rec (g, c, ex, dBc (m.L), dBc (m.L) - ex, 0.001);
            const double ratio = std::abs (m.L / m.R), er = (1 + 0.6 * j.sp) * (1 + 0.6 * j.sp);
            rec (gS, c, er, ratio, ratio / er - 1.0, 1e-6);
        });
    }

    //--------------------------------------------------------------------------
    // 3f disabled unit: dry + 0 (w - dry) through the oversampler and the (still active) DC filter
    {
        auto& gB = su.g ("3f.disabled.bitexact(OS Off/Raw, DC off)", "abs");
        auto& gM = su.g ("3f.disabled.mag", "dB");
        auto& gP = su.g ("3f.disabled.phase", "deg");
        auto& gA = su.g ("3f.disabled.below-40dB", "abs");
        struct J { int os; bool dc; double fs; long long k; int blk; };
        std::vector<J> jobs;
        int idx = 0;
        for (int os = 0; os <= 5; ++os)
            for (bool dc : { true, false })
                for (double fs : kRates)
                    for (auto k : logBins (10.0, 0.45 * fs, gQuick ? 6 : 48, fs, NA)) jobs.push_back ({ os, dc, fs, k, blockFor (idx++, maxBlock) });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::Tanh, 18.0f, j.os);
            s.enabled = false; s.dcFilter = j.dc; s.outDb = -6.0f;
            auto dist = makeDist (j.fs, s, maxBlock);
            const double R = dcRfor (j.fs);
            const int settle = j.dc ? std::max ((int) (0.25 * j.fs), (int) (20.0 / -std::log (R))) : 4096;
            const auto m = measureStereo (distFn (*dist), j.k, 0.5, settle, j.blk, maxBlock, i, true);
            Ctx c { j.fs, j.blk, j.k * j.fs / NA, 0.5, std::string ("os=") + osName (j.os) + " dc=" + (j.dc ? "on" : "off"), newSine() };
            if ((j.os == 0 || j.os == 5) && ! j.dc)
            {
                double worst = 0.0;
                for (size_t n = 0; n < m.l.size(); ++n) worst = std::max ({ worst, std::abs ((double) m.l[n] - m.x[n]), std::abs ((double) m.r[n] - m.x[n]) });
                rec (gB, c, 0, worst, worst, 0.0);
                return;
            }
            const double w = kTwoPi * (double) j.k / NA;
            const cd He = distExpected (j.os, w, nullptr) * (j.dc ? dcHP (w, R) : cd (1.0));
            checkH (gM, gP, gA, c, m.L, He, 0.001, 0.01, -40.0, 1e-5);
        });
        const double R = dcRfor (48000.0);
        auto two = [&] (double f) { const double w = kTwoPi * f / 48000.0; const cd h = osG (osStages (2), w, nullptr) * dcHP (w, R); return h * h; };
        note ("P1 two disabled distortions with plug-in defaults (OS 4x, DC on) at 48 kHz (analytic): " + num (dBc (two (30)), 4) + " dB at 30 Hz, "
              + num (dBc (two (50)), 4) + " dB / " + num (phaseDeg (two (50) * std::polar (1.0, kTwoPi * 50.0 / 48000.0 * 2.0 * StereoOversampling (2).getLatencyInSamples())), 4)
              + " deg (latency removed) at 50 Hz: the bypass is not transparent (DC filter is not gated by On).");
    }

    //--------------------------------------------------------------------------
    // 3g tail drive, auto gain, custom curves
    {
        auto& g = su.g ("3g.tail_drive_per_sample", "rel");
        struct J { double fs; float tdb, split; };
        std::vector<J> jobs;
        for (double fs : { 48000.0, 96000.0 }) for (float tdb : { -12.0f, 12.0f }) for (float split : { 20.0f, 100.0f }) jobs.push_back ({ fs, tdb, split });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::HardClip, 6.0f, ClipDistortion::rawOversampling);
            s.tailDriveDb = j.tdb; s.splitMs = j.split;
            auto dist = makeDist (j.fs, s, 256);
            const int len = (int) (0.3 * j.fs);
            auto l = coherentSine (oddBin (1000.0, j.fs, NA), NA, 0.01, 0.0, len), r = l;
            const auto x = l;
            std::vector<float> hit ((size_t) len);
            for (int n = 0; n < len; ++n) hit[(size_t) n] = (float) (n * 1000.0 / j.fs);
            for (int pos = 0; pos < len; pos += 256) { const int n = std::min (256, len - pos); dist->process (l.data() + pos, r.data() + pos, n, hit.data() + pos); }
            double worst = 0.0;
            for (int n = 0; n < len; ++n)
            {
                if (std::abs (x[(size_t) n]) < 1e-3) continue;
                const double u = std::min (1.0, std::max (0.0, hit[(size_t) n] / std::max (1.0, (double) j.split)));
                const double ex = x[(size_t) n] * std::pow (10.0, (6.0 + j.tdb * u * u * (3 - 2 * u)) / 20.0);
                worst = std::max (worst, std::abs (l[(size_t) n] / ex - 1.0));
            }
            rec (g, { j.fs, 256, 1000, 0.01, "tailDb=" + num (j.tdb) + " split=" + num (j.split), newSine() }, 0, worst, worst, 1e-6);
        });
    }
    {
        auto& g = su.g ("3g.auto_gain_level", "dB");
        auto& gI = su.g ("3g.auto_gain_level(needed gain outside [0.06,16], info)", "dB");
        struct J { double f; float drive, out; };
        std::vector<J> jobs;
        for (double f : { 1000.0, 5000.0 }) for (float drive = 0.0f; drive <= 36.0f; drive += (gQuick ? 18.0f : 6.0f)) for (float out : { 0.0f, -6.0f }) jobs.push_back ({ f, drive, out });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            const double fs = 48000.0;
            const int len = (int) (12.0 * fs), tail = (int) (2.0 * fs);
            double rms[2] = { 0, 0 }, rin = 0.0;
            for (int ag = 0; ag < 2; ++ag)
            {
                auto s = distBase (ClipDistortion::Tanh, j.drive, 2);
                s.dcFilter = true; s.autoGain = ag == 1; s.outDb = j.out;
                auto dist = makeDist (fs, s, 512);
                auto l = coherentSine (oddBin (j.f, fs, NA), NA, 0.5, 0.0, len), r = l;
                const auto x = l;
                runBlocks (distFn (*dist), l.data(), r.data(), len, 512, 512);
                double a = 0, b = 0;
                for (int n = len - tail; n < len; ++n) { a += (double) l[(size_t) n] * l[(size_t) n]; b += (double) x[(size_t) n] * x[(size_t) n]; }
                rms[ag] = std::sqrt (a / tail);
                rin = std::sqrt (b / tail);
            }
            const double needed = rin / rms[0];   // gain auto gain must apply (target 0 dB) to match the input
            const double meas = dB (rms[1] / rin);
            Ctx c { fs, 512, j.f, 0.5, "drive=" + num (j.drive) + " out=" + num (j.out) + " needed=" + num (needed, 4), newSine() };
            // +-0.1 dB: 1 s trackers and 0.0005/sample follower leave < 0.05 dB ripple at >= 1 kHz
            if (needed * juce::Decibels::decibelsToGain (j.out) >= 0.06 && needed * juce::Decibels::decibelsToGain (j.out) <= 16.0)
                rec (g, c, j.out, meas, meas - j.out, 0.1);
            else rec (gI, c, j.out, meas, meas - j.out, -1.0);
        });
    }
    {
        auto& gM = su.g ("3g.custom_linear_curve.mag", "dB");
        auto& gP = su.g ("3g.custom_linear_curve.phase", "deg");
        auto& gA = su.g ("3g.custom_linear_curve.below-40dB", "abs");
        auto& gAct = su.g ("3g.custom_curve_active", "bool");
        struct J { double fs; int os; long long k; };
        std::vector<J> jobs;
        for (double fs : kRates) for (int os : { 0, 2 }) for (auto k : logBins (10.0, 0.45 * fs, gQuick ? 6 : 24, fs, NA)) jobs.push_back ({ fs, os, k });
        const auto lin = FreeCurve::linear();
        const double aT = (double) (1.0f / 3.0f);
        juce::ignoreUnused (aT);
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = distBase (ClipDistortion::HardClip, 3.52f, j.os);
            s.custom = true;
            auto dist = makeDist (j.fs, s, maxBlock, &lin);
            const double d = juce::Decibels::decibelsToGain (3.52f);
            const auto m = measureStereo (distFn (*dist), j.k, 0.5, 4096, 512, maxBlock, i);
            Ctx c { j.fs, 512, j.k * j.fs / NA, 0.5, std::string ("os=") + osName (j.os), newSine() };
            if (i % 24 == 0) rec (gAct, c, 1, dist->isCustomActive() ? 1 : 0, dist->isCustomActive() ? 0 : 1, 0.0);
            const cd He = distExpected (j.os, kTwoPi * (double) j.k / NA, [=] (double th) { return d * adaaA (th); });
            checkH (gM, gP, gA, c, m.L, He, 0.001, 0.01, -40.0, 1e-5);
        });
    }
    {
        auto& g = su.g ("3g.curveseed_harmonics_vs_legacy", "abs");
        const int modes[] = { ClipDistortion::Tanh, ClipDistortion::SoftClip, ClipDistortion::Overdrive, ClipDistortion::Diode2, ClipDistortion::SineFold };
        parallelFor (5, [&] (int i)
        {
            const int mode = modes[i];
            const double fs = 48000.0;
            const float drv = 12.0f, dg = juce::Decibels::decibelsToGain (drv);
            const ClipDistortion::Curve legacy;
            const float reach = CurveSeed::reachFor (mode, legacy, dg, 1.0f, 0.0f, 0.0f, 0.0f);
            const auto seeded = CurveSeed::fromMode (mode, legacy, reach);
            const long long k = oddBin (1000.0, fs, NA);
            std::vector<double> amps[2];
            for (int which = 0; which < 2; ++which)
            {
                auto s = distBase (mode, drv, ClipDistortion::rawOversampling);
                s.custom = which == 1;
                auto dist = makeDist (fs, s, 512, which == 1 ? &seeded.curve : nullptr);
                auto l = coherentSine (k, NA, 0.5, 0.0, 4096 + NA), r = l;
                runBlocks (distFn (*dist), l.data(), r.data(), 4096 + NA, 512, 512);
                for (int m = 1; m <= 20; ++m) amps[which].push_back (2.0 * std::abs (bin (l.data() + 4096, NA, m * k)) / NA);
            }
            const double bound = 4.0 * seeded.maxError / kPi + 1e-5;
            double worst = 0.0;
            for (int m = 0; m < 20; ++m) worst = std::max (worst, std::abs (amps[0][(size_t) m] - amps[1][(size_t) m]));
            rec (g, { fs, 512, 1000, 0.5, std::string ("mode=") + ClipDistortion::modeNames()[mode].toStdString() + " reach=" + num (reach) + " maxError="
                      + num (seeded.maxError), newSine() }, 0, worst, worst, bound);
        });
    }
}

//==============================================================================
// 4. Parametric EQ / EqEngine
struct EqCfg { eq::BandSettings b; std::string name; bool smooth = true; };

eq::BandSettings band (eq::BandType t, float f, float g, float q, int slope = eq::defaultSlope, eq::Placement p = eq::Placement::Stereo)
{
    eq::BandSettings b;
    b.on = true; b.type = t; b.freq = f; b.gainDb = g; b.q = q; b.slope = slope; b.placement = p;
    return b;
}

std::string bandName (const eq::BandSettings& b)
{
    std::ostringstream o;
    o << std::setprecision (9) << eq::bandTypeNames()[(int) b.type] << " f=" << b.freq << " g=" << b.gainDb << " q=" << b.q;
    if (eq::typeUsesSlope (b.type)) o << " slope=" << ref::slopeDb[b.slope] << "dB";
    return o.str();
}

std::vector<EqCfg> eqZlConfigs()
{
    using T = eq::BandType;
    std::vector<EqCfg> v;
    for (float g : { 3.0f, -3.0f, 12.0f, -12.0f, 30.0f, -30.0f }) for (float q : { 0.1f, 0.707f, 4.0f, 18.0f }) v.push_back ({ band (T::Bell, 1000, g, q), "", true });
    for (float g : { 6.0f, -6.0f, 24.0f, -24.0f }) for (float q : { 0.5f, 0.707f, 2.0f })
    {
        v.push_back ({ band (T::LowShelf, 200, g, q), "", true });
        v.push_back ({ band (T::HighShelf, 5000, g, q), "", true });
    }
    for (int sl = 0; sl < 8; ++sl)
    {
        v.push_back ({ band (T::LowCut, sl % 2 ? 20.0f : 5000.0f, 0, 0.707f, sl), "", true });
        v.push_back ({ band (T::HighCut, sl % 2 ? 200.0f : 18000.0f, 0, 0.707f, sl), "", true });
        for (float q : { 0.5f, 2.0f }) v.push_back ({ band (T::BandPass, 1000, 0, q, sl), "", true });
    }
    v.push_back ({ band (T::LowCut, 100, 0, 2.0f, 1), "", true });   // order 2 with a user Q
    v.push_back ({ band (T::HighCut, 3000, 0, 4.0f, 1), "", true });
    for (float q : { 0.5f, 4.0f }) { v.push_back ({ band (T::Notch, 1000, 0, q), "", true }); v.push_back ({ band (T::AllPass, 1000, 0, q), "", true }); }
    for (float g : { 6.0f, -12.0f }) { v.push_back ({ band (T::TiltShelf, 1000, g, 0.707f), "", true }); v.push_back ({ band (T::FlatTilt, 1000, g, 0.707f), "", true }); }
    for (auto& c : v) c.name = bandName (c.b);
    return v;
}

/** Exact-point property of a band at its own frequency (dB), or NaN when it has none. */
double exactPointDb (const eq::BandSettings& b)
{
    using T = eq::BandType;
    const int order = ref::slopeDb[b.slope] / 6;
    switch (b.type)
    {
        case T::Bell: return b.gainDb;
        case T::LowShelf: case T::HighShelf: return b.gainDb / 2.0;
        case T::LowCut: case T::HighCut: return order == 2 ? dB (std::max (0.05, (double) b.q)) : -10.0 * std::log10 (2.0);
        case T::BandPass: case T::TiltShelf: case T::AllPass: return 0.0;
        default: return std::numeric_limits<double>::quiet_NaN();
    }
}

/** EQ FIR length from the documented rule: 4096 * 2^res, doubled for every halving of fs that stays above 50 kHz, <= 2^18. */
int myFirLength (int res, double fs)
{
    int n = 4096 << std::clamp (res, 0, 4);
    for (double r = fs; r > 50000.0; r *= 0.5) n <<= 1;
    return std::min (n, 1 << 18);
}

/** The linear-phase design replicated in double: magnitude sampled at k fs / N (k = 0 at 1 Hz), the 2x2 matrix, an inverse
    FFT, a circular shift by N/2 and a Blackman window with denominator N - 1. Returns h(L->L) and h(L->R). */
struct FirRef { int N = 0; std::vector<double> ll, lr; };
FirRef buildFirRef (const std::vector<eq::BandSettings>& bands, int res, double fs)
{
    FirRef r;
    const int N = myFirLength (res, fs);
    r.N = N;
    std::vector<Casc> d;
    for (auto& b : bands) d.push_back (ref::design (b, fs));
    std::vector<cd> s0 ((size_t) N), s2 ((size_t) N);
    for (int k = 0; k <= N / 2; ++k)
    {
        const double f = std::max (1.0, (double) k * fs / N), w = kTwoPi * f / fs;
        double m = 1, sd = 1, dl = 1, dr = 1;
        for (size_t i = 0; i < bands.size(); ++i)
        {
            if (d[i].empty()) continue;
            const double g = std::abs (Hc (d[i], w));
            switch (bands[i].placement)
            {
                case eq::Placement::Stereo: m *= g; sd *= g; break;
                case eq::Placement::Mid: m *= g; break;
                case eq::Placement::Side: sd *= g; break;
                case eq::Placement::Left: dl *= g; break;
                case eq::Placement::Right: dr *= g; break;
            }
        }
        s0[(size_t) k] = (double) (float) (dl * 0.5 * (m + sd));
        s2[(size_t) k] = (double) (float) (dr * 0.5 * (m - sd));
        if (k > 0 && k < N / 2) { s0[(size_t) (N - k)] = s0[(size_t) k]; s2[(size_t) (N - k)] = s2[(size_t) k]; }
    }
    fft (s0, true);
    fft (s2, true);
    r.ll.resize ((size_t) N);
    r.lr.resize ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const int src = (i + N / 2) % N;
        const double w = 0.42 - 0.5 * std::cos (kTwoPi * i / (N - 1)) + 0.08 * std::cos (2.0 * kTwoPi * i / (N - 1));
        r.ll[(size_t) i] = s0[(size_t) src].real() * w;
        r.lr[(size_t) i] = s2[(size_t) src].real() * w;
    }
    return r;
}
cd firH (const std::vector<double>& h, double w)
{
    cd acc = 0.0, p = 1.0;
    const cd rot = std::polar (1.0, -w);
    for (size_t i = 0; i < h.size(); ++i) { if ((i & 1023) == 0) p = std::polar (1.0, -w * (double) i); acc += h[i] * p; p *= rot; }
    return acc;
}

void suiteEq()
{
    auto& su = S ("eq");
    const int maxBlock = 2048;
    const auto cfgs = eqZlConfigs();

    //--------------------------------------------------------------------------
    // Zero Latency
    {
        auto& gM = su.g ("zl.mag", "dB");
        auto& gP = su.g ("zl.phase", "deg");
        auto& gA = su.g ("zl.-40..-100dB_abs", "abs");
        auto& gI = su.g ("zl.below-100dB(info)", "dB");
        auto& gX = su.g ("zl.exact_points", "dB");
        auto& gN = su.g ("zl.notch_depth_at_f0", "dB");
        auto& gC = su.g ("zl.consistency_eq::magnitude", "dB");
        struct J { size_t cfg; double fs; };
        std::vector<J> jobs;
        for (size_t c = 0; c < cfgs.size(); ++c) for (double fs : kRates) if (! gQuick || c % 4 == 0) jobs.push_back ({ c, fs });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto b = cfgs[j.cfg].b;
            const long long k0 = oddBin (b.freq, j.fs, NA);
            if (b.freq < 0.45 * j.fs) b.freq = (float) (k0 * j.fs / NA);   // put the band's own frequency on a bin
            const auto c = ref::design (b, j.fs);
            const int settle = settleFor (c, j.fs);
            std::set<long long> ks;
            for (auto k : denseBins (b.freq, j.fs, NA, gQuick ? 8 : 24)) ks.insert (k);
            if (b.freq < 0.45 * j.fs) ks.insert (oddBin (b.freq, j.fs, NA));
            eq::ParametricEQ<Params::numBands> e;
            e.prepare (j.fs);
            e.setBand (0, b);
            const auto des = eq::design (b, j.fs);
            int idx = 0;
            for (auto k : ks)
            {
                e.reset();
                const int blk = blockFor (i + idx++, maxBlock);
                const auto m = measureStereo ([&] (float* l, float* r, int n) { e.process (l, r, n); }, k, 0.25, settle, blk, maxBlock, i);
                const double w = kTwoPi * (double) k / NA, f = k * j.fs / NA;
                const cd He = Hc (c, w);
                Ctx cx { j.fs, blk, f, 0.25, bandName (b), newSine() };
                const double ed = dBc (He);
                if (ed >= -40.0)
                {
                    // 0.001 dB / 0.01 deg: double TDF-II states, float rounding only at each stage output (<= 8 x 6e-8)
                    rec (gM, cx, ed, dBc (m.L), dBc (m.L) - ed, 0.001);
                    rec (gP, cx, phaseDeg (He), phaseDeg (m.L), wrapDeg (phaseDeg (m.L) - phaseDeg (He)), 0.01);
                }
                else if (ed >= -100.0) rec (gA, cx, std::abs (He), std::abs (m.L), std::abs (m.L - He), 2e-6);
                else rec (gI, cx, ed, dBc (m.L), dBc (m.L) - ed, -1.0);
                if (ed > -100.0)
                {
                    const double em = dB (eq::magnitude (des, f, j.fs));
                    rec (gC, cx, ed, em, em - ed, 1e-9);
                }
                if (k == k0 && b.freq < 0.45 * j.fs)
                {
                    if (b.type == eq::BandType::Notch) recBound (gN, cx, dBc (m.L), -100.0, true);
                    const double xp = exactPointDb (b);
                    if (std::isfinite (xp)) rec (gX, cx, xp, dBc (m.L), dBc (m.L) - xp, 0.001);
                }
            }
        });
        // placements (L = s, R = 0)
        auto& gPl = su.g ("zl.placement", "dB");
        auto& gPa = su.g ("zl.placement.zero_channel", "abs");
        parallelFor (5, [&] (int p)
        {
            const double fs = 48000.0;
            auto b = band (eq::BandType::Bell, 1000, 12, 2);
            b.placement = (eq::Placement) p;
            const auto c = ref::design (b, fs);
            eq::ParametricEQ<Params::numBands> e;
            e.prepare (fs);
            e.setBand (0, b);
            for (auto k : logBins (30.0, 20000.0, 12, fs, NA))
            {
                e.reset();
                const int len = 24000 + NA;
                auto l = coherentSine (k, NA, 0.25, 0.0, len);
                std::vector<float> r ((size_t) len, 0.0f);
                const auto x = l;
                runBlocks ([&] (float* a, float* bb, int n) { e.process (a, bb, n); }, l.data(), r.data(), len, 512, maxBlock);
                const cd X = bin (x.data() + 24000, NA, k), Lm = bin (l.data() + 24000, NA, k) / X, Rm = bin (r.data() + 24000, NA, k) / X;
                const cd H = Hc (c, kTwoPi * (double) k / NA);
                cd Le = 0, Re = 0;
                switch (b.placement)
                {
                    case eq::Placement::Stereo: case eq::Placement::Left: Le = H; Re = 0; break;
                    case eq::Placement::Right: Le = 1; Re = 0; break;
                    case eq::Placement::Mid: Le = 0.5 * (H + 1.0); Re = 0.5 * (H - 1.0); break;
                    case eq::Placement::Side: Le = 0.5 * (1.0 + H); Re = 0.5 * (1.0 - H); break;
                }
                const char* pn[] = { "Stereo", "Left", "Right", "Mid", "Side" };
                Ctx cx { fs, 512, k * fs / NA, 0.25, std::string ("placement=") + pn[p], newSine() };
                rec (gPl, cx, dBc (Le), dBc (Lm), dBc (Lm) - dBc (Le), 0.001);
                if (std::abs (Re) > 0.01) rec (gPl, cx, dBc (Re), dBc (Rm), dBc (Rm) - dBc (Re), 0.001);
                else rec (gPa, cx, std::abs (Re), std::abs (Rm), std::abs (Rm - Re), 2e-6);
            }
        });
    }

    //--------------------------------------------------------------------------
    // Natural Phase: bands at 2 fs inside JUCE's 2x polyphase IIR oversampler with integer latency compensation
    {
        auto& gM = su.g ("natural.mag(<0.4fs)", "dB");
        auto& gA = su.g ("natural.below-40dB", "abs");
        auto& gP = su.g ("natural.phase(<0.2fs)", "deg");
        auto& gPi = su.g ("natural.phase(>=0.2fs, info)", "deg");
        auto& gL = su.g ("natural.latency_reported_vs_expected_int", "samples");
        auto& gLm = su.g ("natural.measured_delay_vs_reported(bands off)", "samples");
        juce::dsp::Oversampling<float> unc (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        const double latU = unc.getLatencyInSamples();
        double fd = 1.0 - (latU - std::floor (latU));
        if (std::abs (fd - 1.0) < 1e-6) fd = 0.0; else if (fd < 0.618) fd += 1.0;
        const double alpha = (double) ((1.0f - (float) fd) / (1.0f + (float) fd));
        const int expLat = juce::roundToInt (latU + fd);
        std::vector<size_t> sel;
        for (size_t c = 0; c < cfgs.size(); c += std::max<size_t> (1, cfgs.size() / 20)) sel.push_back (c);
        struct J { size_t cfg; double fs; };
        std::vector<J> jobs;
        for (auto c : sel) for (double fs : kRates) jobs.push_back ({ c, fs });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            const auto b = cfgs[j.cfg].b;
            const auto c2 = ref::design (b, 2.0 * j.fs);
            const int settle = settleFor (c2, 2.0 * j.fs) / 2 + 4096;
            eq::EqEngine E;
            E.prepare (j.fs, maxBlock);
            E.setBand (0, b);
            E.setMode (1, 1);
            rec (gL, { j.fs, 0, 0, 0, cfgs[j.cfg].name, -1 }, expLat, E.getLatencyInSamples(), E.getLatencyInSamples() - expLat, 0.0);
            for (auto k : denseBins (b.freq, j.fs, NA, gQuick ? 8 : 24))
            {
                E.setMode (0, 1);   // a mode change resets every filter and the oversampler (fresh state per sine)
                E.setMode (1, 1);
                const auto m = measureStereo ([&] (float* l, float* r, int n) { E.process (l, r, n); }, k, 0.25, settle, 512, maxBlock, i);
                const double w = kTwoPi * (double) k / NA, f = k * j.fs / NA;
                const cd He = osG (osStages (1), w, [&] (double th) { return Hc (c2, th); }) * (fd > 0 ? thiran (w, alpha) : cd (1.0));
                Ctx cx { j.fs, 512, f, 0.25, cfgs[j.cfg].name, newSine() };
                if (f < 0.4 * j.fs)
                {
                    if (dBc (He) > -40.0) rec (gM, cx, dBc (He), dBc (m.L), dBc (m.L) - dBc (He), 0.003);
                    else rec (gA, cx, std::abs (He), std::abs (m.L), std::abs (m.L - He), 1e-5);
                }
                if (dBc (He) > -40.0)
                {
                    const double dp = wrapDeg (phaseDeg (m.L) - phaseDeg (He));
                    if (f < 0.2 * j.fs) rec (gP, cx, phaseDeg (He), phaseDeg (m.L), dp, 1.0);
                    else rec (gPi, cx, phaseDeg (He), phaseDeg (m.L), dp, -1.0);
                }
            }
        });
        for (double fs : kRates)
        {
            eq::EqEngine E;
            E.prepare (fs, maxBlock);
            E.setMode (1, 1);
            const long long k = oddBin (20.0, fs, NA);
            const auto m = measureStereo ([&] (float* l, float* r, int n) { E.process (l, r, n); }, k, 0.25, 8192, 512, maxBlock, 1);
            const double pd = -std::arg (m.L) / (kTwoPi * (double) k / NA);
            rec (gLm, { fs, 512, k * fs / NA, 0.25, "phase delay at ~20 Hz", newSine() }, E.getLatencyInSamples(), pd, pd - E.getLatencyInSamples(), 0.05);
        }
        note ("eq Natural Phase: JUCE uncompensated 2x latency " + num (latU, 8) + " samples + Thiran fractional delay " + num (fd, 6)
              + " = reported " + std::to_string (expLat) + " samples.");
    }

    //--------------------------------------------------------------------------
    // Linear Phase
    {
        auto& gLat = su.g ("linear.latency==N/2+256", "samples");
        auto& gM = su.g ("linear.vs_design_exact_FIR.mag", "dB");
        auto& gP = su.g ("linear.vs_design_exact_FIR.phase", "deg");
        auto& gId = su.g ("linear.vs_ideal_IIR_mag(smooth bands)", "dB");
        auto& gIdI = su.g ("linear.vs_ideal_IIR_mag(narrow/steep, info)", "dB");
        auto& gLin = su.g ("linear.phase_linearity", "deg");
        auto& gOff = su.g ("linear.all_bands_off==delay", "abs");
        auto& gPl = su.g ("linear.placement_matrix", "dB");
        auto& gPla = su.g ("linear.placement_matrix.below-60dB", "abs");
        using T = eq::BandType;
        std::vector<std::pair<eq::BandSettings, bool>> lc {
            { band (T::Bell, 1000, 12, 1), true }, { band (T::Bell, 200, -6, 0.5f), true }, { band (T::LowShelf, 120, 6, 0.707f), true },
            { band (T::HighShelf, 8000, -6, 0.707f), true }, { band (T::LowCut, 40, 0, 0.707f, 3), true }, { band (T::HighCut, 10000, 0, 0.707f, 3), true },
            { band (T::Bell, 1000, 12, 8), false }, { band (T::LowCut, 100, 0, 0.707f, 7), false }, { band (T::BandPass, 1000, 0, 1, 5), false },
            { band (T::TiltShelf, 1000, 6, 0.707f), true } };
        struct J { std::vector<eq::BandSettings> bands; bool smooth; double fs; int res; int nf; std::string name; bool off; };
        std::vector<J> jobs;
        for (size_t c = 0; c < lc.size(); ++c)
            for (int res = 0; res <= 4; ++res)
                if (! gQuick || res == 1) jobs.push_back ({ { lc[c].first }, lc[c].second, 48000.0, res, 24, bandName (lc[c].first) + " res=" + std::to_string (res), false });
        for (size_t c : { 0, 4, 7 })
            for (double fs : { 44100.0, 88200.0, 96000.0, 192000.0 }) jobs.push_back ({ { lc[c].first }, lc[c].second, fs, 1, 24, bandName (lc[c].first) + " res=1", false });
        for (auto p : { eq::Placement::Mid, eq::Placement::Side, eq::Placement::Left, eq::Placement::Right })
        {
            auto b = lc[0].first;
            b.placement = p;
            const char* pn[] = { "Stereo", "Left", "Right", "Mid", "Side" };
            jobs.push_back ({ { b }, true, 48000.0, 1, 12, bandName (b) + " placement=" + pn[(int) p], false });
        }
        for (double fs : kRates) jobs.push_back ({ {}, true, fs, 1, 12, "all bands off", true });
        std::mutex pm;
        double p3worst = 0.0;
        std::string p3where;
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            eq::EqEngine E;
            E.prepare (j.fs, maxBlock);
            for (size_t b = 0; b < j.bands.size(); ++b) E.setBand ((int) b, j.bands[b]);
            E.setMode (2, j.res);
            for (int t = 0; t < 20000 && ! E.isLinearBuildReadyForTesting(); ++t) std::this_thread::sleep_for (std::chrono::milliseconds (2));
            std::vector<float> zl (256, 0.0f), zr (256, 0.0f);
            for (int t = 0; t < 100000 && ! E.isLinearUpToDate(); ++t)
            {
                E.process (zl.data(), zr.data(), 256);
                if (! E.isLinearUpToDate()) std::this_thread::sleep_for (std::chrono::microseconds (200));
            }
            const auto fr = buildFirRef (j.bands, j.res, j.fs);
            const int N = fr.N, lat = N / 2 + 256;
            rec (gLat, { j.fs, 0, 0, 0, j.name, -1 }, lat, E.getLatencyInSamples(), E.getLatencyInSamples() - lat, 0.0);
            for (int t = 0; t < (N + 512) / 256 + 1; ++t) E.process (zl.data(), zr.data(), 256);
            const double f0 = j.bands.empty() ? 1000.0 : j.bands[0].freq;
            const auto ks = j.nf == 24 ? denseBins (f0, j.fs, NA, gQuick ? 8 : 24) : logBins (30.0, 0.45 * j.fs, 12, j.fs, NA);
            const bool place = ! j.bands.empty() && j.bands[0].placement != eq::Placement::Stereo;
            std::vector<Casc> ideal;
            for (auto& b : j.bands) ideal.push_back (ref::design (b, j.fs));
            for (auto k : ks)
            {
                const int settle = N + 768;
                const int len = settle + NA;
                auto l = coherentSine (k, NA, 0.25, 0.0, len);
                std::vector<float> r = place ? std::vector<float> ((size_t) len, 0.0f) : l;
                const auto x = l;
                runBlocks ([&] (float* a, float* b, int n) { E.process (a, b, n); }, l.data(), r.data(), len, 512, maxBlock, i);
                const cd X = bin (x.data() + settle, NA, k), Lm = bin (l.data() + settle, NA, k) / X, Rm = bin (r.data() + settle, NA, k) / X;
                const double w = kTwoPi * (double) k / NA, f = k * j.fs / NA;
                const cd delay = std::polar (1.0, -w * 256.0);
                const cd He = firH (fr.ll, w) * delay;
                Ctx cx { j.fs, 512, f, 0.25, j.name, newSine() };
                if (j.off)
                {
                    const cd ideal_ = std::polar (1.0, -w * lat);
                    rec (gOff, cx, 1.0, std::abs (Lm), std::abs (Lm - ideal_), 1e-5);   // -100 dB relative
                    continue;
                }
                if (place)
                {
                    const cd Re = firH (fr.lr, w) * delay;
                    for (auto [meas, ex] : { std::pair<cd, cd> { Lm, He }, std::pair<cd, cd> { Rm, Re } })
                    {
                        if (dBc (ex) > -60.0) rec (gPl, cx, dBc (ex), dBc (meas), dBc (meas) - dBc (ex), 0.002);
                        else rec (gPla, cx, std::abs (ex), std::abs (meas), std::abs (meas - ex), 1e-5);
                    }
                    continue;
                }
                if (dBc (He) > -60.0)
                {
                    // 0.002 dB / 0.02 deg: float FFT convolution (~1e-6 relative)
                    rec (gM, cx, dBc (He), dBc (Lm), dBc (Lm) - dBc (He), 0.002);
                    rec (gP, cx, phaseDeg (He), phaseDeg (Lm), wrapDeg (phaseDeg (Lm) - phaseDeg (He)), 0.02);
                }
                const double hid = dBc (Hc (ideal[0], w));
                if (f >= 10.0 * j.fs / N)
                {
                    if (j.smooth) rec (gId, cx, hid, dBc (Lm), dBc (Lm) - hid, 0.1);
                    else rec (gIdI, cx, hid, dBc (Lm), dBc (Lm) - hid, -1.0);
                }
                if (dBc (Lm) > -20.0)
                {
                    const double lin = wrapDeg (phaseDeg (Lm * std::polar (1.0, w * lat)));
                    rec (gLin, cx, 0.0, lin, lin, 0.1);
                    const std::lock_guard<std::mutex> lk (pm);
                    if (std::abs (lin) > p3worst) { p3worst = std::abs (lin); p3where = where (cx); }
                }
            }
        });
        note ("P3 linear-phase FIR (window centred at (N-1)/2, impulse at N/2): largest phase deviation from linear (|H| > -20 dB) = "
              + num (p3worst, 4) + " deg at " + p3where + "; DC bin evaluated at 1 Hz.");
    }
}

//==============================================================================
// 5. Multiband saturator
sat::Settings satBase (int numBands, int slope, int os)
{
    sat::Settings s;
    s.enabled = true;
    s.numBands = numBands;
    s.slope = slope;
    s.oversampling = os;
    s.mix = 1.0f; s.outDb = 0.0f; s.inDb = 0.0f; s.channelMode = 0;
    for (auto& b : s.bands)
    {
        b.style = 12; b.driveDb = 0.0f; b.mix = 1.0f; b.feedback = 0.0f; b.dynamics = 0.0f; b.levelDb = 0.0f;
        b.bassDb = b.midDb = b.trebleDb = b.presenceDb = 0.0f; b.drivePan = b.levelPan = 0.0f;
        b.mute = b.solo = b.bypass = false;
    }
    return s;
}

std::unique_ptr<sat::MultibandSaturator> makeSat (double fs, const sat::Settings& s, int maxBlock)
{
    auto m = std::make_unique<sat::MultibandSaturator>();
    m->setSettings (s);
    m->prepare (fs, maxBlock);   // snaps everything (applySettings (true))
    return m;
}

/** Crossover all-pass of one split (my formulas): slope 0 identity, 1: LP1^2 - HP1^2, 2: LR4, 3: LR8. */
cd xoverAP (int slope, double fc, double rate, double th)
{
    switch (slope)
    {
        case 0: return 1.0;
        case 1: { const cd a = ref::lp1 (fc, rate).H (th), b = ref::hp1 (fc, rate).H (th); return a * a - b * b; }
        case 2: { const cd a = ref::lp2 (fc, 0.70710678, rate).H (th), b = ref::hp2 (fc, 0.70710678, rate).H (th); return a * a + b * b; }
        default:
        {
            const cd a = ref::lp2 (fc, 0.54119610, rate).H (th) * ref::lp2 (fc, 1.30656296, rate).H (th);
            const cd b = ref::hp2 (fc, 0.54119610, rate).H (th) * ref::hp2 (fc, 1.30656296, rate).H (th);
            return a * a + b * b;
        }
    }
}

std::array<float, 5> clampXovers (const std::array<float, 5>& x, double rate)
{
    std::array<float, 5> o {};
    float prev = 20.0f;
    for (int j = 0; j < 5; ++j) { o[(size_t) j] = juce::jlimit (prev * 1.05f, (float) (rate * 0.45), x[(size_t) j]); prev = o[(size_t) j]; }
    return o;
}

/** Memoryless saturation styles, double precision (documented formulas). x is already clamped to +-8. */
double satShape (int style, double x)
{
    switch (style)
    {
        case 0:  return std::tanh (x + 0.12 * x * x);
        case 2:  return std::tanh (1.5 * (std::tanh (2.0 * x + 0.3) - (double) 0.29131f));
        case 11: { const double c = std::min (1.0, std::max (-1.0, x)); return 1.5 * (c - c * c * c / 3.0); }
        case 12: return std::min (1.0, std::max (-1.0, x));
        case 13: { double t = (x + 1.0) / 4.0; t -= std::floor (t); return 1.0 - 4.0 * std::abs (t - 0.5); }
        case 14: return 1.2 * std::tanh (std::abs (x));
        case 15: return std::round (16.0 * std::min (1.0, std::max (-1.0, x))) / 16.0;
        default: return x;
    }
}

/** Stateful styles in double (one-poles z += a (x - z) with a = 1 - e^{-2 pi fc / rate}); Sub Fuzz's low-pass runs in
    float because its sign crossings drive the flip-flop (a double low-pass would flip at different samples). */
struct SatRef
{
    int style; double rate; int hold; float smear;
    double pre = 0, post = 0, post2 = 0, low = 0, held = 0, slew = 0;
    float lowF = 0.0f, lastLowF = 0.0f, flip = 1.0f, heldF = 0.0f;
    int holdCount = 0;
    double aPre = 0, aPost = 0, aPost2 = 0, aLow = 0;
    float aLowF = 0.0f;
    SatRef (int s, double r) : style (s), rate (r)
    {
        hold = std::max (1, (int) std::round (r / 8000.0));
        smear = (float) (0.02 * 48000.0 / r);
        auto a = [r] (double hz) { return (double) (float) (1.0 - std::exp (-kTwoPi * hz / r)); };
        switch (s)
        {
            case 1: aPost = a (12000); break;
            case 3: aPost = a (14000); break;
            case 4: aPost = a (8000); break;
            case 5: aPost = a (5000); aPost2 = a (7000); break;
            case 6: aLow = a (200); break;
            case 7: aLow = a (250); break;
            case 8: aPre = a (700); aPost = a (6000); break;
            case 9: aPre = a (700); aPost = a (5000); break;
            case 10: aPre = a (800); aPost = a (4500); aPost2 = a (4500); break;
            case 18: aLowF = (float) a (300); break;
            default: break;
        }
    }
    static double op (double& z, double a, double x) { z += a * (x - z); return z; }
    double step (float xf)
    {
        const double x = xf;
        switch (style)
        {
            case 1: return op (post, aPost, x >= 0 ? std::tanh (1.3 * x) : std::tanh (0.8 * x));
            case 3: { const double c = std::min (1.0, std::max (-1.0, x)); return op (post, aPost, 1.5 * (c - c * c * c / 3.0)); }
            case 4: return op (post, aPost, (double) 0.6366f * std::atan (1.8 * x));
            case 5: return op (post2, aPost2, op (post, aPost, (double) 0.6366f * std::atan (2.5 * x)));
            case 6: { const double lo = op (low, aLow, x); return std::tanh (x - lo) + std::tanh (1.6 * lo) / 1.6; }
            case 7: { const double lo = op (low, aLow, x); return std::tanh (0.9 * (x - lo) + std::tanh (2.5 * lo + 0.1) - (double) 0.0997f); }
            case 8: { const double pr = x - op (pre, aPre, x); return op (post, aPost, std::tanh (1.5 * (x + 0.6 * pr))); }
            case 9: { const double v = 3.0 * (x + 0.8 * (x - op (pre, aPre, x))); return op (post, aPost, v >= 0 ? std::tanh (v) : -0.85 * std::tanh (-0.8 * v)); }
            case 10: { const double v = 6.0 * (x + (x - op (pre, aPre, x))); return op (post2, aPost2, op (post, aPost, std::tanh (2.5 * std::tanh (v)))); }
            case 16: if (holdCount-- <= 0) { held = x; holdCount = hold - 1; } return std::tanh (held);
            case 17: slew += std::min ((double) smear, std::max (-(double) smear, x - slew)); return std::tanh (1.2 * slew);
            case 19:
            {
                if (holdCount-- <= 0) { heldF = xf; holdCount = 2 * hold - 1; }
                const float c = juce::jlimit (-1.0f, 1.0f, heldF * 1.5f);
                return std::round (c * 6.0f) / 6.0f;
            }
            case 18:
            {
                lowF += aLowF * (xf - lowF);
                if (lastLowF < 0.0f && lowF >= 0.0f) flip = -flip;
                lastLowF = lowF;
                const double fuzz = (x >= 0 ? 1.0 : -1.0) * (1.0 - std::exp (-4.0 * std::abs (x)));
                return 0.6 * fuzz + 0.5 * std::tanh (3.0 * std::abs ((double) lowF)) * flip;
            }
            default: return satShape (style, x);
        }
    }
};

void suiteSat()
{
    auto& su = S ("sat");
    const int maxBlock = 2048;

    //--------------------------------------------------------------------------
    // 5a crossover sum = all-pass (every band bypassed)
    {
        auto& gM = su.g ("5a.xover_sum.mag", "dB");
        auto& gP = su.g ("5a.xover_sum.phase", "deg");
        auto& gA = su.g ("5a.xover_sum.below-40dB", "abs");
        auto& gS0 = su.g ("5a.slope0_osOff_per_sample_vs_DC(x)", "ulp");
        auto& gMix = su.g ("5a.global_mix_no_comb", "dB");
        struct J { int slope, nb, os; double fs; long long k; float mix; int blk; };
        std::vector<J> jobs;
        int idx = 0;
        for (int slope = 0; slope < 4; ++slope)
            for (int nb = 1; nb <= 6; ++nb)
            {
                for (int os : { 0, 1 })
                    for (double fs : kRates)
                        for (auto k : logBins (10.0, 0.45 * fs, gQuick ? 4 : 24, fs, NA)) jobs.push_back ({ slope, nb, os, fs, k, 1.0f, blockFor (idx++, maxBlock) });
                for (int os : { 2, 3 })
                    for (auto k : logBins (10.0, 0.45 * 48000.0, gQuick ? 4 : 24, 48000.0, NA)) jobs.push_back ({ slope, nb, os, 48000.0, k, 1.0f, blockFor (idx++, maxBlock) });
            }
        for (float mix : { 0.0f, 0.3f })
            for (auto k : logBins (10.0, 0.45 * 48000.0, 24, 48000.0, NA)) jobs.push_back ({ 2, 3, 0, 48000.0, k, mix, 512 });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = satBase (j.nb, j.slope, j.os);
            s.mix = j.mix;
            for (auto& b : s.bands) b.bypass = true;
            auto m = makeSat (j.fs, s, maxBlock);
            const double R = dcRfor (j.fs);
            const int settle = std::max ((int) (0.25 * j.fs), (int) (20.0 / -std::log (R)));
            const bool perSample = j.slope == 0 && j.os == 0 && j.mix == 1.0f;
            const auto r = measureStereo ([&] (float* l, float* rr, int n) { m->process (l, rr, n); }, j.k, 0.5, settle, j.blk, maxBlock, i, perSample);
            const double rate = j.fs * (1 << j.os);
            const auto xf = clampXovers (s.crossovers, rate);
            const int slope = j.slope, nb = j.nb;
            const RespFn inner = [=] (double th) { cd h = 1.0; for (int q = 0; q < nb - 1; ++q) h *= xoverAP (slope, xf[(size_t) q], rate, th); return h; };
            const double w = kTwoPi * (double) j.k / NA;
            const cd He = (j.os > 0 ? osG (osStages (j.os), w, inner) : inner (w)) * dcHP (w, R);
            Ctx c { j.fs, j.blk, j.k * j.fs / NA, 0.5, "slope=" + std::to_string (j.slope) + " bands=" + std::to_string (j.nb) + " os=" + std::to_string (1 << j.os)
                    + "x mix=" + num (j.mix), newSine() };
            if (j.mix != 1.0f) { rec (gMix, c, dBc (He), dBc (r.L), dBc (r.L) - dBc (He), 0.001); return; }
            // 0.001 dB / 0.02 deg: double biquads, float rounding at each stage output
            checkH (gM, gP, gA, c, r.L, He, 0.001, 0.02, -40.0, 1e-5);
            if (perSample)
            {
                float dx = 0.0f, dy = 0.0f;
                const float Rf = (float) R;
                double worst = 0.0, peak = 0.0;
                for (size_t n = 0; n < r.x.size(); ++n)
                {
                    dy = r.x[n] - dx + Rf * dy;
                    dx = r.x[n];
                    worst = std::max (worst, std::abs ((double) r.l[n] - dy));
                    peak = std::max (peak, std::abs ((double) r.x[n]));
                }
                rec (gS0, c, 0, worst / ulpOf (peak), worst / ulpOf (peak), 4.0);
            }
        });
    }

    //--------------------------------------------------------------------------
    // 5b styles: per-sample against DC_HP (f (clamp (x d, +-8)) / sqrt d), DC recursion mirrored in float
    {
        auto& gMl = su.g ("5b.memoryless_styles_per_sample", "abs");
        auto& gFl = su.g ("5b.filtered_styles_per_sample", "abs");
        auto& gSub = su.g ("5b.subfuzz_octave_down_peak_bin", "bins");
        tolNote ("sat 5b: the DC high-pass is mirrored in float in the reference (its float recursion has a noise gain of "
                 "1/(1-R) ~ 764 at 48 kHz, so a double reference would differ by more than the plan's 1e-6 for reasons unrelated to the styles). Fixed before the first run.");
        struct J { int style; double fs, A; long long k; float drv; bool memoryless; };
        std::vector<J> jobs;
        int cnt = 0;
        for (double fs : kRates)
        {
            for (int st : { 0, 2, 11, 12, 13, 14, 15 })
                for (double A : { 0.05, 0.2, 0.5, 1.0, 2.0, 4.0 })
                    for (auto k : logBins (40.0, 0.4 * fs, 6, fs, 8192))
                        for (float drv : { 0.0f, 12.0f, 36.0f })
                            if ((fs == 48000.0 || ++cnt % 4 == 0) && (! gQuick || ++cnt % 8 == 0)) jobs.push_back ({ st, fs, A, k, drv, true });
            for (int st : { 1, 3, 4, 5, 6, 7, 8, 9, 10, 16, 17, 18, 19 })
                for (double A : { 0.1, 0.5, 2.0 })
                    for (auto k : logBins (40.0, 0.4 * fs, 4, fs, 8192))
                        for (float drv : { 0.0f, 12.0f })
                            if ((fs == 48000.0 || ++cnt % 4 == 0) && (! gQuick || ++cnt % 8 == 0)) jobs.push_back ({ st, fs, A, k, drv, false });
        }
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = satBase (1, 2, 0);
            s.bands[0].style = j.style;
            s.bands[0].driveDb = j.drv;
            auto m = makeSat (j.fs, s, maxBlock);
            const int len = 8192, blk = blockFor (i, maxBlock);
            auto l = coherentSine (j.k, 8192, j.A, 0.0, len), r = l;
            const auto x = l;
            runBlocks ([&] (float* a, float* b, int n) { m->process (a, b, n); }, l.data(), r.data(), len, blk, maxBlock, i);
            const float d = juce::Decibels::decibelsToGain (j.drv), invD = 1.0f / std::sqrt (d), Rf = dcRfor (j.fs);
            SatRef refS (j.style, j.fs);
            float dx = 0.0f, dy = 0.0f;
            double worst = 0.0, ratio = -1.0, wTol = 1.0;
            for (int n = 0; n < len; ++n)
            {
                const float xin = juce::jlimit (-8.0f, 8.0f, x[(size_t) n] * d + 0.0f);
                const double w = refS.step (xin);
                const float e = (float) (w * (double) invD);
                dy = e - dx + Rf * dy;
                dx = e;
                const double tol = (j.memoryless ? 1e-6 : 1e-5) + 4.0 * ulpOf (dy);
                const double err = std::abs ((double) l[(size_t) n] - dy);
                if (err / tol > ratio) { ratio = err / tol; worst = err; wTol = tol; }
            }
            Ctx c { j.fs, blk, j.k * j.fs / 8192, j.A, "style=" + std::string (sat::styles()[(size_t) j.style].name) + " drive=" + num (j.drv), newSine() };
            rec (j.memoryless ? gMl : gFl, c, 0, worst, worst, wTol);
        });
        // Sub Fuzz: the flip-flop component sits at f0 / 2
        for (double fs : { 48000.0, 96000.0 })
            for (double f : { 80.0, 200.0, 500.0 })
            {
                auto s = satBase (1, 2, 0);
                s.bands[0].style = 18;
                auto m = makeSat (fs, s, 512);
                const long long k = 2 * oddBin (f / 2, fs, NA);
                const int len = 16384 + NA;
                auto l = coherentSine (k, NA, 0.5, 0.0, len), r = l;
                runBlocks ([&] (float* a, float* b, int n) { m->process (a, b, n); }, l.data(), r.data(), len, 512, 512);
                std::vector<cd> Y ((size_t) NA);
                for (int n = 0; n < NA; ++n) Y[(size_t) n] = l[(size_t) (16384 + n)];
                fft (Y, false);
                long long best = 0;
                double bv = -1;
                for (long long b = (long long) (5.0 * NA / fs) + 1; b < k; ++b) if (std::abs (Y[(size_t) b]) > bv) { bv = std::abs (Y[(size_t) b]); best = b; }
                rec (gSub, { fs, 512, k * fs / NA, 0.5, "peak below f0 at " + num (best * fs / NA, 6) + " Hz", newSine() }, (double) k / 2, (double) best, (double) best - k / 2.0, 1.0);
            }
    }

    //--------------------------------------------------------------------------
    // 5b linear region: tone controls, drive / level pan, mid/side
    {
        auto& gM = su.g ("5b.tone_linear_region.mag", "dB");
        auto& gP = su.g ("5b.tone_linear_region.phase", "deg");
        auto& gA = su.g ("5b.tone_linear_region.below-40dB", "abs");
        auto& gPan = su.g ("5b.pan_ms_linear_region", "rel");
        struct J { double fs; int ctl; float g; long long k; };
        std::vector<J> jobs;
        for (double fs : kRates) for (int ctl = 0; ctl < 4; ++ctl) for (float g : { -12.0f, 12.0f })
            for (auto k : logBins (10.0, 0.45 * fs, gQuick ? 6 : 24, fs, NA)) jobs.push_back ({ fs, ctl, g, k });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = satBase (1, 2, 0);
            s.bands[0].style = 12;
            s.bands[0].driveDb = 6.0f;
            float* t[] = { &s.bands[0].bassDb, &s.bands[0].midDb, &s.bands[0].trebleDb, &s.bands[0].presenceDb };
            *t[j.ctl] = j.g;
            auto m = makeSat (j.fs, s, maxBlock);
            const double R = dcRfor (j.fs);
            const int settle = std::max ((int) (0.25 * j.fs), (int) (20.0 / -std::log (R)));
            const auto r = measureStereo ([&] (float* a, float* b, int n) { m->process (a, b, n); }, j.k, 0.25, settle, 512, maxBlock, i);
            const float d = juce::Decibels::decibelsToGain (6.0f);
            const double gain = (double) d * (double) (1.0f / std::sqrt (d));
            Biq tone;
            switch (j.ctl)
            {
                case 0: tone = ref::lowShelf (120.0, j.g, 0.7, j.fs); break;
                case 1: tone = ref::bell (800.0, j.g, (double) 0.7f, j.fs); break;
                case 2: tone = ref::highShelf (5000.0, j.g, 0.7, j.fs); break;
                default: tone = ref::bell (3000.0, j.g, (double) 1.2f, j.fs); break;
            }
            const double w = kTwoPi * (double) j.k / NA;
            const cd He = gain * tone.H (w) * dcHP (w, R);
            const char* names[] = { "bass", "mid", "treble", "presence" };
            checkH (gM, gP, gA, { j.fs, 512, j.k * j.fs / NA, 0.25, std::string (names[j.ctl]) + "=" + num (j.g), newSine() }, r.L, He, 0.002, 0.02, -40.0, 1e-5);
        });
        note ("sat: in the linear region the band gain is drive / sqrt(drive) = +drive/2 dB (the level compensation is 1/sqrt(drive)).");
        struct P { int kind; float v; };
        std::vector<P> ps { { 0, -1.0f }, { 0, -0.5f }, { 0, 0.5f }, { 0, 1.0f }, { 1, -1.0f }, { 1, -0.5f }, { 1, 0.5f }, { 1, 1.0f }, { 2, 0.0f }, { 2, 0.5f } };
        parallelFor ((int) ps.size(), [&] (int i)
        {
            const auto& p = ps[(size_t) i];
            const double fs = 48000.0;
            auto s = satBase (1, 2, 0);
            s.bands[0].style = 12;
            s.bands[0].driveDb = 6.0f;
            if (p.kind == 0) s.bands[0].drivePan = p.v;
            if (p.kind == 1) s.bands[0].levelPan = p.v;
            if (p.kind == 2) { s.channelMode = 1; s.bands[0].drivePan = p.v; }
            auto m = makeSat (fs, s, 512);
            const long long k = oddBin (1000.0, fs, NA);
            const int settle = (int) (0.3 * fs), len = settle + NA;
            auto l = coherentSine (k, NA, 0.1, 0.0, len);
            auto r = p.kind == 2 ? std::vector<float> ((size_t) len, 0.0f) : l;
            const auto x = l;
            runBlocks ([&] (float* a, float* b, int n) { m->process (a, b, n); }, l.data(), r.data(), len, 512, 512);
            const cd X = bin (x.data() + settle, NA, k), Lm = bin (l.data() + settle, NA, k) / X, Rm = bin (r.data() + settle, NA, k) / X;
            const double w = kTwoPi * (double) k / NA;
            const cd dc = dcHP (w, dcRfor (fs));
            const float d = juce::Decibels::decibelsToGain (6.0f);
            const float panDb = 6.0f * s.bands[0].drivePan;
            const double cd0 = (double) d * juce::Decibels::decibelsToGain (-panDb), cd1 = (double) d * juce::Decibels::decibelsToGain (panDb);
            cd Le, Re;
            if (p.kind == 0) { Le = std::sqrt (cd0); Re = std::sqrt (cd1); }
            else if (p.kind == 1) { Le = std::sqrt ((double) d) * (p.v > 0 ? 1.0 - p.v : 1.0); Re = std::sqrt ((double) d) * (p.v < 0 ? 1.0 + p.v : 1.0); }
            else { const double mm = 0.5 * std::sqrt (cd0), ss = 0.5 * std::sqrt (cd1); Le = mm + ss; Re = mm - ss; }
            Le *= dc; Re *= dc;
            const char* kinds[] = { "drivePan", "levelPan", "M/S drivePan" };
            Ctx c { fs, 512, k * fs / NA, 0.1, std::string (kinds[p.kind]) + "=" + num (p.v), newSine() };
            rec (gPan, c, std::abs (Le), std::abs (Lm), std::abs (Lm - Le) / std::max (std::abs (Le), 1e-3), 1e-6);
            rec (gPan, c, std::abs (Re), std::abs (Rm), std::abs (Rm - Re) / std::max (std::abs (Re), 1e-3), 1e-6);
        });
        notDone ("sat feedback band-pass coefficient check (1e-12): the coefficients are private and not observable in isolation through process() "
                 "(closed loop); covered by the stability checks only.");
    }
}

//==============================================================================
// 6. Multiband compressor
double gcRef (double x, const comp::BandSettings& b, int style, bool limit)
{
    double R = std::max (1.0, (double) b.ratio), W = std::max (0.0, (double) b.kneeDb);
    if (style == comp::Smooth || style == comp::Opto) W += 6.0;
    if (style == comp::T4Opto) { R = limit ? 12.0 : 3.0; W = limit ? 4.0 : 14.0; }
    const double T = b.thresholdDb;
    if (style == comp::VariMu) R = 1.0 + (R - 1.0) * std::min (2.5, std::max (0.2, 1.0 + (x - T) / 18.0));
    const double s = 1.0 - 1.0 / R;
    const int dir = style == comp::TTM ? 2 : b.direction;
    double down = 0.0, up = 0.0;
    if (dir != 1)
    {
        const double d = x - T;
        if (W > 0 && std::abs (d) <= W / 2) down = -s * (d + W / 2) * (d + W / 2) / (2 * W);
        else if (d > 0) down = -s * d;
    }
    if (dir != 0)
    {
        const double d = T - x;
        if (W > 0 && std::abs (d) <= W / 2) up = s * (d + W / 2) * (d + W / 2) / (2 * W);
        else if (d > 0) up = s * d;
        up *= std::min (1.0, std::max (0.0, (x + 80.0) / 20.0));
    }
    const double range = std::max (0.0, (double) b.rangeDb);
    return std::min (range, std::max (-range, down + up));
}
double makeupRef (const comp::BandSettings& b, int style, bool limit)
{
    const double g = gcRef (-6.0, b, style, limit);
    return std::min (24.0, std::max (-24.0, g < 0 ? -0.5 * g : -0.5 * gcRef (-36.0, b, style, limit) * 0.5));
}
bool peakStyle (int s) { return s == comp::Clean || s == comp::Mastering || s == comp::TTM || s == comp::Pumping || s == comp::Classic; }
bool feedbackStyle (int s) { return s == comp::Classic || s == comp::VariMu; }

comp::Settings compBase (int style, float thr, float ratio, float knee)
{
    comp::Settings s;
    s.enabled = true; s.numBands = 1; s.style = style; s.lookahead = 0; s.holdMs = 0.0f; s.scHighPass = 20.0f;
    s.mix = 1.0f; s.outDb = 0.0f; s.autoGain = false; s.limit = false;
    for (auto& b : s.bands) { b.direction = 0; b.thresholdDb = thr; b.ratio = ratio; b.kneeDb = knee; b.rangeDb = 30.0f; b.attackMs = 10.0f; b.releaseMs = 120.0f; b.gainDb = 0.0f; }
    return s;
}

/** Output / input gain (dB) of a steady sine after settleS seconds, measured over a coherent NA-sample window. */
cd compH (const comp::Settings& s, double fs, long long k, double A, double settleS, int blk = 512, std::vector<float>* keep = nullptr)
{
    auto c = std::make_unique<comp::MultibandCompressor>();
    c->setSettings (s);
    c->prepare (fs);   // prepare snaps from the stored settings
    const int settle = (int) (settleS * fs), len = settle + NA;
    auto l = coherentSine (k, NA, A, 0.0, len), r = l;
    const auto x = l;
    runBlocks ([&] (float* a, float* b, int n) { c->process (a, b, n); }, l.data(), r.data(), len, blk, 4096);
    if (keep) *keep = l;
    return bin (l.data() + settle, NA, k) / bin (x.data() + settle, NA, k);
}

/** [min, max] of the expected static gain for detector levels in [x - dLo, x + dHi]; feedback styles solve
    G = gc (x + G + e) (detector on the output) for each e. */
std::pair<double, double> gcInterval (double x, double dLo, double dHi, const comp::BandSettings& b, int style, bool limit, double makeup = 0.0)
{
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i <= 64; ++i)
    {
        const double e = -dLo + (dLo + dHi) * i / 64.0;
        double G;
        if (feedbackStyle (style))
        {
            double a = -200, z = 200;
            for (int it = 0; it < 100; ++it) { const double mid = 0.5 * (a + z); if (mid - gcRef (x + mid + makeup + e, b, style, limit) > 0) z = mid; else a = mid; }
            G = 0.5 * (a + z);
        }
        else G = gcRef (x + e, b, style, limit);
        lo = std::min (lo, G); hi = std::max (hi, G);
    }
    return { lo, hi };
}

void suiteComp()
{
    auto& su = S ("comp");
    //--------------------------------------------------------------------------
    // 6a static formula, then the measured static curve
    {
        auto& g = su.g ("6a.gainComputer_vs_formula", "dB");
        auto& gm = su.g ("6a.autoMakeup_vs_formula", "dB");
        tolNote ("comp 6a: gainComputer() is evaluated in float; tolerance 1e-5 dB + 8 ulp of the largest operand (|x|, |T|, knee, range). "
                 "Peak-style steady-state intervals include the sample-peak deficit of a sampled sine, 20 log10 cos(pi f0 / fs) "
                 "(-0.30 dB at 4 kHz / 48 kHz), which the plan's 0.06 dB ripple allowance did not include. Fixed before the first run.");
        std::mt19937 rng (12345);
        std::uniform_real_distribution<float> U (0.0f, 1.0f);
        double worst = 0.0, wtol = 1.0, ratio = -1.0;
        std::string wh;
        for (int i = 0; i < (gQuick ? 10000 : 100000); ++i)
        {
            comp::BandSettings b;
            b.direction = (int) (U (rng) * 3) % 3;
            b.thresholdDb = -60.0f * U (rng);
            b.ratio = 1.0f + 99.0f * U (rng) * U (rng);
            b.kneeDb = U (rng) < 0.2f ? 0.0f : 24.0f * U (rng);
            b.rangeDb = 60.0f * U (rng);
            const int style = (int) (U (rng) * comp::numStyles) % comp::numStyles;
            const bool limit = U (rng) < 0.5f;
            const float x = -100.0f + 130.0f * U (rng);
            const double a = comp::gainComputer (x, b, style, limit), e = gcRef (x, b, style, limit);
            const double tol = 1e-5 + 8.0 * ulpOf (std::max ({ std::abs ((double) x), std::abs ((double) b.thresholdDb), (double) b.kneeDb + 6.0, (double) b.rangeDb }));
            if (std::abs (a - e) / tol > ratio) { ratio = std::abs (a - e) / tol; worst = a - e; wtol = tol; wh = "x=" + num (x) + " style=" + std::to_string (style); }
            if (i % 1000 == 0)
            {
                const double ma = comp::autoMakeup (b, style, limit), me = makeupRef (b, style, limit);
                rec (gm, { 0, 0, 0, 0, "style=" + std::to_string (style), -1 }, me, ma, ma - me, 1e-5 + 8.0 * ulpOf (24.0));
            }
        }
        rec (g, { 0, 0, 0, 0, std::to_string (gQuick ? 10000 : 100000) + " random points, worst at " + wh, -1 }, 0, worst, worst, wtol);
    }
    {
        auto& gS = su.g ("6a.static_curve_measured", "dB");
        auto& gF = su.g ("6a.static_curve_feedback_styles(fixed point)", "dB");
        auto& gP4 = su.g ("6a.P4_feedback_shift_vs_feedforward(info)", "dB");
        struct J { int style; float thr, ratio, knee; int dir; float range; double lvl; };
        std::vector<J> jobs;
        std::vector<double> levels;
        for (double l = -60; l <= 24; l += (gQuick ? 18 : 6)) levels.push_back (l);
        for (int style : { (int) comp::Clean, (int) comp::Punch })
            for (float ratio : { 1.0f, 1.5f, 2.0f, 4.0f, 8.0f, 20.0f, 100.0f })
                for (float knee : { 0.0f, 6.0f, 24.0f })
                    for (float thr : { -40.0f, -18.0f, -6.0f })
                        for (double lv : levels) jobs.push_back ({ style, thr, ratio, knee, 0, 30.0f, lv });
        for (int dir : { 1, 2 }) for (float knee : { 0.0f, 6.0f, 24.0f }) for (double lv : levels) jobs.push_back ({ comp::Clean, -18.0f, 4.0f, knee, dir, 30.0f, lv });
        for (float range : { 6.0f, 30.0f }) for (double lv : levels) jobs.push_back ({ comp::Clean, -40.0f, 100.0f, 0.0f, 0, range, lv });
        for (int style = 0; style < comp::numStyles; ++style) for (double lv : levels) jobs.push_back ({ style, -18.0f, 4.0f, 6.0f, 0, 30.0f, lv });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            const double fs = 48000.0;
            auto s = compBase (j.style, j.thr, j.ratio, j.knee);
            s.bands[0].direction = j.dir;
            s.bands[0].rangeDb = j.range;
            const bool pk = peakStyle (j.style);
            const double f0 = pk ? 4000.0 : 1000.0;
            const long long k = oddBin (f0, fs, NA);
            const double A = std::pow (10.0, j.lvl / 20.0);
            const cd H = compH (s, fs, k, A, 3.0);
            const double G = dBc (H);
            const double deficit = pk ? -dB (std::cos (kPi * k / NA)) : 0.0;
            const auto iv = gcInterval (j.lvl, 0.06 + deficit, pk ? 0.0 : 0.06, s.bands[0], j.style, false);
            const double err = G < iv.first ? G - iv.first : (G > iv.second ? G - iv.second : 0.0);
            Ctx c { fs, 512, k * fs / NA, A, std::string ("style=") + comp::styleNames()[j.style].toStdString() + " thr=" + num (j.thr) + " ratio=" + num (j.ratio)
                    + " knee=" + num (j.knee) + " dir=" + std::to_string (j.dir) + " range=" + num (j.range) + " level=" + num (j.lvl) + "dBFS interval=["
                    + num (iv.first, 5) + "," + num (iv.second, 5) + "]", newSine() };
            // +-0.05 dB around the interval (peak ripple ~0.054 dB at 4 kHz, RMS 2f ripple < 0.03 dB at 1 kHz); feedback styles 0.1 dB
            rec (feedbackStyle (j.style) ? gF : gS, c, gcRef (j.lvl, s.bands[0], j.style, false), G, err, feedbackStyle (j.style) ? 0.1 : 0.05);
            if (feedbackStyle (j.style))
            {
                const double ff = gcRef (j.lvl, s.bands[0], j.style, false);
                rec (gP4, c, ff, G, G - ff, -1.0);
            }
        });
    }
    {
        auto& g = su.g ("6a.auto_makeup_output_level", "dB");
        std::vector<std::pair<float, float>> tk;
        for (float thr : { -30.0f, -18.0f, -6.0f }) for (float knee : { 0.0f, 6.0f }) tk.push_back ({ thr, knee });
        parallelFor ((int) tk.size(), [&] (int i)
        {
            const double fs = 48000.0;
            auto s = compBase (comp::Clean, tk[(size_t) i].first, 4.0f, tk[(size_t) i].second);
            s.autoGain = true;
            const long long k = oddBin (4000.0, fs, NA);
            const double A = 0.5, x = dB (A);
            const cd H = compH (s, fs, k, A, 3.0);
            const double M = makeupRef (s.bands[0], comp::Clean, false);
            const auto iv = gcInterval (x, 0.06 - dB (std::cos (kPi * k / NA)), 0.0, s.bands[0], comp::Clean, false);
            const double G = dBc (H) - M;
            const double err = G < iv.first ? G - iv.first : (G > iv.second ? G - iv.second : 0.0);
            rec (g, { fs, 512, k * fs / NA, A, "thr=" + num (s.bands[0].thresholdDb) + " knee=" + num (s.bands[0].kneeDb) + " makeup=" + num (M, 5), newSine() },
                 iv.first + M, dBc (H), err, 0.05);
        });
    }
    //--------------------------------------------------------------------------
    // 6b timing: attack / release time constants from the exact per-sample gain y / x
    {
        auto& gAt = su.g ("6b.attack_tau", "rel");
        auto& gRe = su.g ("6b.release_tau(memory~0)", "rel");
        struct J { int style; float att, rel; double fs; bool attack; };
        std::vector<J> jobs;
        for (double fs : { 48000.0, 96000.0 })
        {
            for (int style : { (int) comp::Clean, (int) comp::Mastering }) for (float att : { 1.0f, 10.0f, 50.0f }) jobs.push_back ({ style, att, 120.0f, fs, true });
            for (float rel : { 50.0f, 120.0f, 400.0f }) jobs.push_back ({ comp::Clean, 10.0f, rel, fs, false });
        }
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            // Review fix: the release check claims memory ~ 0, but memory grows while |GR| > 2 dB (Compressor.h), including during
            // the release itself, so with a -18 dB threshold (13.5 dB GR) it reached 0.1..0.5 and the slow (2 x rel) branch set the
            // tail (tau ~ 1.6 x rel). A -2 dB threshold keeps GR at 1.5 dB < 2 dB, so memory stays exactly 0 and tau = 0.5 x rel.
            auto s = compBase (j.style, j.attack ? -18.0f : -2.0f, 4.0f, 0.0f);
            s.bands[0].attackMs = j.att;
            s.bands[0].releaseMs = j.rel;
            auto c = std::make_unique<comp::MultibandCompressor>();
            c->setSettings (s);
            c->prepare (j.fs);
            // Review fix: a sine at exactly fs/4 (k = NA/4, phase 0) has sample peaks of exactly +-1 every other sample, so the
            // peak detector sees a constant level; the 3999.76 Hz sine used before beat against the sample grid (sample peak
            // 0.966..1.0 drifting over the burst) and biased the attack fit by +5.8 % at 48 kHz (reproduced in a double model).
            const long long k = NA / 4;
            const int pre = (int) (0.5 * j.fs), burst = j.attack ? (int) ((10.0 * j.att + 50.0) * 0.001 * j.fs) : (int) (0.02 * j.fs), post = (int) ((j.attack ? 0.1 : 6.0 * j.rel * 0.001) * j.fs);
            const int len = pre + burst + post;
            auto x = coherentSine (k, NA, 1.0, 0.0, len);
            for (int n = 0; n < len; ++n) if (n < pre || n >= pre + burst) x[(size_t) n] *= 0.01f;
            auto l = x, r = x;
            runBlocks ([&] (float* a, float* b, int n) { c->process (a, b, n); }, l.data(), r.data(), len, 256, 4096);
            // gain trajectory (dB) on samples with |x| > 0.3 peak
            const int a0 = j.attack ? pre : pre + burst, a1 = j.attack ? pre + burst : len;
            std::vector<std::pair<double, double>> pts;
            for (int n = a0; n < a1; ++n)
            {
                const double peak = (n < pre || n >= pre + burst) ? 0.01 : 1.0;
                if (std::abs (x[(size_t) n]) > 0.3 * peak) pts.push_back ({ (double) (n - a0), dB (l[(size_t) n] / x[(size_t) n]) });
            }
            double fin = 0.0;
            int cnt = 0;
            for (size_t q = pts.size() * 9 / 10; q < pts.size(); ++q) { fin += pts[q].second; ++cnt; }
            fin /= std::max (1, cnt);
            const double g0 = pts.front().second;
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int n = 0;
            for (auto& [t, g] : pts)
            {
                const double rel = (g - fin) / (g0 - fin);
                if (rel < 0.1 || rel > 0.9) continue;
                const double y = std::log (rel);
                sx += t; sy += y; sxx += t * t; sxy += t * y; ++n;
            }
            const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
            const double tauMs = -1.0 / slope / j.fs * 1000.0;
            const double expMs = j.attack ? (j.style == comp::Mastering ? std::max (j.att, 0.5f) : j.att) : 0.5 * j.rel;
            Ctx cx { j.fs, 256, k * j.fs / NA, 1.0, std::string ("style=") + comp::styleNames()[j.style].toStdString() + (j.attack ? " att=" + num (j.att) : " rel=" + num (j.rel))
                     + " tau_ms=" + num (tauMs, 5), newSine() };
            // +-5 %: the residual detector smoothing adds about one half-period
            rec (j.attack ? gAt : gRe, cx, expMs, tauMs, tauMs / expMs - 1.0, 0.05);
        });
    }
    //--------------------------------------------------------------------------
    // 6b lookahead: single band, ratio 1 = an exact delay of round (ms fs / 1000) samples
    {
        auto& gB = su.g ("6b.lookahead_exact_delay", "abs");
        auto& gL = su.g ("6b.lookahead_reported_latency", "samples");
        for (double fs : kRates)
            for (int la = 0; la < 5; ++la)
            {
                auto s = compBase (comp::Clean, -18.0f, 1.0f, 0.0f);
                s.lookahead = la;
                auto c = std::make_unique<comp::MultibandCompressor>();
                c->setSettings (s);
                c->prepare (fs);
                const int D = (int) std::lround (comp::lookaheadMs (la) * 0.001 * fs);
                const int len = (int) (0.2 * fs);
                auto l = coherentSine (oddBin (1000.0, fs, NA), NA, 0.5, 0.0, len), r = l;
                const auto x = l;
                runBlocks ([&] (float* a, float* b, int n) { c->process (a, b, n); }, l.data(), r.data(), len, 333, 4096);
                double worst = 0.0;
                for (int n = D; n < len; ++n) worst = std::max (worst, std::abs ((double) l[(size_t) n] - x[(size_t) (n - D)]));
                Ctx cx { fs, 333, 1000, 0.5, "lookahead=" + comp::lookaheadNames()[la].toStdString(), newSine() };
                rec (gB, cx, 0, worst, worst, 0.0);
                rec (gL, cx, D, c->getLatencyInSamples(), c->getLatencyInSamples() - D, 0.0);
            }
    }
    //--------------------------------------------------------------------------
    // 6c ratio 1: flat (product of LR4 all-passes) for every mix; mix with gain reduction = 1 - m + m g
    {
        auto& gM = su.g ("6c.ratio1_flat.mag", "dB");
        auto& gP = su.g ("6c.ratio1_flat.phase", "deg");
        auto& gA = su.g ("6c.ratio1_flat.below-40dB", "abs");
        auto& gX = su.g ("6c.mix_with_gr", "dB");
        struct J { int nb; double fs; float mix; long long k; };
        std::vector<J> jobs;
        for (int nb = 1; nb <= 4; ++nb) for (double fs : kRates) for (float mix : { 0.0f, 0.5f, 1.0f })
            for (auto k : logBins (10.0, 0.45 * fs, gQuick ? 4 : 24, fs, NA)) jobs.push_back ({ nb, fs, mix, k });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            auto s = compBase (comp::Clean, -18.0f, 1.0f, 6.0f);
            s.numBands = j.nb;
            s.mix = j.mix;
            const cd H = compH (s, j.fs, j.k, 0.5, 0.25, blockFor (i, 4096));
            std::array<float, 5> xs { s.crossovers[0], s.crossovers[1], s.crossovers[2], 20000.0f, 20000.0f };
            const auto xf = clampXovers (xs, j.fs);
            const double w = kTwoPi * (double) j.k / NA;
            cd He = 1.0;
            for (int q = 0; q < j.nb - 1; ++q) He *= xoverAP (2, xf[(size_t) q], j.fs, w);
            checkH (gM, gP, gA, { j.fs, blockFor (i, 4096), j.k * j.fs / NA, 0.5, "bands=" + std::to_string (j.nb) + " mix=" + num (j.mix), newSine() }, H, He, 0.001, 0.02, -40.0, 1e-5);
        });
        parallelFor (9, [&] (int i)
        {
            const double fs = 48000.0, A = std::pow (10.0, (-12.0 + 6.0 * (i % 3)) / 20.0);
            const float mix = 0.25f * (1 + i / 3);
            const long long k = oddBin (4000.0, fs, NA);
            auto s = compBase (comp::Clean, -18.0f, 4.0f, 0.0f);
            const cd g1 = compH (s, fs, k, A, 3.0);
            s.mix = mix;
            const cd gm = compH (s, fs, k, A, 3.0);
            const double ex = dB (std::abs (1.0 - (double) mix + (double) mix * g1));
            rec (gX, { fs, 512, k * fs / NA, A, "mix=" + num (mix) + " g(mix 1)=" + num (dBc (g1), 5) + "dB", newSine() }, ex, dBc (gm), dBc (gm) - ex, 0.02);
        });
    }
}

//==============================================================================
// 7. Output clipper
double clipCurveRef (double x, double soft)
{
    const double a = std::abs (x), k0 = 1.0 - 0.75 * soft;
    if (a <= k0 || soft <= 0.0) return std::min (1.0, std::max (-1.0, x));
    const double w = 1.0 - k0, y = k0 + w * std::tanh ((a - k0) / w);
    return x < 0 ? -y : y;
}

/** True peak of a segment: 8x interpolation with a 64-tap Hann-windowed sinc (informational accuracy ~1e-3). */
double truePeak (const float* x, int len)
{
    static const auto table = []
    {
        std::array<std::array<double, 64>, 8> t {};
        for (int p = 0; p < 8; ++p)
            for (int i = 0; i < 64; ++i)
            {
                const double d = (i - 31) - p / 8.0;
                const double s = std::abs (d) < 1e-12 ? 1.0 : std::sin (kPi * d) / (kPi * d);
                const double w = 0.5 + 0.5 * std::cos (kPi * d / 32.5);
                t[(size_t) p][(size_t) i] = s * w;
            }
        return t;
    }();
    double pk = 0.0;
    for (int n = 32; n < len - 33; ++n)
        for (int p = 0; p < 8; ++p)
        {
            double acc = 0.0;
            for (int i = 0; i < 64; ++i) acc += table[(size_t) p][(size_t) i] * x[n - 31 + i];
            pk = std::max (pk, std::abs (acc));
        }
    return pk;
}

void suiteClip()
{
    auto& su = S ("clip");
    auto& gPk = su.g ("sample_peak<=ceiling(0 tol)", "abs");
    auto& gM = su.g ("below_knee_linear=G0_4x.mag", "dB");
    auto& gP = su.g ("below_knee_linear=G0_4x.phase", "deg");
    auto& gH = su.g ("above_knee_harmonics(>-60dBc, f0<=1k)", "dB");
    auto& gJ = su.g ("final_jlimit_active_samples(info)", "count");
    auto& gTp = su.g ("P8_true_peak_overshoot(info, max signed)", "dB");
    gTp.signedInfo = true;
    auto& gLat = su.g ("P6_latency_residual(measured-reported)", "samples");
    auto& gOff = su.g ("off_bitexact", "abs");
    const int maxBlock = 2048;
    struct J { double fs; float soft, ceil, adb; double f; };
    std::vector<J> jobs;
    for (double fs : kRates)
        for (float soft : { 0.0f, 0.3f, 1.0f })
            for (float ceil : { -12.0f, -6.0f, -0.3f, 0.0f })
                for (float adb = -24.0f; adb <= 24.0f; adb += (gQuick ? 12.0f : 6.0f))
                    for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 3000.0, 8000.0, 15000.0 })
                        jobs.push_back ({ fs, soft, ceil, adb, f });
    std::mutex lm;
    std::map<double, std::pair<double, double>> latRes;   // fs -> (lowest f, residual)
    parallelFor ((int) jobs.size(), [&] (int i)
    {
        const auto& j = jobs[(size_t) i];
        OutputClipper c;
        c.prepare (j.fs, maxBlock);
        c.setParameters (true, j.ceil, j.soft);
        const long long k = oddBin (j.f, j.fs, NA);
        const int settle = std::max ((int) (0.2 * j.fs), 4096), len = settle + NA, blk = blockFor (i, maxBlock);
        const double A = std::pow (10.0, j.adb / 20.0);
        auto l = coherentSine (k, NA, A, 0.0, len), r = l;
        const auto x = l;
        runBlocks ([&] (float* a, float* b, int n) { c.process (a, b, n); }, l.data(), r.data(), len, blk, maxBlock, i);
        const float cg = juce::Decibels::decibelsToGain (j.ceil);
        double pk = 0.0;
        int lim = 0;
        for (int n = settle; n < len; ++n)
        {
            pk = std::max ({ pk, (double) std::abs (l[(size_t) n]), (double) std::abs (r[(size_t) n]) });
            if (std::abs (l[(size_t) n]) == cg) ++lim;
        }
        const double f0 = k * j.fs / NA, w0 = kTwoPi * (double) k / NA;
        Ctx cx { j.fs, blk, f0, A, "soft=" + num (j.soft) + " ceil=" + num (j.ceil) + " level=" + num (j.adb) + "dBFS", newSine() };
        recBound (gPk, cx, pk, (double) cg, true);
        const double k0 = 1.0 - 0.75 * j.soft;
        const cd X = bin (x.data() + settle, NA, k), H = bin (l.data() + settle, NA, k) / X;
        if (A <= k0 * cg * std::pow (10.0, -1.0 / 20.0))
        {
            const cd He = osG (osStages (2), w0, nullptr);
            rec (gM, cx, dBc (He), dBc (H), dBc (H) - dBc (He), 0.001);
            rec (gP, cx, phaseDeg (He), phaseDeg (H), wrapDeg (phaseDeg (H) - phaseDeg (He)), 0.01);
            const std::lock_guard<std::mutex> lk (lm);
            if (! latRes.count (j.fs) || f0 < latRes[j.fs].first)
                latRes[j.fs] = { f0, -std::arg (H) / w0 - c.getLatencyInSamples() };
        }
        else
        {
            if (j.f <= 1000.0)
            {
                const auto cm = harmonicAmps ([&] (double u) { return clipCurveRef (u, j.soft); }, A / cg, 60);
                std::vector<cd> Y ((size_t) NA);
                for (int n = 0; n < NA; ++n) Y[(size_t) n] = l[(size_t) (settle + n)];
                fft (Y, false);
                const double y1 = cg * cm[1] * std::abs (osG (osStages (2), w0, nullptr));
                for (int m = 1; m <= 60 && m * f0 < 0.4 * j.fs; ++m)
                {
                    const double ex = cg * cm[(size_t) m] * std::abs (osG (osStages (2), m * w0, nullptr));
                    if (dB (ex / y1) < -60.0) continue;
                    long long b = (m * k) % NA;
                    if (b > NA / 2) b = NA - b;
                    const double meas = 2.0 * std::abs (Y[(size_t) b]) / NA;
                    Ctx cm2 = cx; cm2.params += " m=" + std::to_string (m) + " jlimit_samples=" + std::to_string (lim);
                    rec (gH, cm2, dB (ex), dB (meas), dB (meas) - dB (ex), 0.1);
                }
            }
            rec (gJ, cx, 0, lim, lim, -1.0);
            const double tp = truePeak (l.data() + settle, 4096);
            rec (gTp, cx, dB (cg), dB (tp), dB (tp) - dB (cg), -1.0);
        }
    });
    for (auto& [fs, v] : latRes)
        rec (gLat, { fs, 0, v.first, 0, "lowest below-knee frequency", -1 }, 0, v.second, v.second, 0.5);
    for (double fs : kRates)
    {
        OutputClipper c;
        c.prepare (fs, 512);
        c.setParameters (false, -0.3f, 0.3f);
        auto l = coherentSine (oddBin (1000.0, fs, NA), NA, 2.0, 0.0, 8192), r = l;
        const auto x = l;
        runBlocks ([&] (float* a, float* b, int n) { c.process (a, b, n); }, l.data(), r.data(), 8192, 512, 512);
        double worst = 0.0;
        for (int n = 0; n < 8192; ++n) worst = std::max (worst, std::abs ((double) l[(size_t) n] - x[(size_t) n]));
        rec (gOff, { fs, 512, 1000, 2.0, "clipper off", newSine() }, 0, worst, worst, 0.0);
    }
}

//==============================================================================
// 8. Space reverb
space::Settings revBase (int mode)
{
    space::Settings s;
    s.enabled = true; s.mode = mode; s.mix = 1.0f; s.duck = 0.0f;
    return s;
}

std::unique_ptr<space::SpaceReverb> makeRev (double fs, const space::Settings& s)
{
    auto r = std::make_unique<space::SpaceReverb>();
    r->prepare (fs);
    r->setSettings (s);
    return r;
}

/** max over f of |H_line| = |gB LP_b + gM (LP_t - LP_b) + gH (1 - LP_t)| on the parameter-extreme grid (port of rev_loopgain.py). */
struct LoopWorst { double g = 0, f = 0; std::string where; };
LoopWorst loopGainWorst (double fs, const std::vector<float>& lsizes)
{
    LoopWorst w;
    const double base[8] = { 29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.7, 73.1 };
    const double shapes[6][2] = { { 0.7, 1.0 }, { 1.0, 1.0 }, { 0.9, 0.85 }, { 1.5, 0.95 }, { 0.6, 1.15 }, { 1.7, 0.75 } };
    std::vector<double> fr;
    for (int i = 0; i < 1500; ++i) fr.push_back (5.0 * std::pow ((0.4999 * fs) / 5.0, i / 1499.0));
    for (int m = 0; m < 6; ++m)
        for (float lsize : lsizes)
            for (double dec : { 0.1, 1.2, 20.0 })
                for (double bm : { 0.25, 1.0, 4.0 })
                    for (double hm : { 0.1, 1.0, 2.0 })
                        for (double bx : { 50.0, 250.0, 2000.0 })
                            for (double hx : { 1000.0, 4000.0, 16000.0 })
                                for (double b : base)
                                {
                                    const double L = b * shapes[m][0] * (0.35 + 1.65 * lsize) * 1e-3;
                                    auto g = [L] (double t) { return std::min (0.99995, std::pow (10.0, -3.0 * L / std::max (0.05, t))); };
                                    const double t60 = std::min (std::max (dec, 0.1), 20.0);
                                    const double gM = g (t60), gB = g (t60 * bm), gH = g (t60 * hm * shapes[m][1]);
                                    const double Gb = tptG (fs, bx), Gh = tptG (fs, hx);
                                    for (double f : fr)
                                    {
                                        const double ww = kTwoPi * f / fs;
                                        const cd lb = tptLP (ww, Gb), lt = tptLP (ww, Gh);
                                        const double v = std::abs (gB * lb + gM * (lt - lb) + gH * (1.0 - lt));
                                        if (v > w.g)
                                        {
                                            w.g = v; w.f = f;
                                            w.where = "mode=" + space::modeNames()[m].toStdString() + " lsize=" + num (lsize) + " decay=" + num (dec) + " bassMult=" + num (bm)
                                                      + " highMult=" + num (hm) + " bassX=" + num (bx) + " highX=" + num (hx) + " line=" + num (b) + "ms gB/gM/gH="
                                                      + num (gB, 6) + "/" + num (gM, 6) + "/" + num (gH, 6);
                                        }
                                    }
                                }
    return w;
}

/** Schroeder T60 from the -5 .. -dbEnd dB range of the energy decay curve of a band-passed tail. */
double schroederT60 (const std::vector<float>& L, const std::vector<float>& R, int start, double fs, double f, double q, double dbEnd)
{
    const auto bp = ref::bp0 (f, q, fs);
    double s1 [2] = { 0, 0 }, s2 [2] = { 0, 0 };
    std::vector<double> e (L.size());
    for (size_t n = 0; n < L.size(); ++n)
    {
        double acc = 0.0;
        for (int c = 0; c < 2; ++c)
        {
            const double x = c == 0 ? L[n] : R[n];
            const double y = bp.b0 * x + s1[c];
            s1[c] = bp.b1 * x - bp.a1 * y + s2[c];
            s2[c] = bp.b2 * x - bp.a2 * y;
            acc += y * y;
        }
        e[n] = acc;
    }
    std::vector<double> edc (e.size() + 1, 0.0);
    for (size_t n = e.size(); n-- > (size_t) start;) edc[n] = edc[n + 1] + e[n];
    const double e0 = edc[(size_t) start];
    if (e0 <= 0) return 0.0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    for (size_t n = (size_t) start; n < e.size(); n += 16)
    {
        const double d = 10.0 * std::log10 (std::max (edc[n] / e0, 1e-300));
        if (d > -5.0 || d < -dbEnd) continue;
        const double t = (double) (n - (size_t) start) / fs;
        sx += t; sy += d; sxx += t * t; sxy += t * d; ++cnt;
    }
    if (cnt < 3) return 0.0;
    const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    return -60.0 / slope;
}

std::vector<float> burst (double fs, double f, double A, int on, int total)
{
    std::vector<float> x ((size_t) total, 0.0f);
    const int fade = (int) (0.005 * fs);
    for (int n = 0; n < on; ++n)
    {
        double g = 1.0;
        if (n < fade) g = 0.5 - 0.5 * std::cos (kPi * n / fade);
        if (n > on - fade) g = 0.5 - 0.5 * std::cos (kPi * (on - n) / fade);
        x[(size_t) n] = (float) (A * g * std::sin (kTwoPi * f * n / fs));
    }
    return x;
}

void suiteRev()
{
    auto& su = S ("rev");
    auto& gDis = su.g ("disabled_bitexact", "abs");
    auto& gMix0 = su.g ("mix0_bitexact", "abs");
    auto& gW0 = su.g ("width0_mono_L==R", "abs");
    auto& gDry = su.g ("dry_gain_min(1,sqrt2*cos(m*pi/2))", "abs");
    auto& gLoop = su.g ("P5_max_line_loop_gain(analytic, info)", "gain");
    auto& gP5 = su.g ("P5_decay_at_worst_frequency(info)", "s");
    auto& gRt = su.g ("rt60_T30_vs_decay", "rel");
    auto& gFin = su.g ("stability_finite", "count");
    auto& gTail = su.g ("stability_tail_non_increasing(+0.5dB)", "dB");
    notDone ("reverb Hadamard normalisation / orthogonality (1e-7): private (hadamard() and its constant are not reachable through process()); "
             "the code constant 0.35355339f vs 1/sqrt(8) = 0.3535533906 differs by 6e-10 (read from the source, not tested).");

    for (double fs : kRates)
    {
        const int len = (int) fs;
        auto x = coherentSine (oddBin (440.0, fs, NA), NA, 0.5, 0.0, len);
        {
            auto s = revBase (1); s.enabled = false;
            auto r = makeRev (fs, s);
            auto l = x, rr = x;
            runBlocks ([&] (float* a, float* b, int n) { r->process (a, b, n); }, l.data(), rr.data(), len, 512, 4096);
            double worst = 0; for (int n = 0; n < len; ++n) worst = std::max (worst, std::abs ((double) l[(size_t) n] - x[(size_t) n]));
            rec (gDis, { fs, 512, 440, 0.5, "enabled=false", newSine() }, 0, worst, worst, 0.0);
        }
        {
            auto s = revBase (3); s.mix = 0.0f; s.decay = 20.0f;
            auto r = makeRev (fs, s);
            auto l = x, rr = x;
            runBlocks ([&] (float* a, float* b, int n) { r->process (a, b, n); }, l.data(), rr.data(), len, 333, 4096);
            double worst = 0; for (int n = 0; n < len; ++n) worst = std::max ({ worst, std::abs ((double) l[(size_t) n] - x[(size_t) n]), std::abs ((double) rr[(size_t) n] - x[(size_t) n]) });
            rec (gMix0, { fs, 333, 440, 0.5, "mix=0", newSine() }, 0, worst, worst, 0.0);
        }
        {
            auto s = revBase (5); s.width = 0.0f; s.mix = 0.6f;
            auto r = makeRev (fs, s);
            auto l = x, rr = x;
            runBlocks ([&] (float* a, float* b, int n) { r->process (a, b, n); }, l.data(), rr.data(), len, 256, 4096);
            double worst = 0; for (int n = 0; n < len; ++n) worst = std::max (worst, std::abs ((double) l[(size_t) n] - rr[(size_t) n]));
            rec (gW0, { fs, 256, 440, 0.5, "width=0 mono in", newSine() }, 0, worst, worst, 0.0);
        }
        for (int mi = 0; mi <= 10; ++mi)
        {
            auto s = revBase (1);
            s.mix = (float) mi / 10.0f;
            auto r = makeRev (fs, s);
            const int settle = (int) (0.5 * fs), pre = (int) (s.predelayMs * 0.001 * fs);
            std::vector<float> l ((size_t) settle + pre, 0.0f);
            const auto sx = coherentSine (oddBin (1000.0, fs, NA), NA, 0.5, 0.7, pre);
            std::copy (sx.begin(), sx.end(), l.begin() + settle);
            auto rr = l;
            const auto in = l;
            runBlocks ([&] (float* a, float* b, int n) { r->process (a, b, n); }, l.data(), rr.data(), (int) l.size(), 256, 4096);
            const float mf = juce::jlimit (0.0f, 1.0f, s.mix) * juce::MathConstants<float>::halfPi;
            const double ex = s.mix <= 0.0f ? 1.0 : std::min (1.0, std::sqrt (2.0) * std::cos (s.mix * kPi / 2.0));
            double worst = 0;
            for (int n = settle; n < settle + pre - 1; ++n)
                if (std::abs (in[(size_t) n]) > 0.05) worst = std::max (worst, std::abs (l[(size_t) n] / in[(size_t) n] - ex));
            juce::ignoreUnused (mf);
            // 1e-6 after the 0.002 / sample glide has settled (float cos / sqrt2 rounding ~1e-7)
            rec (gDry, { fs, 256, 1000, 0.5, "mix=" + num (s.mix), newSine() }, ex, ex + worst, worst, 1e-6);
        }
    }

    // P5: analytic loop gain at all rates and Lsize extremes, then the measured decay at the worst frequency
    {
        std::vector<LoopWorst> lw (kRates.size());
        parallelFor ((int) kRates.size(), [&] (int i) { lw[(size_t) i] = loopGainWorst (kRates[(size_t) i], { 0.0f, 1.0f }); });
        for (size_t i = 0; i < kRates.size(); ++i)
            rec (gLoop, { kRates[i], 0, lw[i].f, 0, lw[i].where, -1 }, 1.0, lw[i].g, lw[i].g - 1.0, -1.0);
        const double fs = 48000.0, f = lw[1].f;
        auto s = revBase (4);
        s.decay = 20.0f; s.bassMult = 4.0f; s.highMult = 2.0f; s.bassXover = 2000.0f; s.highXover = 1000.0f; s.lSize = 0.0f;
        s.lowCut = 20.0f; s.highCut = 20000.0f;
        auto r = makeRev (fs, s);
        const int total = (int) (30.0 * fs), on = (int) (0.3 * fs);
        auto l = burst (fs, f, 0.5, on, total), rr = l;
        runBlocks ([&] (float* a, float* b, int n) { r->process (a, b, n); }, l.data(), rr.data(), total, 512, 4096);
        const double t60 = schroederT60 (l, rr, on, fs, f, 8.0, 15.0);
        rec (gP5, { fs, 512, f, 0.5, "Plate decay=20 bassMult=4 highMult=2 bassX=2000 highX=1000 lsize=0 (set decay 20 s)", newSine() }, 20.0, t60, t60 - 20.0, -1.0);
        s.bassXover = 250.0f; s.highXover = 4000.0f; s.bassMult = 1.0f; s.highMult = 1.0f;
        auto r2 = makeRev (fs, s);
        auto l2 = burst (fs, f, 0.5, on, total), rr2 = l2;
        runBlocks ([&] (float* a, float* b, int n) { r2->process (a, b, n); }, l2.data(), rr2.data(), total, 512, 4096);
        const double t60b = schroederT60 (l2, rr2, on, fs, f, 8.0, 15.0);
        rec (gP5, { fs, 512, f, 0.5, "same, bassX=250 highX=4000 mults 1 (reference)", newSine() }, 20.0, t60b, t60b - 20.0, -1.0);
        note ("P5 reverb: analytic worst line loop gain " + num (lw[1].g, 6) + " at " + num (lw[1].f, 5) + " Hz (48 kHz; " + lw[1].where
              + "); measured T60 at that frequency (Schroeder, -5..-15 dB) " + num (t60, 4) + " s with Bass Xover > High Xover vs " + num (t60b, 4)
              + " s with ordered crossovers (set: 20 s).");
    }

    // RT60 (T30) of 1 kHz-region sine bursts: set decay +-20 %
    {
        struct J { int mode; float decay; double f; };
        std::vector<J> jobs;
        for (int m = 0; m < 6; ++m) for (float d : { 0.5f, 1.2f, 5.0f, 20.0f }) for (double f : { 500.0, 1000.0, 2000.0 }) if (! gQuick || f == 1000.0) jobs.push_back ({ m, d, f });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            const double fs = 48000.0;
            auto s = revBase (j.mode);
            s.decay = j.decay; s.bassXover = 250.0f; s.highXover = 4000.0f;
            // Review fix: the expected T60 is the Decay knob only when the bass / high decay multipliers are 1 (the defaults
            // 0.8 / 0.6 shorten the decay at 500 Hz / 2 kHz through the overlapping one-pole split), and the T30 window of a
            // 0.5 s decay is dominated by the early reflections, which are not part of the FDN decay: late field only (depth 1).
            s.bassMult = 1.0f; s.highMult = 1.0f; s.depth = 1.0f;
            auto r = makeRev (fs, s);
            const int on = (int) (0.3 * fs), total = on + (int) ((0.8 * j.decay + 2.0) * fs);
            auto l = burst (fs, j.f, 0.5, on, total), rr = l;
            runBlocks ([&] (float* a, float* b, int n) { r->process (a, b, n); }, l.data(), rr.data(), total, 512, 4096);
            const double t60 = schroederT60 (l, rr, on, fs, j.f, 4.0, 35.0);
            // +-20 %: input / early diffusion all-passes, +-0.6 ms delay modulation, overlapping 3-band split at 0.5-2 kHz
            rec (gRt, { fs, 512, j.f, 0.5, "mode=" + space::modeNames()[j.mode].toStdString() + " decay=" + num (j.decay) + " T30=" + num (t60, 4), newSine() },
                 j.decay, t60, t60 / j.decay - 1.0, 0.2);
        });
    }

    // stability: +24 dBFS sines through the parameter extremes
    {
        struct Cfg { const char* name; std::function<void (space::Settings&)> set; };
        std::vector<Cfg> cfgs {
            { "decay20 bassMult4 highMult2 bassX2000 highX1000 lsize0", [] (space::Settings& s) { s.decay = 20; s.bassMult = 4; s.highMult = 2; s.bassXover = 2000; s.highXover = 1000; s.lSize = 0; } },
            { "decay20 lsize1 diffusion1 mod1", [] (space::Settings& s) { s.decay = 20; s.bassMult = 4; s.highMult = 2; s.lSize = 1; s.lDiffusion = 1; s.eDiffusion = 1; s.lModDepth = 1; s.eModDepth = 1; s.lModRate = 1; s.eModRate = 1; } },
            { "decay20 bassMult0.25 highMult0.1 bassX50 highX16000 width2 predelay500", [] (space::Settings& s) { s.decay = 20; s.bassMult = 0.25f; s.highMult = 0.1f; s.bassXover = 50; s.highXover = 16000; s.width = 2; s.predelayMs = 500; } },
            { "decay0.1 cross1 send1 duck1", [] (space::Settings& s) { s.decay = 0.1f; s.eCross = 1; s.lCross = 1; s.eSend = 1; s.duck = 1; } } };
        struct J { size_t cfg; int mode; double f; };
        std::vector<J> jobs;
        for (size_t c = 0; c < cfgs.size(); ++c) for (int m = 0; m < 6; ++m)
            for (double f : { 20.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 }) if (! gQuick || f == 1000.0) jobs.push_back ({ c, m, f });
        parallelFor ((int) jobs.size(), [&] (int i)
        {
            const auto& j = jobs[(size_t) i];
            const double fs = 48000.0;
            auto s = revBase (j.mode);
            cfgs[j.cfg].set (s);
            auto r = makeRev (fs, s);
            const int on = (int) (2.0 * fs), total = on + (int) (10.0 * fs);
            auto l = burst (fs, j.f, std::pow (10.0, 24.0 / 20.0), on, total), rr = l;
            runBlocks ([&] (float* a, float* b, int n) { r->process (a, b, n); }, l.data(), rr.data(), total, 512, 4096);
            int bad = 0;
            double pA = 0, pB = 0;
            for (int n = 0; n < total; ++n)
            {
                if (! std::isfinite (l[(size_t) n]) || ! std::isfinite (rr[(size_t) n])) ++bad;
                const double v = std::max (std::abs ((double) l[(size_t) n]), std::abs ((double) rr[(size_t) n]));
                if (n >= total - (int) (5 * fs)) pB = std::max (pB, v); else if (n >= on) pA = std::max (pA, v);
            }
            Ctx c { fs, 512, j.f, 15.85, std::string ("mode=") + space::modeNames()[j.mode].toStdString() + " " + cfgs[j.cfg].name, newSine() };
            rec (gFin, c, 0, bad, bad, 0.0);
            if (pB > 1e-30) recBound (gTail, c, dB (pB) - dB (std::max (pA, 1e-30)), 0.5, true);
        });
    }
}

//==============================================================================
// 9. Whole processor (the oscillator is the only source: processBlock ignores the host input)
void setP (CodexProcessor& p, const juce::String& id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    jassert (prm != nullptr);
    if (prm != nullptr) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}
float getP (CodexProcessor& p, const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); }

std::unique_ptr<CodexProcessor> makeProc()
{
    auto p = std::make_unique<CodexProcessor>();
    p->setNonRealtime (true);
    p->seedNoiseForTesting (1);
    setP (*p, ID::keyMode, 0.0f);   // Fixed Key
    return p;
}

/** W0: synth sine, no sweep / click / attack, sustain 1, velocity 0, sub off; every module off and transparent
    (distortions off with OS Off and DC off), output 0 dB, clipper off, modern order. */
void procW0 (CodexProcessor& p)
{
    setP (p, ID::engine, 0); setP (p, ID::wave, 0); setP (p, ID::pAmount, 0); setP (p, ID::clickLevel, 0);
    setP (p, ID::aAttack, 0); setP (p, ID::aSustain, 1); setP (p, ID::aRelease, 1); setP (p, ID::velocity, 0); setP (p, ID::subOn, 0);
    setP (p, ID::outGain, 0); setP (p, ID::clipOn, 0); setP (p, ID::chainOrder, 0); setP (p, ID::phase, 0);
    for (int st = 1; st <= 2; ++st) { setP (p, ID::clip (st, "On"), 0); setP (p, ID::clip (st, "OS"), 0); setP (p, ID::clip (st, "DC"), 0); }
    setP (p, ID::sat ("On"), 0); setP (p, ID::cmp ("On"), 0); setP (p, ID::spc ("On"), 0);
    for (int e = 0; e < 2; ++e)
    {
        setP (p, ID::eqOn (e), 1); setP (p, ID::eqMode (e), 0);
        for (int b = 0; b < Params::numBands; ++b) setP (p, ID::eqBand (e, b, "On"), 0);
    }
}

void setPitch (CodexProcessor& p, int key, int oct, float tune) { setP (p, ID::key, (float) key); setP (p, ID::octave, (float) oct); setP (p, ID::tune, tune); }
int keyNoteOf (CodexProcessor& p) { return Params::keyBaseNote + (int) getP (p, ID::key) + 12 * ((int) getP (p, ID::octave) - 1); }

struct Render { std::vector<float> l, r; };
/** prepareToPlay, optional settle (linear-phase EQ), note-on at sample 0 (velocity 127), render len samples. */
Render renderProc (CodexProcessor& p, double fs, int len, int block, bool settleEq = false, int noteOffAt = -1, int seed = 5)
{
    p.setRateAndBufferSizeDetails (fs, 512);
    p.prepareToPlay (fs, 512);
    juce::AudioBuffer<float> buf (2, 4096);
    juce::MidiBuffer none;
    if (settleEq)
    {
        for (int t = 0; t < 200000 && ! p.isSettled(); ++t)
        {
            if (! p.isEqBuildReadyForTesting()) { std::this_thread::sleep_for (std::chrono::milliseconds (2)); continue; }
            buf.setSize (2, 256, false, false, true);
            buf.clear();
            p.processBlock (buf, none);
        }
    }
    Render o;
    o.l.assign ((size_t) len, 0.0f);
    o.r.assign ((size_t) len, 0.0f);
    juce::Random rng (seed);
    for (int pos = 0; pos < len;)
    {
        const int n = std::min (block > 0 ? block : 1 + rng.nextInt (4096), len - pos);
        buf.setSize (2, n, false, false, true);
        buf.clear();
        juce::MidiBuffer midi;
        if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
        if (noteOffAt >= pos && noteOffAt < pos + n) midi.addEvent (juce::MidiMessage::noteOff (1, 60), noteOffAt - pos);
        p.processBlock (buf, midi);
        std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + n, o.l.begin() + pos);
        std::copy (buf.getReadPointer (1), buf.getReadPointer (1) + n, o.r.begin() + pos);
        pos += n;
    }
    return o;
}

/** The voice alone, as the processor configures it (W0 settings), for the same pitch. */
std::vector<float> refVoice (CodexProcessor& p, double fs, int len, int wave = 0)
{
    return renderVoice (fs, synthSettings (wave, getP (p, ID::tune), 0.0f), (float) keyNoteOf (p), len, 512).out;
}

/** H = Y / X at the voice frequency (BH7, from 1 s on). */
cd procH (const std::vector<float>& y, const std::vector<float>& x, double fs, double f, int start)
{
    const int len = (int) y.size() - start;
    const double w = kTwoPi * f / fs;
    return toneBH (y.data() + start, len, w) / toneBH (x.data() + start, len, w);
}

struct Pitch { int key, oct; float tune; };
std::vector<Pitch> pitches (int n, int seed)
{
    std::mt19937 rng ((unsigned) seed);
    std::vector<Pitch> v;
    for (int i = 0; i < n; ++i)
    {
        Pitch p { (int) (rng() % 12), (int) (rng() % 3), 0.0f };
        if (i % 3 == 1) p.tune = (float) ((int) (rng() % 4801) - 2400) / 100.0f;
        v.push_back (p);
    }
    return v;
}

void suiteProc()
{
    auto& su = S ("proc");
    std::vector<int> blocks { 1, 64, 256, 333, 512, 4096, -1 };

    //--------------------------------------------------------------------------
    // W0: everything off -> bit-exact voice
    {
        auto& g = su.g ("W0.all_off==KickVoice_bitexact", "abs");
        const auto ps = pitches (gQuick ? 12 : 60, 1);
        struct J { double fs; Pitch p; int blk; };
        std::vector<J> jobs;
        int idx = 0;
        for (double fs : kRates) for (auto& p : ps) jobs.push_back ({ fs, p, blocks[(size_t) (idx++ % (int) blocks.size())] });
        for (int b : blocks) for (int q = 0; q < 3; ++q) jobs.push_back ({ 48000.0, ps[(size_t) q], b });
        std::vector<std::unique_ptr<CodexProcessor>> procs;
        for (int t = 0; t < gThreads; ++t) { procs.push_back (makeProc()); procW0 (*procs.back()); }
        std::atomic<int> next { 0 };
        std::vector<std::thread> th;
        for (int t = 0; t < gThreads; ++t)
            th.emplace_back ([&, t]
            {
                juce::ScopedNoDenormals nd;
                for (;;)
                {
                    const int i = next.fetch_add (1);
                    if (i >= (int) jobs.size()) break;
                    auto& p = *procs[(size_t) t];
                    const auto& j = jobs[(size_t) i];
                    setPitch (p, j.p.key, j.p.oct, j.p.tune);
                    const int len = (int) (0.5 * j.fs);
                    const auto o = renderProc (p, j.fs, len, j.blk);
                    const auto x = refVoice (p, j.fs, len);
                    double worst = 0;
                    for (int n = 0; n < len; ++n) worst = std::max ({ worst, std::abs ((double) o.l[(size_t) n] - x[(size_t) n]), std::abs ((double) o.r[(size_t) n] - x[(size_t) n]) });
                    rec (g, { j.fs, j.blk, expectedVoiceFreq (j.fs, (float) keyNoteOf (p), getP (p, ID::tune)), 1, "note=" + std::to_string (keyNoteOf (p)) + " tune=" + num (getP (p, ID::tune)), newSine() },
                         0, worst, worst, 0.0);
                }
            });
        for (auto& t : th) t.join();
    }

    // generic worker helper for processor jobs: configure (p), then measure
    auto runProcJobs = [&] (int n, const std::function<void (CodexProcessor&, int)>& job)
    {
        std::vector<std::unique_ptr<CodexProcessor>> procs;
        for (int t = 0; t < gThreads; ++t) { procs.push_back (makeProc()); procW0 (*procs.back()); }
        std::atomic<int> next { 0 };
        std::vector<std::thread> th;
        for (int t = 0; t < gThreads; ++t)
            th.emplace_back ([&, t] { juce::ScopedNoDenormals nd; for (;;) { const int i = next.fetch_add (1); if (i >= n) break; procW0 (*procs[(size_t) t]); job (*procs[(size_t) t], i); } });
        for (auto& t : th) t.join();
    };

    //--------------------------------------------------------------------------
    // W1: off modules with factory defaults (OS 4x, DC on), output -3 dB: [G0_4x H_DC]^2 * 10^(-3/20)
    {
        auto& gM = su.g ("W1.defaults_off.mag", "dB");
        auto& gP = su.g ("W1.defaults_off.phase", "deg");
        auto& gL = su.g ("W1.reported_latency==round(2*L_os4x)", "samples");
        const auto ps = pitches (gQuick ? 6 : 30, 2);
        runProcJobs ((int) (ps.size() * kRates.size()), [&] (CodexProcessor& p, int i)
        {
            const double fs = kRates[(size_t) i / ps.size()];
            const auto& pp = ps[(size_t) i % ps.size()];
            for (int st = 1; st <= 2; ++st) { setP (p, ID::clip (st, "OS"), 2); setP (p, ID::clip (st, "DC"), 1); }
            setP (p, ID::outGain, -3.0f);
            setPitch (p, pp.key, pp.oct, pp.tune);
            const int len = (int) (3.0 * fs);
            const auto o = renderProc (p, fs, len, 512);
            const auto x = refVoice (p, fs, len);
            const double f = expectedVoiceFreq (fs, (float) keyNoteOf (p), getP (p, ID::tune));
            const cd H = procH (o.l, x, fs, f, (int) fs);
            const double w = kTwoPi * f / fs;
            const cd one = osG (osStages (2), w, nullptr) * dcHP (w, dcRfor (fs));
            const cd He = one * one * (double) juce::Decibels::decibelsToGain (-3.0f);
            Ctx c { fs, 512, f, 1, "note=" + std::to_string (keyNoteOf (p)) + " tune=" + num (getP (p, ID::tune)), newSine() };
            // 0.002 dB / 0.05 deg
            rec (gM, c, dBc (He), dBc (H), dBc (H) - dBc (He), 0.002);
            rec (gP, c, phaseDeg (He), phaseDeg (H), wrapDeg (phaseDeg (H) - phaseDeg (He)), 0.05);
        });
        auto p = makeProc();
        procW0 (*p);
        for (int st = 1; st <= 2; ++st) { setP (*p, ID::clip (st, "OS"), 2); setP (*p, ID::clip (st, "DC"), 1); }
        for (double fs : kRates)
        {
            renderProc (*p, fs, 512, 512);
            const int ex = juce::roundToInt (2.0 * StereoOversampling (2).getLatencyInSamples());
            rec (gL, { fs, 512, 0, 0, "", -1 }, ex, p->getLatencySamples(), p->getLatencySamples() - ex, 0.0);
        }
    }

    //--------------------------------------------------------------------------
    // W2: EQ 1 Zero Latency (sine: complex gain; triangle: time domain against the analytic cascade)
    {
        auto& gM = su.g ("W2.eq1_zl.mag", "dB");
        auto& gP = su.g ("W2.eq1_zl.phase", "deg");
        auto& gA = su.g ("W2.eq1_zl.below-40dB", "abs");
        auto& gT = su.g ("W2.eq1_zl.triangle_time_domain", "abs");
        tolNote ("proc W2: output gain set to -24 dB (expected values include it): the first run clipped boosted bands at the processor's "
                 "+18 dBFS safety clamp (a harness set-up error, corrected after the first run).");
        const auto all = eqZlConfigs();
        std::vector<eq::BandSettings> cf;
        for (size_t c = 0; c < all.size(); c += (gQuick ? 12 : 3)) cf.push_back (all[c].b);
        const auto ps = pitches (gQuick ? 3 : 10, 3);
        const int perCfg = (int) ps.size() + 3;
        runProcJobs ((int) (cf.size() * perCfg), [&] (CodexProcessor& p, int i)
        {
            const double fs = 48000.0;
            const auto& b = cf[(size_t) i / perCfg];
            const int q = i % perCfg;
            const bool tri = q >= (int) ps.size();
            const auto& pp = ps[(size_t) (tri ? q - (int) ps.size() : q)];
            setP (p, ID::eqBand (0, 0, "On"), 1); setP (p, ID::eqBand (0, 0, "Type"), (float) (int) b.type); setP (p, ID::eqBand (0, 0, "Freq"), b.freq);
            setP (p, ID::eqBand (0, 0, "Gain"), b.gainDb); setP (p, ID::eqBand (0, 0, "Q"), b.q); setP (p, ID::eqBand (0, 0, "Slope"), (float) b.slope);
            setP (p, ID::eqBand (0, 0, "Place"), 0);
            if (tri) setP (p, ID::wave, 1);
            setP (p, ID::outGain, -24.0f);   // keep boosted bands below the +18 dBFS safety clamp
            setPitch (p, pp.key, pp.oct, pp.tune);
            eq::BandSettings rb = b;
            rb.freq = getP (p, ID::eqBand (0, 0, "Freq")); rb.gainDb = getP (p, ID::eqBand (0, 0, "Gain")); rb.q = getP (p, ID::eqBand (0, 0, "Q"));
            const auto c = ref::design (rb, fs);
            const int settle = std::max ((int) fs, settleFor (c, fs));
            const int len = settle + (int) (2.0 * fs);
            const auto o = renderProc (p, fs, len, 512);
            const auto x = refVoice (p, fs, len, tri ? 1 : 0);
            const double f = expectedVoiceFreq (fs, (float) keyNoteOf (p), getP (p, ID::tune));
            Ctx cx { fs, 512, f, 1, bandName (rb) + " note=" + std::to_string (keyNoteOf (p)) + (tri ? " triangle" : ""), newSine() };
            if (! tri)
            {
                const cd H = procH (o.l, x, fs, f, settle), He = Hc (c, kTwoPi * f / fs) * (double) juce::Decibels::decibelsToGain (-24.0f);
                checkH (gM, gP, gA, cx, H, He, 0.001, 0.01, -40.0, 2e-6);
                return;
            }
            // double cascade (independent coefficients) over the exact float triangle samples
            std::vector<double> y (x.begin(), x.end());
            for (auto& v : y) v *= (double) juce::Decibels::decibelsToGain (-24.0f);
            for (auto& bq : c)
            {
                double s1 = 0, s2 = 0;
                for (auto& v : y) { const double in = v, out = bq.b0 * in + s1; s1 = bq.b1 * in - bq.a1 * out + s2; s2 = bq.b2 * in - bq.a2 * out; v = out; }
            }
            double worst = 0.0;
            for (int n = settle; n < len; ++n) worst = std::max (worst, std::abs ((double) o.l[(size_t) n] - y[(size_t) n]));
            rec (gT, cx, 0, worst, worst, 2e-6);
        });
    }

    //--------------------------------------------------------------------------
    // W3: EQ 1 Natural / Linear latency reporting and the measured delay (bands off)
    {
        auto& gL = su.g ("W3.eq_latency_reported", "samples");
        auto& gD = su.g ("W3.eq_measured_delay_vs_reported", "samples");
        auto p = makeProc();
        procW0 (*p);
        juce::dsp::Oversampling<float> unc (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        const double latU = unc.getLatencyInSamples();
        double fd = 1.0 - (latU - std::floor (latU));
        if (std::abs (fd - 1.0) < 1e-6) fd = 0.0; else if (fd < 0.618) fd += 1.0;
        struct J { int mode, res; double fs; };
        std::vector<J> jobs;
        for (double fs : kRates) { jobs.push_back ({ 1, 1, fs }); jobs.push_back ({ 2, 1, fs }); }
        for (int res : { 0, 2, 3, 4 }) jobs.push_back ({ 2, res, 48000.0 });
        for (auto& j : jobs)
        {
            procW0 (*p);
            setP (*p, ID::eqMode (0), (float) j.mode);
            setP (*p, ID::eqRes (0), (float) j.res);
            setPitch (*p, 2, 1, 0.0f);   // G1 ~ 49 Hz
            const int ex = j.mode == 1 ? juce::roundToInt (latU + fd) : myFirLength (j.res, j.fs) / 2 + 256;
            const int len = ex + (int) (3.0 * j.fs);
            const auto o = renderProc (*p, j.fs, len, 512, true);
            const auto x = refVoice (*p, j.fs, len);
            const double f = expectedVoiceFreq (j.fs, (float) keyNoteOf (*p), 0.0f), w = kTwoPi * f / j.fs;
            Ctx c { j.fs, 512, f, 1, std::string (j.mode == 1 ? "Natural" : "Linear") + " res=" + std::to_string (j.res), newSine() };
            rec (gL, c, ex, p->getLatencySamples(), p->getLatencySamples() - ex, 0.0);
            const cd H = procH (o.l, x, j.fs, f, (int) j.fs + ex);
            const double resid = -std::arg (H * std::polar (1.0, w * ex)) / w;
            rec (gD, c, ex, ex + resid, resid, 0.05);
        }
    }

    //--------------------------------------------------------------------------
    // W4: chain order: with only linear modules on, every order gives the same output (LTI modules commute)
    {
        auto& g = su.g ("W4.chain_order_invariance(linear modules)", "abs");
        const int nOrders = gQuick ? 60 : chain::numOrders;
        auto cfg = [] (CodexProcessor& p)
        {
            setP (p, ID::eqBand (0, 0, "On"), 1); setP (p, ID::eqBand (0, 0, "Type"), (float) (int) eq::BandType::Bell);
            setP (p, ID::eqBand (0, 0, "Freq"), 200.0f); setP (p, ID::eqBand (0, 0, "Gain"), 6.0f); setP (p, ID::eqBand (0, 0, "Q"), 1.0f);
            setP (p, ID::eqBand (1, 0, "On"), 1); setP (p, ID::eqBand (1, 0, "Type"), (float) (int) eq::BandType::LowShelf);
            setP (p, ID::eqBand (1, 0, "Freq"), 100.0f); setP (p, ID::eqBand (1, 0, "Gain"), 4.0f);
            setP (p, ID::sat ("On"), 1); setP (p, ID::sat ("OS"), 0);
            for (int b = 0; b < sat::maxBands; ++b) setP (p, ID::satBand (b, "Bypass"), 1);
            setP (p, ID::cmp ("On"), 1); setP (p, ID::cmp ("Bands"), 2); setP (p, ID::cmpBand (0, "Ratio"), 1.0f); setP (p, ID::cmpBand (1, "Ratio"), 1.0f);
            setP (p, ID::cmp ("Auto"), 0);
            setPitch (p, 5, 1, 0.0f);
        };
        const double fs = 48000.0;
        std::vector<float> refL;
        {
            auto p = makeProc();
            procW0 (*p);
            cfg (*p);
            refL = renderProc (*p, fs, (int) (0.25 * fs), 512).l;
        }
        std::vector<std::unique_ptr<CodexProcessor>> procs;
        for (int t = 0; t < gThreads; ++t)
        {
            procs.push_back (makeProc());
            procW0 (*procs.back());
            cfg (*procs.back());
            procs.back()->setRateAndBufferSizeDetails (fs, 512);
            procs.back()->prepareToPlay (fs, 512);
        }
        std::atomic<int> next { 0 };
        std::vector<std::thread> th;
        for (int t = 0; t < gThreads; ++t)
            th.emplace_back ([&, t]
            {
                juce::ScopedNoDenormals nd;
                auto& p = *procs[(size_t) t];
                juce::AudioBuffer<float> buf (2, 512);
                juce::MidiBuffer none;
                const int len = (int) (0.25 * fs);
                for (;;)
                {
                    const int i = next.fetch_add (1);
                    if (i >= nOrders) break;
                    const int order = gQuick ? (i * 83) % chain::numOrders : i;
                    setP (p, ID::chainOrder, (float) order);
                    for (int q = 0; q < (int) (0.4 * fs) / 512; ++q)   // note-off, ring-out and reorder fade
                    {
                        buf.setSize (2, 512, false, false, true);
                        buf.clear();
                        juce::MidiBuffer off;
                        if (q == 0) off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                        p.processBlock (buf, q == 0 ? off : none);
                    }
                    std::vector<float> out ((size_t) len);
                    for (int pos = 0; pos < len; pos += 512)
                    {
                        const int n = std::min (512, len - pos);
                        buf.setSize (2, n, false, false, true);
                        buf.clear();
                        juce::MidiBuffer midi;
                        if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
                        p.processBlock (buf, midi);
                        std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + n, out.begin() + pos);
                    }
                    double worst = 0;
                    for (int n = 0; n < len; ++n) worst = std::max (worst, std::abs ((double) out[(size_t) n] - refL[(size_t) n]));
                    rec (g, { fs, 512, 0, 1, "order=" + std::to_string (order) + " " + chain::describe (order).toStdString(), newSine() }, 0, worst, worst, 1e-6);
                }
            });
        for (auto& t : th) t.join();
        notDone ("W4: the extra '50 random orders x 3 pitches' pass was not run separately (all orders run at one pitch).");
    }

    //--------------------------------------------------------------------------
    // W5: saturation on with every band bypassed; compressor on at ratio 1 (3 bands)
    {
        auto& gM = su.g ("W5.sat_bypassed+comp_ratio1.mag", "dB");
        auto& gP = su.g ("W5.sat_bypassed+comp_ratio1.phase", "deg");
        const auto ps = pitches (gQuick ? 6 : 30, 5);
        runProcJobs ((int) ps.size() * 2, [&] (CodexProcessor& p, int i)
        {
            const double fs = 48000.0;
            const bool satCase = i % 2 == 0;
            const auto& pp = ps[(size_t) i / 2];
            setPitch (p, pp.key, pp.oct, pp.tune);
            if (satCase)
            {
                setP (p, ID::sat ("On"), 1);
                for (int b = 0; b < sat::maxBands; ++b) setP (p, ID::satBand (b, "Bypass"), 1);
            }
            else
            {
                setP (p, ID::cmp ("On"), 1); setP (p, ID::cmp ("Bands"), 3); setP (p, ID::cmp ("Auto"), 0);
                for (int b = 0; b < 3; ++b) setP (p, ID::cmpBand (b, "Ratio"), 1.0f);
            }
            const int len = (int) (3.0 * fs);
            const auto o = renderProc (p, fs, len, 512);
            const auto x = refVoice (p, fs, len);
            const double f = expectedVoiceFreq (fs, (float) keyNoteOf (p), getP (p, ID::tune)), w = kTwoPi * f / fs;
            cd He = 1.0;
            if (satCase)
            {
                const int nb = (int) getP (p, ID::sat ("Bands")), slope = (int) getP (p, ID::sat ("Slope")), os = (int) getP (p, ID::sat ("OS"));
                std::array<float, 5> xs {};
                for (int j = 0; j < 5; ++j) xs[(size_t) j] = getP (p, ID::sat (("X" + juce::String (j + 1)).toRawUTF8()));
                const double rate = fs * (1 << os);
                const auto xf = clampXovers (xs, rate);
                const RespFn inner = [=] (double th) { cd h = 1.0; for (int q = 0; q < nb - 1; ++q) h *= xoverAP (slope, xf[(size_t) q], rate, th); return h; };
                He = (os > 0 ? osG (osStages (os), w, inner) : inner (w)) * dcHP (w, dcRfor (fs));
            }
            else
            {
                std::array<float, 5> xs { 20000.0f, 20000.0f, 20000.0f, 20000.0f, 20000.0f };
                for (int j = 0; j < 3; ++j) xs[(size_t) j] = getP (p, ID::cmp (("X" + juce::String (j + 1)).toRawUTF8()));
                const auto xf = clampXovers (xs, fs);
                for (int q = 0; q < 2; ++q) He *= xoverAP (2, xf[(size_t) q], fs, w);
            }
            const cd H = procH (o.l, x, fs, f, (int) fs);
            Ctx c { fs, 512, f, 1, std::string (satCase ? "saturation bypassed (defaults)" : "compressor 3 bands ratio 1") + " note=" + std::to_string (keyNoteOf (p)), newSine() };
            rec (gM, c, dBc (He), dBc (H), dBc (H) - dBc (He), 0.002);
            rec (gP, c, phaseDeg (He), phaseDeg (H), wrapDeg (phaseDeg (H) - phaseDeg (He)), 0.05);
        });
    }

    //--------------------------------------------------------------------------
    // W6: latency accounting: reported = round (sum of fractional module latencies + clipper); measured delay
    {
        auto& gR = su.g ("W6.reported==round(sum)", "samples");
        auto& gD = su.g ("W6.measured_delay-reported(<=0.51)", "samples");
        auto p = makeProc();
        struct C { const char* name; int os1, os2; bool clip; int look; };
        const std::vector<C> cs { { "D1 4x + D2 2x", 2, 1, false, 0 }, { "D1 4x + D2 2x + clipper", 2, 1, true, 0 },
                                  { "D1 4x + D2 2x + clipper + comp look 5 ms", 2, 1, true, 3 }, { "D1 16x + D2 8x + clipper", 4, 3, true, 0 } };
        for (auto& cc : cs)
            for (double fs : kRates)
            {
                procW0 (*p);
                setP (*p, ID::clip (1, "OS"), (float) cc.os1); setP (*p, ID::clip (2, "OS"), (float) cc.os2);
                if (cc.clip) { setP (*p, ID::clipOn, 1); setP (*p, ID::outGain, -12.0f); }
                if (cc.look > 0) { setP (*p, ID::cmp ("On"), 1); setP (*p, ID::cmp ("Look"), (float) cc.look); setP (*p, ID::cmpBand (0, "Ratio"), 1.0f); setP (*p, ID::cmp ("Auto"), 0); }
                setPitch (*p, 2, 1, 0.0f);
                const int len = (int) (3.0 * fs);
                const auto o = renderProc (*p, fs, len, 512);
                const auto x = refVoice (*p, fs, len);
                const double sum = StereoOversampling (cc.os1).getLatencyInSamples() + StereoOversampling (cc.os2).getLatencyInSamples()
                                   + (cc.look > 0 ? std::lround (comp::lookaheadMs (cc.look) * 0.001 * fs) : 0)
                                   + (cc.clip ? juce::roundToInt (StereoOversampling (2).getLatencyInSamples()) : 0);
                const int ex = juce::roundToInt (sum);
                Ctx c { fs, 512, 0, 1, std::string (cc.name) + " sum=" + num (sum, 7), newSine() };
                rec (gR, c, ex, p->getLatencySamples(), p->getLatencySamples() - ex, 0.0);
                const double f = expectedVoiceFreq (fs, (float) keyNoteOf (*p), 0.0f), w = kTwoPi * f / fs;
                cd H = procH (o.l, x, fs, f, (int) fs);
                if (cc.clip) H /= (double) juce::Decibels::decibelsToGain (-12.0f);
                const double meas = ex - std::arg (H * std::polar (1.0, w * ex)) / w;
                c.f = f;
                rec (gD, c, ex, meas, meas - ex, 0.51);
            }
    }

    //--------------------------------------------------------------------------
    // W7: sub-layer alignment (informational): phase of sub vs main after removing the sub's own low-pass
    {
        auto& g = su.g ("W7.sub_vs_main_phase_after_LP(info)", "deg");
        const auto ps = pitches (gQuick ? 5 : 20, 7);
        runProcJobs ((int) ps.size() * 5, [&] (CodexProcessor& p, int i)
        {
            const double fs = 48000.0;
            const int cfgI = i % 5;
            const auto& pp = ps[(size_t) i / 5];
            const char* names[] = { "all off OS Off", "dist off OS 4x DC on", "sat bypassed", "comp 3 bands ratio 1", "D1 OS 16x" };
            switch (cfgI)
            {
                case 1: for (int st = 1; st <= 2; ++st) { setP (p, ID::clip (st, "OS"), 2); setP (p, ID::clip (st, "DC"), 1); } break;
                case 2: setP (p, ID::sat ("On"), 1); for (int b = 0; b < sat::maxBands; ++b) setP (p, ID::satBand (b, "Bypass"), 1); break;
                case 3: setP (p, ID::cmp ("On"), 1); setP (p, ID::cmp ("Bands"), 3); setP (p, ID::cmp ("Auto"), 0); for (int b = 0; b < 3; ++b) setP (p, ID::cmpBand (b, "Ratio"), 1.0f); break;
                case 4: setP (p, ID::clip (1, "OS"), 4); break;
                default: break;
            }
            setPitch (p, pp.key, pp.oct, pp.tune);
            setP (p, ID::subLevel, 0.0f); setP (p, ID::subLowPass, 300.0f); setP (p, ID::subFade, 0.0f); setP (p, ID::subPhase, 0.0f);
            const int len = (int) (3.0 * fs);
            const auto a = renderProc (p, fs, len, 512);
            setP (p, ID::subOn, 1);
            const auto b = renderProc (p, fs, len, 512);
            std::vector<float> sub ((size_t) len);
            for (int n = 0; n < len; ++n) sub[(size_t) n] = b.l[(size_t) n] - a.l[(size_t) n];
            const double f = expectedVoiceFreq (fs, (float) keyNoteOf (p), getP (p, ID::tune)), w = kTwoPi * f / fs;
            const cd M = toneBH (a.l.data() + (int) fs, len - (int) fs, w), Sx = toneBH (sub.data() + (int) fs, len - (int) fs, w);
            const float G = tptG (fs, getP (p, ID::subLowPass));
            const cd lp = tptLP (w, G);
            const double d = wrapDeg (phaseDeg (Sx) - phaseDeg (M) - phaseDeg (lp * lp));
            rec (g, { fs, 512, f, 1, std::string (names[cfgI]) + " note=" + std::to_string (keyNoteOf (p)), newSine() }, 0, d, d, -1.0);
        });
    }

    //--------------------------------------------------------------------------
    // W8: output gain; safety clamp; clipper ceiling
    {
        auto& gG = su.g ("W8.output_gain", "dB");
        auto& gS = su.g ("W8.hot_chain_peak<=7.94", "abs");
        auto& gN = su.g ("W8.hot_chain_non_finite", "count");
        auto& gC = su.g ("W8.clipper_peak<=ceiling", "abs");
        auto p = makeProc();
        const double fs = 48000.0;
        for (float og : { -24.0f, 0.0f, 12.0f })
            for (int k = 0; k < 5; ++k)
            {
                procW0 (*p);
                setP (*p, ID::outGain, og);
                setPitch (*p, k * 2, 1, 0.0f);
                const int len = (int) (2.0 * fs);
                const auto o = renderProc (*p, fs, len, 512);
                const auto x = refVoice (*p, fs, len);
                const double f = expectedVoiceFreq (fs, (float) keyNoteOf (*p), 0.0f);
                const cd H = procH (o.l, x, fs, f, (int) (0.5 * fs));
                rec (gG, { fs, 512, f, 1, "out=" + num (og), newSine() }, og, dBc (H), dBc (H) - og, 0.001);
            }
        for (int clip = 0; clip < 2; ++clip)
            for (double rate : kRates)
            {
                auto q = makeProc();   // factory defaults (click, distortions on)
                setP (*q, ID::clip (1, "Drive"), 48.0f); setP (*q, ID::clip (1, "Out"), 12.0f); setP (*q, ID::clip (2, "Out"), 12.0f); setP (*q, ID::outGain, 12.0f);
                setP (*q, ID::clipOn, (float) clip);
                const auto o = renderProc (*q, rate, (int) rate, 512);
                double pk = 0;
                int bad = 0;
                const int from = clip ? (int) (0.05 * rate) : 0;
                for (int n = from; n < (int) rate; ++n)
                {
                    if (! std::isfinite (o.l[(size_t) n]) || ! std::isfinite (o.r[(size_t) n])) ++bad;
                    pk = std::max ({ pk, (double) std::abs (o.l[(size_t) n]), (double) std::abs (o.r[(size_t) n]) });
                }
                Ctx c { rate, 512, 0, 0, clip ? "clipper on, ceiling -0.3 dB" : "D1 drive 48, outs +12, clipper off", newSine() };
                if (clip) recBound (gC, c, pk, (double) juce::Decibels::decibelsToGain (-0.3f), true);
                else { recBound (gS, c, pk, 7.94f, true); rec (gN, c, 0, bad, bad, 0.0); }
            }
    }

    //--------------------------------------------------------------------------
    // W9: determinism (factory defaults): two instances; block-size independence
    {
        auto& gI = su.g ("W9.two_instances_bitexact", "abs");
        auto& gB = su.g ("W9.block_size_independence_bitexact", "abs");
        const double fs = 48000.0;
        const int len = (int) fs;
        auto a = makeProc(), b = makeProc();
        const auto ra = renderProc (*a, fs, len, 512), rb = renderProc (*b, fs, len, 512);
        double w = 0;
        for (int n = 0; n < len; ++n) w = std::max ({ w, std::abs ((double) ra.l[(size_t) n] - rb.l[(size_t) n]), std::abs ((double) ra.r[(size_t) n] - rb.r[(size_t) n]) });
        rec (gI, { fs, 512, 0, 0, "factory defaults", newSine() }, 0, w, w, 0.0);
        for (int blk : blocks)
        {
            auto c = makeProc();
            const auto rc = renderProc (*c, fs, len, blk);
            double d = 0;
            int first = -1;
            for (int n = 0; n < len; ++n)
            {
                const double e = std::max (std::abs ((double) rc.l[(size_t) n] - ra.l[(size_t) n]), std::abs ((double) rc.r[(size_t) n] - ra.r[(size_t) n]));
                if (e > 0 && first < 0) first = n;
                d = std::max (d, e);
            }
            std::string who;
            if (d > 0.0)
            {
                // attribution: which distortion's oversampler (OS forced Off) removes the difference
                for (int st = 1; st <= 2; ++st)
                {
                    auto e1 = makeProc(), e2 = makeProc();
                    setP (*e1, ID::clip (st, "OS"), 0); setP (*e2, ID::clip (st, "OS"), 0);
                    const auto r1 = renderProc (*e1, fs, len, 512), r2 = renderProc (*e2, fs, len, blk);
                    double dd = 0;
                    for (int n = 0; n < len; ++n) dd = std::max (dd, (double) std::abs (r1.l[(size_t) n] - r2.l[(size_t) n]));
                    who += " | with D" + std::to_string (st) + " OS Off: max diff " + num (dd, 3);
                }
            }
            rec (gB, { fs, blk, 0, 0, "vs block 512, first difference at sample " + std::to_string (first) + who, newSine() }, 0, d, d, 0.0);
        }
    }
}

//==============================================================================
// 10. Extreme inputs, 11. denormals and NaN
struct ModInst { ProcFn fn; std::shared_ptr<void> hold; };
struct ModDef
{
    std::string name;
    std::function<ModInst (double, int)> make;
    int silence = 0;            // 0: exact zero, 1: |out| <= 1e-4 (P9), 2: no claim
    std::function<cd (double, double)> H;   // linear modules: H (w, fs)
    bool dcRemoves = false;
};

std::vector<ModDef> extremeModules()
{
    std::vector<ModDef> v;
    for (int st : { 1, 4 })
        v.push_back ({ "os" + std::to_string (1 << st) + "x", [st] (double, int mb)
        {
            auto os = std::make_shared<StereoOversampling> (st);
            os->initProcessing ((size_t) mb);
            return ModInst { [os] (float* l, float* r, int n) { osProcess (*os, l, r, n); }, os };
        }, 0, [st] (double w, double) { return osG (osStages (st), w, nullptr); }, false });
    auto dist = [] (std::string name, int mode, float drv, int os, bool dc, float chr, float off)
    {
        return ModDef { name, [=] (double fs, int mb)
        {
            auto s = distBase (mode, drv, os);
            s.dcFilter = dc; s.character = chr; s.offset = off;
            std::shared_ptr<ClipDistortion> d (makeDist (fs, s, mb).release());
            return ModInst { [d] (float* l, float* r, int n) { d->process (l, r, n); }, d };
        }, 0, nullptr, dc };
    };
    v.push_back (dist ("dist Tanh d48 4x DC", ClipDistortion::Tanh, 48.0f, 2, true, 0.0f, 0.0f));
    v.push_back (dist ("dist TriFold d48 OSoff", ClipDistortion::TriFold, 48.0f, 0, false, 0.0f, 0.0f));
    v.push_back (dist ("dist Bitcrush d24 Raw", ClipDistortion::Bitcrush, 24.0f, 5, false, 0.0f, 0.0f));
    v.push_back (dist ("dist HardClip chr1 off0.5 16x", ClipDistortion::HardClip, 24.0f, 4, false, 1.0f, 0.5f));
    auto eqm = [] (std::string name, eq::BandSettings b)
    {
        return ModDef { name, [b] (double fs, int)
        {
            auto e = std::make_shared<eq::ParametricEQ<Params::numBands>>();
            e->prepare (fs);
            e->setBand (0, b);
            return ModInst { [e] (float* l, float* r, int n) { e->process (l, r, n); }, e };
        }, 0, [b] (double w, double fs) { return Hc (ref::design (b, fs), w); }, false };
    };
    v.push_back (eqm ("eq Bell +30 Q18", band (eq::BandType::Bell, 1000, 30, 18)));
    v.push_back (eqm ("eq LowCut 96dB 20Hz", band (eq::BandType::LowCut, 20, 0, 0.707f, 7)));
    auto satm = [] (std::string name, int style, int sil)
    {
        return ModDef { name, [style] (double fs, int mb)
        {
            auto s = satBase (3, 2, 1);
            for (auto& b : s.bands) { b.style = style; b.driveDb = 36.0f; }
            std::shared_ptr<sat::MultibandSaturator> m (makeSat (fs, s, mb).release());
            return ModInst { [m] (float* l, float* r, int n) { m->process (l, r, n); }, m };
        }, sil, nullptr, true };
    };
    v.push_back (satm ("sat Driven Tube d36", 2, 1));
    v.push_back (satm ("sat Iron Transformer d36", 7, 1));
    v.push_back (satm ("sat Foldback d36", 13, 0));
    v.push_back ({ "comp Clean ratio100 thr-40 2 bands", [] (double fs, int)
    {
        auto s = compBase (comp::Clean, -40.0f, 100.0f, 0.0f);
        s.numBands = 2;
        for (auto& b : s.bands) b.rangeDb = 60.0f;
        std::shared_ptr<comp::MultibandCompressor> c (new comp::MultibandCompressor());
        c->setSettings (s);
        c->prepare (fs);
        return ModInst { [c] (float* l, float* r, int n) { c->process (l, r, n); }, c };
    }, 0, nullptr, false });
    v.push_back ({ "clipper soft1 ceil-12", [] (double fs, int mb)
    {
        auto c = std::make_shared<OutputClipper>();
        c->prepare (fs, mb);
        c->setParameters (true, -12.0f, 1.0f);
        return ModInst { [c] (float* l, float* r, int n) { c->process (l, r, n); }, c };
    }, 0, nullptr, false });
    v.push_back ({ "reverb Plate decay20 extreme", [] (double fs, int)
    {
        auto s = revBase (4);
        s.decay = 20; s.bassMult = 4; s.highMult = 2; s.bassXover = 2000; s.highXover = 1000; s.lSize = 0;
        std::shared_ptr<space::SpaceReverb> r (makeRev (fs, s).release());
        return ModInst { [r] (float* l, float* rr, int n) { r->process (l, rr, n); }, r };
    }, 0, nullptr, false });
    return v;
}

void suiteExt()
{
    auto& su = S ("ext");
    auto& gF = su.g ("finite_outputs", "count");
    auto& gZ = su.g ("silence_in->exact_zero_out", "abs");
    auto& gP9 = su.g ("P9_silence_in->|out|<=1e-4", "abs");
    auto& gT = su.g ("1e-30_linear:exact_or_flushed", "rel");
    auto& gN = su.g ("nyquist_response=|H(pi)|", "abs");
    auto& gD = su.g ("dc_response=H(0)", "abs");
    auto& gDr = su.g ("dc_removed_by_dc_filter", "abs");
    auto& gS = su.g ("+24dBFS_DC_drive48=static_curve", "abs");
    tolNote ("ext +24 dBFS DC through drive 48 (|v| ~ 4000): tolerance max(1e-5 max(1,|y|), the 3a1 static-curve bound); at |v| ~ 4000 the float "
             "argument itself is quantised to 2.4e-4, which the plan's flat 1e-5 did not account for (the folds failed by 1.1e-5 / 2.5e-5 in "
             "the first run; changed after the first run, listed here).");
    auto& gB = su.g ("+24dBFS_DC_drive48_bounded", "abs");
    const auto mods = extremeModules();
    const char* inputs[] = { "DC+1", "DC-1", "Nyquist", "silence", "sine+24dBFS@20", "sine+24dBFS@100", "sine+24dBFS@1k", "sine+24dBFS@10k",
                             "sine+24dBFS@0.45fs", "sine1e-30@1k", "DC1e-30", "step0->+24dBFS" };
    constexpr int nIn = 12;
    const std::vector<int> blocks { 1, 7, 64, 256 };
    struct J { size_t mod; int in; double fs; int blk; };
    std::vector<J> jobs;
    for (size_t m = 0; m < mods.size(); ++m) for (int in = 0; in < nIn; ++in) for (double fs : kRates) for (int b : blocks)
        if (! gQuick || (b == 64 && fs == 48000.0)) jobs.push_back ({ m, in, fs, b });
    std::mutex tm;
    std::map<std::string, int> tinyKind;
    parallelFor ((int) jobs.size(), [&] (int i)
    {
        const auto& j = jobs[(size_t) i];
        const auto& md = mods[j.mod];
        auto inst = md.make (j.fs, 256);
        const bool lin = (bool) md.H;
        int len = (int) (0.25 * j.fs);
        if (lin && (j.in <= 2 || j.in == 9 || j.in == 10)) len = (int) (2.5 * j.fs);   // linear steady-state checks: slowest pole settled (2 s), measure the last 0.5 s
        std::vector<float> x ((size_t) len, 0.0f);
        const double big = std::pow (10.0, 24.0 / 20.0);
        const double sf[] = { 20.0, 100.0, 1000.0, 10000.0, 0.45 * j.fs };
        for (int n = 0; n < len; ++n)
        {
            double v = 0.0;
            switch (j.in)
            {
                case 0: v = 1.0; break;
                case 1: v = -1.0; break;
                case 2: v = (n & 1) ? -1.0 : 1.0; break;
                case 3: v = 0.0; break;
                case 4: case 5: case 6: case 7: case 8: v = big * std::sin (kTwoPi * sf[j.in - 4] * n / j.fs); break;
                case 9: v = 1e-30 * std::sin (kTwoPi * 1000.0 * n / j.fs); break;
                case 10: v = 1e-30; break;
                case 11: v = n < (int) (0.05 * j.fs) ? 0.0 : big * std::sin (kTwoPi * 1000.0 * n / j.fs); break;
                default: break;
            }
            x[(size_t) n] = (float) v;
        }
        auto l = x, r = x;
        runBlocks (inst.fn, l.data(), r.data(), len, j.blk, 256, i);
        Ctx c { j.fs, j.blk, 0, 0, md.name + " in=" + inputs[j.in], newSine() };
        int bad = 0;
        for (int n = 0; n < len; ++n) if (! std::isfinite (l[(size_t) n]) || ! std::isfinite (r[(size_t) n])) ++bad;
        rec (gF, c, 0, bad, bad, 0.0);   // any NaN / Inf fails
        double pk = 0;
        for (int n = 0; n < len; ++n) pk = std::max ({ pk, (double) std::abs (l[(size_t) n]), (double) std::abs (r[(size_t) n]) });
        if (j.in == 3)
        {
            if (md.silence == 0) rec (gZ, c, 0, pk, pk, 0.0);
            else if (md.silence == 1) recBound (gP9, c, pk, 1e-4, true);
        }
        if (lin && (j.in == 0 || j.in == 1 || j.in == 2))
        {
            const double w = j.in == 2 ? kPi : 0.0;
            const double he = std::abs (md.H (w, j.fs));
            double tail = 0;
            for (int n = len - 64; n < len; ++n) tail = std::max (tail, (double) std::abs (l[(size_t) n]));
            rec (j.in == 2 ? gN : gD, c, he, tail, tail - he, 1e-5 + 1e-4 * he);
        }
        if (md.dcRemoves && (j.in == 0 || j.in == 1))
        {
            const double tail = std::abs ((double) l[(size_t) len - 1]);
            rec (gDr, c, 0, tail, tail, 1e-5);
        }
        if (lin && (j.in == 9 || j.in == 10))
        {
            const double he = std::abs (md.H (j.in == 9 ? kTwoPi * 1000.0 / j.fs : 0.0, j.fs));
            const int h = len - (int) (0.5 * j.fs);
            const double amp = j.in == 9 ? std::abs (toneBH (l.data() + h, len - h, kTwoPi * 1000.0 / j.fs)) : std::abs ((double) l[(size_t) len - 1]);
            bool allZero = true;
            for (int n = h; n < len; ++n) allZero = allZero && l[(size_t) n] == 0.0f;
            // H ~ 0 (a low cut at DC): the expected output is 0; compare the absolute level against 1e-6 of the input instead
            const double rel = he < 1e-6 ? amp / 1e-30 : amp / (1e-30 * he) - 1.0;
            const bool exact = std::abs (rel) <= 1e-6;
            Ctx cx = c;
            cx.params += allZero ? " -> flushed to 0" : (exact ? " -> exact" : " -> neither (rel " + num (rel, 4) + ")");
            rec (gT, cx, 1e-30 * he, amp, allZero ? 0.0 : rel, 1e-6);
            const std::lock_guard<std::mutex> lk (tm);
            tinyKind[md.name + (allZero ? " flushed" : (exact ? " exact" : " NEITHER"))]++;
        }
    });
    {
        std::ostringstream o;
        o << "ext 1e-30 inputs through linear modules: ";
        for (auto& [k, n] : tinyKind) o << k << " x" << n << "; ";
        note (o.str());
    }
    // +24 dBFS DC through drive 48: far outside the +-16 ADAA table (linear extension / fmod wrap): static curve, bounded
    for (int mode = 0; mode < ClipDistortion::numModes; ++mode)
        for (double sgn : { 1.0, -1.0 })
        {
            const double fs = 48000.0, X = sgn * std::pow (10.0, 24.0 / 20.0);
            auto d = makeDist (fs, distBase (mode, 48.0f, 0), 256);
            std::vector<float> l (4096, (float) X), r = l;
            runBlocks (distFn (*d), l.data(), r.data(), 4096, 256, 256);
            const double ex = myTransfer (mode, (float) X, juce::Decibels::decibelsToGain (48.0f), 0.0, 0.0);
            Ctx c { fs, 256, 0, X, std::string ("mode=") + ClipDistortion::modeNames()[mode].toStdString(), newSine() };
            rec (gS, c, ex, l[4095], l[4095] - ex, std::max (1e-5 * std::max (1.0, std::abs (ex)), staticTol (mode, (float) X, juce::Decibels::decibelsToGain (48.0f), 0.0, 0.0)));
            recBound (gB, c, std::abs ((double) l[4095]), 1.0 + 1e-6, true);
        }
}

void suiteDen()
{
    auto& su = S ("den");
    auto& gNo = su.g ("no_FTZ.subnormal_outputs_in_tail(info)", "count");
    auto& gT = su.g ("no_FTZ.tail/loud_ns_per_sample(info, >3x=stall)", "ratio");
    auto& gF = su.g ("FTZ.subnormal_outputs", "count");
    auto& gNaN = su.g ("P10_NaN_injection_recovers(info, 1=yes)", "bool");
    std::vector<ModDef> mods;
    {
        auto all = extremeModules();
        mods.push_back (all[0]);
        mods.push_back (all[2]);
        mods.push_back (all[6]);
        mods.push_back (all[7]);
        mods.push_back ({ "sat 3 bands Warm Tube 2x", [] (double fs, int mb)
        {
            auto s = satBase (3, 2, 1);
            for (auto& b : s.bands) { b.style = 1; b.driveDb = 6.0f; }
            std::shared_ptr<sat::MultibandSaturator> m (makeSat (fs, s, mb).release());
            return ModInst { [m] (float* l, float* r, int n) { m->process (l, r, n); }, m };
        }, 0, nullptr, true });
        mods.push_back ({ "comp Clean 2 bands", [] (double fs, int)
        {
            auto s = compBase (comp::Clean, -18.0f, 4.0f, 6.0f);
            s.numBands = 2;
            std::shared_ptr<comp::MultibandCompressor> c (new comp::MultibandCompressor());
            c->setSettings (s);
            c->prepare (fs);
            return ModInst { [c] (float* l, float* r, int n) { c->process (l, r, n); }, c };
        }, 0, nullptr, false });
        mods.push_back (all[12]);
        mods.push_back (all[13]);
    }
    const double fs = 48000.0;
    struct J { size_t m; bool ftz; };
    std::vector<J> jobs;
    for (size_t m = 0; m < mods.size(); ++m) { jobs.push_back ({ m, false }); jobs.push_back ({ m, true }); }
    parallelFor ((int) jobs.size(), [&] (int i)
    {
        const auto& j = jobs[(size_t) i];
        std::unique_ptr<juce::ScopedNoDenormals> nd;
        if (j.ftz) nd = std::make_unique<juce::ScopedNoDenormals>();
        auto inst = mods[j.m].make (fs, 256);
        const int loud = (int) fs, total = loud + (int) ((gQuick ? 3.0 : 10.0) * fs);
        auto l = coherentSine (oddBin (1000.0, fs, NA), NA, 1.0, 0.0, total);
        for (int n = loud; n < total; ++n) l[(size_t) n] = 0.0f;
        auto r = l;
        double tLoud = 0, tTail = 0;
        for (int pos = 0; pos < total; pos += 256)
        {
            const double t0 = threadCpu();
            inst.fn (l.data() + pos, r.data() + pos, std::min (256, total - pos));
            const double dt = threadCpu() - t0;
            (pos < loud ? tLoud : tTail) += dt;
        }
        int sub = 0;
        for (int n = loud; n < total; ++n)
            if (std::fpclassify (l[(size_t) n]) == FP_SUBNORMAL || std::fpclassify (r[(size_t) n]) == FP_SUBNORMAL) ++sub;
        Ctx c { fs, 256, 1000, 1, mods[j.m].name, newSine() };
        if (j.ftz) rec (gF, c, 0, sub, sub, 0.0);
        else
        {
            rec (gNo, c, 0, sub, sub, -1.0);
            const double ratio = (tTail / (total - loud)) / std::max (1e-12, tLoud / loud);
            rec (gT, c, 1.0, ratio, ratio, -1.0);
            if (ratio > 3.0) note ("den: " + mods[j.m].name + " runs " + num (ratio, 3) + "x slower per sample in the silent tail without FTZ (denormal stall)");
        }
        if (j.ftz)
        {
            auto inst2 = mods[j.m].make (fs, 256);
            const int len = (int) (1.5 * fs);
            auto a = coherentSine (oddBin (1000.0, fs, NA), NA, 0.5, 0.0, len);
            a[(size_t) (fs / 2)] = std::numeric_limits<float>::quiet_NaN();
            auto b = a;
            runBlocks (inst2.fn, a.data(), b.data(), len, 256, 256);
            int nan = 0;
            for (int n = len - (int) (0.5 * fs); n < len; ++n) if (! std::isfinite (a[(size_t) n])) ++nan;
            rec (gNaN, { fs, 256, 1000, 0.5, mods[j.m].name + " non-finite samples in the last 0.5 s: " + std::to_string (nan), newSine() }, 1, nan == 0 ? 1 : 0, nan == 0 ? 0 : 1, -1.0);
        }
    });
}

//==============================================================================
std::string statusLine (Group& g)
{
    std::ostringstream o;
    const bool info = g.checks == 0;
    o << "[" << (info ? "INFO" : (g.fails == 0 ? "PASS" : "FAIL")) << "] " << std::left << std::setw (5) << g.suite << " " << std::setw (58) << g.name
      << " sines=" << std::setw (5) << g.sines.size();
    if (info)
    {
        o << " rows=" << g.infos << (g.signedInfo ? "  max v=" : "  max|v|=") << num (g.infoWorst, 4) << " " << g.unit << " (" << num (g.infoVal, 5) << ")  @ " << g.infoWhere;
        return o.str();
    }
    o << " checks=" << std::setw (5) << g.checks << " fails=" << std::setw (4) << g.fails;
    if (g.kind == 0)
    {
        o << "  worst err=" << num (g.worstErr, 4) << " " << g.unit << " tol=" << num (g.worstTol, 3);
        if (g.worstRatio > 0) o << (g.fails ? "  (" : "  margin=") << (g.fails ? num (g.worstRatio, 3) + "x tol)" : num (1.0 / g.worstRatio, 3) + "x");
        else o << "  margin=inf";
    }
    else
        o << "  worst=" << num (g.worstErr, 5) << " " << g.unit << (g.kind == 1 ? " bound<=" : " bound>=") << num (g.worstTol, 5)
          << (g.fails ? "  excess=" : "  margin=") << num (std::abs (g.worstRatio), 4);
    if (g.infos > 0) o << "  (+" << g.infos << " info rows)";
    o << "  @ " << g.worstWhere;
    return o.str();
}

void writeOsJson()
{
    std::ofstream o (gOut.getChildFile ("os_coeffs.json").getFullPathName().toStdString());
    o << std::setprecision (17) << "{\n";
    for (int st = 1; st <= 4; ++st)
    {
        o << "  \"" << st << "\": [";
        for (size_t s = 0; s < osStages (st).size(); ++s)
        {
            const auto& x = osStages (st)[s];
            auto arr = [&o] (const std::vector<double>& v) { o << "["; for (size_t i = 0; i < v.size(); ++i) o << (i ? "," : "") << v[i]; o << "]"; };
            o << (s ? "," : "") << "{\"upD\":"; arr (x.upD); o << ",\"upE\":"; arr (x.upE); o << ",\"dnD\":"; arr (x.dnD); o << ",\"dnE\":"; arr (x.dnE); o << "}";
        }
        o << "]" << (st < 4 ? "," : "") << "\n";
    }
    o << "}\n";
}
} // namespace sl

int main (int argc, char** argv)
{
    using namespace sl;
    juce::ScopedJuceInitialiser_GUI init;
    juce::ScopedNoDenormals noDenormals;
    if (argc < 2) { std::cout << "usage: CodexSineLab <outdir> [--suites osc,os,dist,eq,sat,comp,clip,rev,proc,ext,den] [--threads N] [--quick]\n"; return 2; }
    gOut = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    gOut.createDirectory();
    std::set<std::string> want;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--quick") gQuick = true;
        else if (a == "--threads" && i + 1 < argc) gThreads = std::max (1, std::atoi (argv[++i]));
        else if (a == "--suites" && i + 1 < argc) { std::stringstream ss (argv[++i]); std::string t; while (std::getline (ss, t, ',')) want.insert (t); }
    }
    notDone ("osc triangle: spectrum vs the folded 8/(pi^2 m^2) series (the per-sample check against the exact sampled triangle is run instead; "
             "it determines the spectrum completely).");
    notDone ("osc 909: per-sample sin(tri pi/2) == sin(2 pi p) identity for shape = 1 (only the tail-frequency check is run).");
    notDone ("os: separate assertion against the design stopband (-90 / -75 dB + 10 dB per stage); image / alias levels are compared with the "
             "analytic composite response instead (+-1 dB).");
    notDone ("dist Downsample: zero-order-hold image spectrum (the hold pattern is checked per sample instead, index-exact).");
    notDone ("den: subnormal counts of internal states (private); only outputs are classified.");
    gOs.resize (5);
    for (int st = 1; st <= 4; ++st) gOs[(size_t) st] = makeOsStages (st);
    tw();
    writeOsJson();

    const std::vector<std::pair<std::string, std::function<void()>>> suites {
        { "osc", suiteOsc }, { "os", suiteOs }, { "dist", suiteDist }, { "eq", suiteEq }, { "sat", suiteSat }, { "comp", suiteComp },
        { "clip", suiteClip }, { "rev", suiteRev }, { "proc", suiteProc }, { "ext", suiteExt }, { "den", suiteDen } };
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& [name, fn] : suites)
    {
        if (! want.empty() && ! want.count (name)) continue;
        const auto a = std::chrono::steady_clock::now();
        auto& s = S (name);
        std::cout << "== " << name << " ..." << std::flush;
        fn();
        s.seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - a).count();
        s.ran = true;
        std::cout << " " << num (s.seconds, 3) << " s" << std::endl;
    }
    const double total = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

    std::ostringstream out;
    long sines = gSineCounter.load(), groups = 0, failed = 0, checks = 0, fails = 0;
    out << "Codex SineLab: " << sines << " sine / signal runs, " << gThreads << " threads, " << num (total, 4) << " s" << (gQuick ? " (QUICK)" : "") << "\n\n";
    for (auto& name : gSuiteOrder)
    {
        auto& s = *gSuites[name];
        out << "---- " << name << " (" << num (s.seconds, 3) << " s)\n";
        for (auto& g : s.groups)
        {
            out << statusLine (*g) << "\n";
            ++groups;
            checks += g->checks;
            fails += g->fails;
            if (g->fails) ++failed;
        }
        s.csv.flush();
    }
    out << "\nTOTAL: " << groups << " groups, " << checks << " checks, " << fails << " failed checks in " << failed << " failing groups.\n";
    out << "\nFindings / predictions:\n";
    for (auto& n : gNotes) out << "  - " << n << "\n";
    out << "\nTolerance / reference notes (each says whether it was fixed before the first run or changed after it):\n";
    for (auto& n : gTolNotes) out << "  - " << n << "\n";
    out << "\nPlan items not executed:\n";
    for (auto& n : gNotDone) out << "  - " << n << "\n";
    std::cout << "\n" << out.str();
    std::ofstream (gOut.getChildFile ("summary.txt").getFullPathName().toStdString()) << out.str();
    gSuites.clear();
    return fails == 0 ? 0 : 1;
}
