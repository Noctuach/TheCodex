#pragma once
#include "AdaaTable.h"
#include "CustomCurve.h"
#include <atomic>
#include <array>

/** One compiled custom curve, ready for the audio thread. */
struct CustomCurveSlot
{
    using Table = BasicAdaaTable<4>;   // +-1: the drawn domain (the curve is flat beyond, so the linear extension is exact)

    bool valid = false;
    FreeCurve curve;
    float reach = 1.0f, invReach = 1.0f;
    Table tbl;

    /** Curve value at a shaper input v (v = u * reach). */
    float shapeV (float v) const noexcept { return tbl.shapeAt ((double) v * invReach); }
    /** Antiderivative in v: F_v(v) = R * G(v / R), G the integral of the drawn curve. */
    double Fv (double v) const noexcept { return (double) reach * tbl.eval (v * (double) invReach); }
};

/**
    Hands compiled custom curves from the editor (or a state restore) to the audio thread without locks or allocation
    on the audio side. Four preallocated slots: the reader owns "front" (playing) and "prev" (being crossfaded out),
    the writer owns "back", and "middle" is exchanged atomically (with a dirty bit) between them. The writer can
    therefore never touch a slot the audio thread is reading.

    publish(): any non-audio thread (serialised by a lock the audio thread never takes).
    tryAcquire(): audio thread only, and only while no crossfade is running (the prev slot must be free).
*/
class CustomCurveBank
{
public:
    CustomCurveBank()
    {
        for (auto& s : slots)
        {
            s.valid = false;
            s.tbl.build ([] (float u) { return std::clamp (u, -1.0f, 1.0f); }, false);   // allocates here, never later
        }
    }

    /** nullptr = no custom curve. A no-op when nothing changed since the last publish. */
    void publish (const FreeCurve* c)
    {
        const juce::ScopedLock sl (writeLock);
        const bool valid = c != nullptr;
        if (published && valid == lastValid && (! valid || *c == lastCurve)) return;
        auto& s = slots[(size_t) back];
        s.valid = valid;
        if (valid)
        {
            s.curve = *c;
            s.reach = c->reach;
            s.invReach = 1.0f / c->reach;
            const FreeCurveShape shape (*c);
            s.tbl.build ([&shape] (float u) { return shape.evalU (u); }, false);
            lastCurve = *c;
        }
        lastValid = valid;
        published = true;
        back = middle.exchange (back | dirty, std::memory_order_acq_rel) & 3;
    }

    /** Takes the newest published slot, if any. front / prev are the reader's slot indices. */
    bool tryAcquire (int& front, int& prev) noexcept
    {
        if ((middle.load (std::memory_order_acquire) & dirty) == 0) return false;
        const int in = middle.exchange (prev, std::memory_order_acq_rel) & 3;
        prev = front;
        front = in;
        return true;
    }

    const CustomCurveSlot& slot (int i) const noexcept { return slots[(size_t) (i & 3)]; }

private:
    std::array<CustomCurveSlot, 4> slots;
    std::atomic<int> middle { 2 };
    int back = 3;                    // reader starts with front = 0, prev = 1
    juce::CriticalSection writeLock; // the audio thread never touches it
    bool published = false, lastValid = false;
    FreeCurve lastCurve;
    static constexpr int dirty = 4;
};
