#pragma once
#include "Widgets.h"

/**
    Easter egg: click the book in the header and a dancer joins you. Every kick moves it to the next pose with a
    bounce; between kicks it sways. Click the book again and it leaves. An original figure drawn
    in Codex's own style.
*/
class Dancer : public juce::Component, private juce::Timer
{
public:
    explicit Dancer (std::atomic<unsigned int>& hits) : hitCounter (hits)
    {
        lastHits = hitCounter.load();
        setInterceptsMouseClicks (false, false);   // never in the way of the controls under it
    }

    void toggle()
    {
        if (isVisible() && targetAlpha > 0.5f) { targetAlpha = 0.0f; return; }
        targetAlpha = 1.0f;
        alpha = std::max (alpha, 0.0f);
        lastHits = hitCounter.load();
        setVisible (true);
        toFront (false);
        startTimerHz (60);
    }

    bool isDancing() const { return isVisible() && targetAlpha > 0.5f; }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        const auto r = getLocalBounds().toFloat();
        const float unit = r.getHeight() * 0.33f;
        const auto pose = currentPose();
        const Point<float> hip { r.getCentreX(), r.getY() + r.getHeight() * 0.53f + pose[10] * unit };
        auto dir = [] (float a) { return Point<float> (std::sin (a), std::cos (a)); };   // 0 = straight down

        // a dark stage behind the figure, so the controls under it don't show through
        g.setGradientFill (ColourGradient (Colours_::background.withAlpha (0.85f * alpha), r.getCentreX(), r.getCentreY(),
                                           Colours_::background.withAlpha (0.0f), r.getCentreX() + r.getWidth() * 0.5f, r.getCentreY(), true));
        g.fillEllipse (r);

        // spotlight and floor, pulsing with the kick
        const float glow = 0.18f + 0.3f * bounce;
        g.setGradientFill (ColourGradient (Colours_::accent.withAlpha (glow * alpha), r.getCentreX(), hip.y,
                                           Colours_::accent.withAlpha (0.0f), r.getCentreX() + r.getWidth() * 0.5f, hip.y, true));
        g.fillEllipse (r.reduced (r.getWidth() * 0.04f));
        const float floorY = r.getY() + r.getHeight() * 0.53f + unit * 1.12f;
        g.setColour (Colours_::accent.withAlpha ((0.25f + 0.4f * bounce) * alpha));
        g.fillEllipse (Rectangle<float> (unit * (1.0f + 0.25f * bounce), unit * 0.12f).withCentre ({ r.getCentreX(), floorY }));

        // skeleton
        const float torsoLen = unit * 0.62f;
        const Point<float> neck = hip + Point<float> (std::sin (pose[0]), -std::cos (pose[0])) * torsoLen;
        const Point<float> shoulder = hip + (neck - hip) * 0.8f;
        const float upperArm = unit * 0.32f, foreArm = unit * 0.3f, thigh = unit * 0.42f, shin = unit * 0.42f;
        auto limb = [&] (Point<float> from, float a1, float len1, float a2, float len2)
        {
            const auto joint = from + dir (a1) * len1;
            const auto end = joint + dir (a1 + a2) * len2;
            Path p;
            p.startNewSubPath (from);
            p.lineTo (joint);
            p.lineTo (end);
            return p;
        };
        Path body;
        body.startNewSubPath (hip);
        body.lineTo (neck);
        body.addPath (limb (shoulder, pose[2], upperArm, pose[3], foreArm));
        body.addPath (limb (shoulder, pose[4], upperArm, pose[5], foreArm));
        body.addPath (limb (hip, pose[6], thigh, pose[7], shin));
        body.addPath (limb (hip, pose[8], thigh, pose[9], shin));

        const float w = unit * 0.11f;
        const auto col = Colours_::accent.interpolatedWith (Colours_::accentHi, bounce);
        g.setColour (col.withAlpha (0.18f * alpha));
        g.strokePath (body, PathStrokeType (w * 2.4f, PathStrokeType::curved, PathStrokeType::rounded));
        g.setColour (col.withAlpha (alpha));
        g.strokePath (body, PathStrokeType (w, PathStrokeType::curved, PathStrokeType::rounded));

        const float headR = unit * 0.17f;
        const auto head = neck + Point<float> (std::sin (pose[0] + pose[1]), -std::cos (pose[0] + pose[1])) * (headR * 1.45f);
        g.setColour (col.withAlpha (0.18f * alpha));
        g.fillEllipse (Rectangle<float> (headR * 2.9f, headR * 2.9f).withCentre (head));
        g.setColour (col.withAlpha (alpha));
        g.fillEllipse (Rectangle<float> (headR * 2.0f, headR * 2.0f).withCentre (head));
    }

private:
    // lean, head tilt, upper arm L, forearm L (relative), upper arm R, forearm R, thigh L, shin L, thigh R, shin R, hip drop
    using Pose = std::array<float, 11>;

    static const std::array<Pose, 6>& poses()
    {
        static const std::array<Pose, 6> p { {
            {  0.00f,  0.00f, -2.60f, -0.20f,  2.60f,  0.20f, -0.25f,  0.00f,  0.25f,  0.00f, 0.00f },   // arms up, V
            {  0.15f,  0.20f, -0.55f, -1.60f,  2.40f,  0.00f, -0.10f,  0.00f,  0.55f, -0.70f, 0.03f },   // point up, knee kick
            { -0.05f, -0.10f, -1.20f,  1.40f,  1.20f, -1.40f, -0.50f,  0.65f,  0.50f, -0.65f, 0.10f },   // pump, squat
            { -0.15f, -0.20f, -2.40f,  0.00f,  0.55f,  1.60f, -0.55f,  0.70f,  0.10f,  0.00f, 0.03f },   // mirror: point up, knee kick
            {  0.00f,  0.00f, -2.95f,  0.70f,  2.95f, -0.70f, -0.08f,  0.00f,  0.08f,  0.00f, 0.02f },   // hands together above
            {  0.10f,  0.15f, -1.55f,  0.00f,  1.55f,  0.00f, -0.40f,  0.30f,  0.40f, -0.30f, 0.06f },   // arms out
        } };
        return p;
    }

    Pose currentPose() const
    {
        const auto& a = poses()[(size_t) from];
        const auto& b = poses()[(size_t) to];
        const float t = 1.0f - std::pow (1.0f - juce::jlimit (0.0f, 1.0f, blend), 3.0f);   // ease out
        Pose p {};
        for (size_t i = 0; i < p.size(); ++i) p[i] = a[i] + (b[i] - a[i]) * t;
        // idle sway between kicks, and the bounce of the hit
        const float sway = std::sin (clock * 2.6f) * idle;
        p[0] += 0.08f * sway;
        p[2] += 0.25f * sway; p[4] += 0.25f * sway;
        p[10] += 0.05f * bounce + 0.02f * std::abs (sway);
        return p;
    }

    void timerCallback() override
    {
        constexpr float dt = 1.0f / 60.0f;
        clock += dt;
        const unsigned int hits = hitCounter.load (std::memory_order_relaxed);
        if (hits != lastHits)
        {
            lastHits = hits;
            from = to;
            to = (to + 1 + (int) (hits % 2)) % (int) poses().size();
            blend = 0.0f;
            bounce = 1.0f;
            sinceHit = 0.0f;
        }
        blend = std::min (1.0f, blend + dt / 0.14f);
        bounce *= 0.9f;
        sinceHit += dt;
        idle += ((sinceHit > 1.5f ? 1.0f : 0.0f) - idle) * 0.05f;   // starts swaying once the kicks stop

        alpha += (targetAlpha - alpha) * 0.18f;
        if (targetAlpha < 0.5f && alpha < 0.02f)
        {
            alpha = 0.0f;
            setVisible (false);
            stopTimer();
            return;
        }
        repaint();
    }

    std::atomic<unsigned int>& hitCounter;
    unsigned int lastHits = 0;
    int from = 0, to = 0;
    float blend = 1.0f, bounce = 0.0f, clock = 0.0f, sinceHit = 10.0f, idle = 1.0f, alpha = 0.0f, targetAlpha = 0.0f;
};
