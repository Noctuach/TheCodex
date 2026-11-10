#include "PluginProcessor.h"
#include "GUI/PluginEditor.h"

CodexProcessor::CodexProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "CodexState", Params::createLayout())
{
    startTimerHz (20);   // applies latency changes to the host from the message thread
    engine = p (ID::engine); model909 = p (ID::model909); tune909 = p (ID::tune909);
    attack909 = p (ID::attack909); decay909 = p (ID::decay909);
    key = p (ID::key); octave = p (ID::octave); keyMode = p (ID::keyMode);

    wave = p (ID::wave); tune = p (ID::tune); phase = p (ID::phase);
    clickLevel = p (ID::clickLevel); clickDecay = p (ID::clickDecay); clickTone = p (ID::clickTone);

    pAmount = p (ID::pAmount); pAttack = p (ID::pAttack); pDecay = p (ID::pDecay);
    pSustain = p (ID::pSustain); pRelease = p (ID::pRelease); pCurve = p (ID::pCurve);

    aAttack = p (ID::aAttack); aDecay = p (ID::aDecay); aSustain = p (ID::aSustain);
    aRelease = p (ID::aRelease); aCurve = p (ID::aCurve);

    outGainParam = p (ID::outGain);
    subOnParam = p (ID::subOn); subLevelParam = p (ID::subLevel); subLpParam = p (ID::subLowPass);
    subFadeParam = p (ID::subFade); subPhaseParam = p (ID::subPhase); velocityParam = p (ID::velocity);
    clipOnParam = p (ID::clipOn); clipCeilParam = p (ID::clipCeiling); clipSoftParam = p (ID::clipSoft);
    eqScaleParam = p (ID::eqScale);

    for (int s = 0; s < 2; ++s)
    {
        auto c = [&] (const char* sfx) { return p (ID::clip (s + 1, sfx)); };
        clipParams[s] = { c ("On"), c ("Mode"), c ("Drive"), c ("Boost"), c ("Char"), c ("Bias"), c ("LoFilt"), c ("HiFilt"),
                          c ("ShelfF"), c ("ShelfG"), c ("Mix"), c ("Out"), c ("DC"), c ("OS"), c ("Spatial"),
                          c ("FltPos"), c ("FltType"), c ("FltFreq"), c ("FltRes") };
        for (int k = 0; k < ClipDistortion::numCurvePoints; ++k)
        {
            clipParams[s].xp[(size_t) k] = c (("XP" + juce::String (k + 1)).toRawUTF8());
            clipParams[s].xn[(size_t) k] = c (("XN" + juce::String (k + 1)).toRawUTF8());
        }
    }

    for (int e = 0; e < ID::numEqs; ++e)
    {
        for (int b = 0; b < Params::numBands; ++b)
            bandParams[e][b] = { p (ID::eqBand (e, b, "On")), p (ID::eqBand (e, b, "Type")), p (ID::eqBand (e, b, "Freq")), p (ID::eqBand (e, b, "Gain")),
                                 p (ID::eqBand (e, b, "Q")), p (ID::eqBand (e, b, "Slope")), p (ID::eqBand (e, b, "Place")) };
        eqScaleParams[e] = p (ID::eqScaleFor (e));
        eqOnParams[e] = p (ID::eqOn (e));
        eqModeParams[e] = p (ID::eqMode (e));
        eqResParams[e] = p (ID::eqRes (e));
    }

    {
        const char* spc[] = { "On", "Mode", "Mix", "PreDelay", "Decay", "HighCut", "LowCut", "Depth", "Width", "Duck",
                              "ESize", "ECross", "ESend", "EDiff", "EModRate", "EModDepth",
                              "LSize", "LCross", "LDiff", "LModRate", "LModDepth", "BassMult", "BassX", "HighMult", "HighX" };
        for (int i = 0; i < 25; ++i) spcParams[i] = p (ID::spc (spc[i]));
    }
    {
        const char* g[] = { "On", "Bands", "Style", "Look", "Hold", "ScHp", "Mix", "Out", "Auto", "Limit", "X1", "X2", "X3" };
        for (int i = 0; i < 13; ++i) cmpGlobal[i] = p (ID::cmp (g[i]));
        const char* bp[] = { "Dir", "Thr", "Ratio", "Knee", "Range", "Att", "Rel", "Gain", "Mute", "Solo", "Bypass" };
        for (int b = 0; b < comp::maxBands; ++b)
            for (int i = 0; i < 11; ++i) cmpBandParams[b][i] = p (ID::cmpBand (b, bp[i]));
    }
    for (int e = 0; e < 2; ++e)
    {
        const char env = e == 0 ? 'p' : 'a';
        drawModes[e] = p (ID::envMode (env));
        drawCounts[e] = p (ID::envPoints (env));
        for (int i = 0; i < DrawEnvelope::maxPoints; ++i)
        {
            drawT[e][i] = p (ID::envPoint (env, 'T', i));
            drawL[e][i] = p (ID::envPoint (env, 'L', i));
            drawB[e][i] = p (ID::envPoint (env, 'B', i));
        }
    }
    for (int m = 0; m < ID::numMacros; ++m) macros[m] = p (ID::macro (m));
    satOn = p (ID::sat ("On")); satBands = p (ID::sat ("Bands")); satSlope = p (ID::sat ("Slope"));
    satOs = p (ID::sat ("OS")); satMix = p (ID::sat ("Mix")); satOut = p (ID::sat ("Out"));
    satIn = p (ID::sat ("In")); satMode = p (ID::sat ("Mode"));
    for (int j = 0; j < sat::maxBands - 1; ++j)
        satX[(size_t) j] = p (ID::sat (("X" + juce::String (j + 1)).toRawUTF8()));
    for (int b = 0; b < sat::maxBands; ++b)
    {
        auto c = [&] (const char* sfx) { return p (ID::satBand (b, sfx)); };
        satBandParams[b] = { c ("Style"), c ("Drive"), c ("Mix"), c ("Fb"), c ("FbFreq"), c ("Dyn"),
                             c ("Bass"), c ("MidT"), c ("Treble"), c ("Pres"), c ("DrivePan"), c ("LevelPan"),
                             c ("Level"), c ("Mute"), c ("Solo"), c ("Bypass") };
    }

    undoManager.clearUndoHistory(); // start with a clean history
}

bool CodexProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void CodexProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    loadMeasurer.reset (sampleRate, juce::jmax (1, samplesPerBlock));
    // worst-case chain latency: two Maximum-resolution linear-phase EQs (~0.7 s each) + oversampling + lookahead
    subDelay.assign ((size_t) (sampleRate * 1.6) + 1, 0.0f);
    subDelayPos = 0;
    subLp1.reset(); subLp2.reset();
    subGain.reset (sampleRate, 0.02);
    for (auto* v : { &left, &right, &mid, &sub })
        v->assign ((size_t) chunkSize, 0.0f);
    scopePos = -1;

    voice.prepare (sampleRate);
    updateParameters();
    clip1.prepare (sampleRate, chunkSize);
    clip2.prepare (sampleRate, chunkSize);
    for (auto& e : equalisers) e.prepare (sampleRate, chunkSize);
    saturator.prepare (sampleRate, chunkSize);
    reverb.prepare (sampleRate);
    compressor.prepare (sampleRate);
    updateParameters();

    clipper.prepare (sampleRate, chunkSize);
    outGain.reset (sampleRate, 0.02);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outGainParam->load()));

    updateLatency();
}

int CodexProcessor::chainLatency() const
{
    return juce::roundToInt (clip1.getLatencyInSamples() + clip2.getLatencyInSamples() + saturator.getLatencyInSamples()
                             + compressor.getLatencyInSamples() + equalisers[0].getLatencyInSamples() + equalisers[1].getLatencyInSamples());
}

void CodexProcessor::updateLatency()
{
    const int latency = juce::roundToInt (clip1.getLatencyInSamples() + clip2.getLatencyInSamples()
                                          + saturator.getLatencyInSamples() + compressor.getLatencyInSamples()
                                          + equalisers[0].getLatencyInSamples() + equalisers[1].getLatencyInSamples()
                                          + clipper.getLatencyInSamples());
    // The host is told on the message thread (setLatencySamples notifies listeners and can lock);
    // when this already runs on the message thread (offline renders, tests) it is applied at once.
    latencyTarget.store (latency);
    if (juce::MessageManager::existsAndIsCurrentThread() && latency != getLatencySamples())
        setLatencySamples (latency);
}

void CodexProcessor::timerCallback()
{
    const int latency = latencyTarget.load();
    if (latency != getLatencySamples())
        setLatencySamples (latency);
}

int CodexProcessor::getKeyNote() const
{
    return Params::keyBaseNote + (int) key->load() + 12 * ((int) octave->load() - 1);
}

void CodexProcessor::updateParameters()
{
    KickVoice::Settings s;
    s.engine           = (int) engine->load();
    s.model909         = (int) model909->load();
    s.tune909          = tune909->load();
    s.attack909        = attack909->load();
    s.decay909         = decay909->load();
    s.wave             = (int) wave->load();
    s.tuneSemis        = tune->load();
    s.startPhaseDeg    = phase->load();
    // Macros (0.5 = neutral): applied on top of the parameters, so the preset itself stays untouched
    const float mPunch = macros[0]->load() - 0.5f, mDirt = macros[1]->load() - 0.5f, mTail = macros[2]->load() - 0.5f, mWidth = macros[3]->load() - 0.5f;
    const float punchAmount = std::exp2 (mPunch * 1.0f);          // pitch sweep depth x0.7 .. x1.4
    const float punchSpeed  = std::exp2 (-mPunch * 1.2f);         // faster sweep with more punch
    const float tailMul     = std::exp2 (mTail * 2.0f);           // decay / release x0.5 .. x2
    s.decay909         = juce::jlimit (0.0f, 1.0f, s.decay909 + mTail * 0.5f);
    s.attack909        = juce::jlimit (0.0f, 1.0f, attack909->load() + mPunch * 0.6f);
    s.pitchAmountSemis = juce::jlimit (0.0f, 96.0f, pAmount->load() * punchAmount);
    s.clickLevel       = juce::jlimit (0.0f, 1.0f, clickLevel->load() * (1.0f + mPunch * 2.0f));
    s.clickDecayMs     = clickDecay->load();
    s.clickToneHz      = clickTone->load();
    s.subFadeMs        = subFadeParam->load();
    s.subPhaseDeg      = subPhaseParam->load();
    voice.setSettings (s);

    voice.pitchEnv.setParameters (pAttack->load(), pDecay->load() * punchSpeed, pSustain->load(), pRelease->load(), pCurve->load());
    voice.pitchDrawMode = drawModes[0]->load() > 0.5f;
    voice.ampDrawMode = drawModes[1]->load() > 0.5f && s.engine == KickVoice::SynthEngine;
    for (int e = 0; e < 2; ++e)
    {
        DrawEnvelope::Shape shape;
        shape.count = (int) drawCounts[e]->load();
        for (int i = 0; i < DrawEnvelope::maxPoints; ++i)
            shape.points[(size_t) i] = { drawT[e][i]->load() * (e == 0 ? punchSpeed : tailMul), drawL[e][i]->load(), drawB[e][i]->load() };
        (e == 0 ? voice.pitchDraw : voice.ampDraw).setShape (shape);
    }
    if (s.engine == KickVoice::Engine909 || voice.ampDrawMode) // amp ADSR is a gate: attack + release only
        voice.ampEnv.setParameters (voice.ampDrawMode ? 0.0f : aAttack->load(), 1.0f, 1.0f, aRelease->load() * tailMul, aCurve->load());
    else
        voice.ampEnv.setParameters (aAttack->load(), aDecay->load() * tailMul, aSustain->load(), aRelease->load() * tailMul, aCurve->load());

    ClipDistortion* clips[] = { &clip1, &clip2 };
    for (int i = 0; i < 2; ++i)
    {
        const auto& c = clipParams[i];
        ClipDistortion::Settings cs;
        cs.enabled      = c.on->load() > 0.5f;
        cs.mode         = (int) c.mode->load();
        cs.driveDb      = juce::jlimit (0.0f, 48.0f, c.drive->load() + mDirt * 24.0f);   // Dirt macro: +-12 dB drive
        cs.boostDb      = c.boost->load();
        cs.character    = c.chr->load();
        cs.offset       = c.bias->load();
        cs.loFilterHz   = c.loFilt->load();
        cs.hiFilterHz   = c.hiFilt->load();
        cs.shelfHz      = c.shelfF->load();
        cs.shelfDb      = c.shelfG->load();
        cs.mix          = c.mix->load();
        cs.outDb        = c.out->load();
        cs.dcFilter     = c.dc->load() > 0.5f;
        cs.oversampling = (int) c.os->load();
        // Width macro: right adds stereo distortion, left narrows it back to mono
        cs.spatial      = juce::jlimit (0.0f, 1.0f, c.spatial->load() * (1.0f + juce::jmin (0.0f, mWidth) * 2.0f) + juce::jmax (0.0f, mWidth) * 0.8f);
        cs.filterPos    = (int) c.fltPos->load();
        cs.filterType   = (int) c.fltType->load();
        cs.filterHz     = c.fltFreq->load();
        cs.filterRes    = c.fltRes->load();
        for (int k = 0; k < ClipDistortion::numCurvePoints; ++k)
        {
            cs.curve.pos[(size_t) k] = c.xp[(size_t) k]->load();
            cs.curve.neg[(size_t) k] = c.xn[(size_t) k]->load();
        }
        clips[i]->setSettings (cs);
    }

    for (int e = 0; e < ID::numEqs; ++e)
    for (int b = 0; b < Params::numBands; ++b)
    {
        const auto& bp = bandParams[e][b];
        eq::BandSettings bs;
        bs.on     = bp.on->load() > 0.5f && eqOnParams[e]->load() > 0.5f;
        bs.type   = (eq::BandType) (int) bp.type->load();
        bs.freq   = bp.freq->load();
        bs.gainDb = bp.gain->load();
        bs.q      = bp.q->load();
        bs.slope  = (int) bp.slope->load();
        bs.placement = (eq::Placement) (int) bp.place->load();
        if (eq::typeUsesGain (bs.type))
            bs.gainDb *= eqScaleParams[e]->load();   // Gain Scale
        equalisers[(size_t) e].setBand (b, bs);
        if (b == Params::numBands - 1)
            equalisers[(size_t) e].setMode ((int) eqModeParams[e]->load(), (int) eqResParams[e]->load());
    }

    sat::Settings ss;
    ss.enabled      = satOn->load() > 0.5f;
    ss.numBands     = juce::jlimit (1, sat::maxBands, (int) satBands->load());
    ss.slope        = (int) satSlope->load();
    ss.oversampling = (int) satOs->load();
    ss.mix          = satMix->load();
    ss.outDb        = satOut->load();
    ss.inDb         = satIn->load();
    ss.channelMode  = (int) satMode->load();
    for (int j = 0; j < sat::maxBands - 1; ++j)
        ss.crossovers[(size_t) j] = satX[(size_t) j]->load();
    for (int b = 0; b < sat::maxBands; ++b)
    {
        const auto& sp = satBandParams[b];
        auto& bs = ss.bands[(size_t) b];
        bs.style = (int) sp.style->load();
        bs.driveDb = sp.drive->load();
        bs.mix = sp.mix->load();
        bs.feedback = sp.fb->load();
        bs.feedbackHz = sp.fbFreq->load();
        bs.dynamics = sp.dyn->load();
        bs.bassDb = sp.bass->load();
        bs.midDb = sp.midT->load();
        bs.trebleDb = sp.treble->load();
        bs.presenceDb = sp.pres->load();
        bs.drivePan = sp.drivePan->load();
        bs.levelPan = sp.levelPan->load();
        bs.levelDb = sp.level->load();
        bs.mute = sp.mute->load() > 0.5f;
        bs.solo = sp.solo->load() > 0.5f;
        bs.bypass = sp.bypass->load() > 0.5f;
    }
    saturator.setSettings (ss);

    {
        auto v = [this] (int i) { return spcParams[i]->load(); };
        space::Settings rs;
        rs.enabled = v (0) > 0.5f; rs.mode = (int) v (1); rs.mix = v (2); rs.predelayMs = v (3); rs.decay = v (4);
        rs.highCut = v (5); rs.lowCut = v (6); rs.depth = v (7); rs.width = juce::jlimit (0.0f, 2.0f, v (8) * (1.0f + mWidth * 2.0f)); rs.duck = v (9);
        rs.eSize = v (10); rs.eCross = v (11); rs.eSend = v (12); rs.eDiffusion = v (13); rs.eModRate = v (14); rs.eModDepth = v (15);
        rs.lSize = v (16); rs.lCross = v (17); rs.lDiffusion = v (18); rs.lModRate = v (19); rs.lModDepth = v (20);
        rs.bassMult = v (21); rs.bassXover = v (22); rs.highMult = v (23); rs.highXover = v (24);
        reverb.setSettings (rs);
    }
    {
        auto g = [this] (int i) { return cmpGlobal[i]->load(); };
        comp::Settings cs;
        cs.enabled = g (0) > 0.5f; cs.numBands = (int) g (1); cs.style = (int) g (2); cs.lookahead = (int) g (3);
        cs.holdMs = g (4); cs.scHighPass = g (5); cs.mix = g (6); cs.outDb = g (7); cs.autoGain = g (8) > 0.5f; cs.limit = g (9) > 0.5f;
        for (int j = 0; j < comp::maxBands - 1; ++j) cs.crossovers[(size_t) j] = g (10 + j);
        for (int b = 0; b < comp::maxBands; ++b)
        {
            auto v = [this, b] (int i) { return cmpBandParams[b][i]->load(); };
            auto& bs = cs.bands[(size_t) b];
            bs.direction = (int) v (0); bs.thresholdDb = v (1); bs.ratio = v (2); bs.kneeDb = v (3); bs.rangeDb = v (4);
            bs.attackMs = v (5); bs.releaseMs = v (6); bs.gainDb = v (7); bs.mute = v (8) > 0.5f; bs.solo = v (9) > 0.5f; bs.bypass = v (10) > 0.5f;
        }
        compressor.setSettings (cs);
    }

    outGain.setTargetValue (juce::Decibels::decibelsToGain (outGainParam->load()));
    clipper.setParameters (clipOnParam->load() > 0.5f, clipCeilParam->load(), clipSoftParam->load());
    subGain.setTargetValue (subOnParam->load() > 0.5f ? juce::Decibels::decibelsToGain (subLevelParam->load()) : 0.0f);
    {
        const double sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;
        subLp1.set (sr, subLpParam->load());
        subLp2.set (sr, subLpParam->load());
    }

    // Oversampling and the saturation stage change the latency: report it to the host
    updateLatency();
}

void CodexProcessor::startScope()
{
    const float window = juce::jlimit (0.05f, 2.0f, scopeWindowSeconds.load());
    scopeCapturedSeconds.store (window);
    scopeSamplesPerPoint = juce::jmax (1, juce::roundToInt (getSampleRate() * window / scopePoints));
    scopePos = 0;
    scopeCount = 0;
    scopeCurMin.fill (1.0e9f);
    scopeCurMax.fill (-1.0e9f);
    scopeWritten.store (0);
    scopeGeneration.fetch_add (1);
}

void CodexProcessor::feedScope (float l, float r) noexcept
{
    const float v[] = { l, r, 0.5f * (l + r), 0.5f * (l - r) };
    for (size_t c = 0; c < numScopeChannels; ++c)
    {
        scopeCurMin[c] = std::min (scopeCurMin[c], v[c]);
        scopeCurMax[c] = std::max (scopeCurMax[c], v[c]);
    }
    if (++scopeCount >= scopeSamplesPerPoint)
    {
        for (size_t c = 0; c < numScopeChannels; ++c)
        {
            scopeMin[c][(size_t) scopePos] = scopeCurMin[c];
            scopeMax[c][(size_t) scopePos] = scopeCurMax[c];
        }
        scopeWritten.store (scopePos + 1);
        scopeCount = 0;
        scopeCurMin.fill (1.0e9f);
        scopeCurMax.fill (-1.0e9f);
        if (++scopePos >= scopePoints)
            scopePos = -1;
    }
}

void CodexProcessor::handleMidi (const juce::MidiMessage& m, int positionInChunk)
{
    if (m.isNoteOn())
    {
        captureArmAt = positionInChunk;
        const bool followMidi = keyMode->load() > 0.5f;
        // velocity scales the voice from this exact sample (before the effects, like a harder or softer hit)
        const float amount = velocityParam->load();
        voice.velocityGain = 1.0f - amount * (1.0f - m.getFloatVelocity());
        voice.noteOn (m.getNoteNumber(), (float) (followMidi ? m.getNoteNumber() : getKeyNote()));
    }
    else if (m.isNoteOff())
        voice.noteOff (m.getNoteNumber());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        voice.allNotesOff();
}

void CodexProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::AudioProcessLoadMeasurer::ScopedTimer loadTimer (loadMeasurer, buffer.getNumSamples());
    updateParameters();

    // GUI preview button always plays the selected key (uses an out-of-range note id so MIDI can't cut it)
    constexpr int previewId = 128;
    if (previewOn.exchange (false))  { voice.noteOn (previewId, (float) getKeyNote()); captureArmAt = 0; }
    if (previewOff.exchange (false)) voice.noteOff (previewId);

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    auto midiIt = midi.begin();
    std::array<float, 2> blockPeak { 0.0f, 0.0f };

    for (int start = 0; start < numSamples; start += chunkSize)
    {
        const int n = std::min (chunkSize, numSamples - start);
        float* l = left.data();
        float* r = right.data();
        float* m = mid.data();

        // Render the (mono) voice, splitting at MIDI events for sample-accurate triggering
        int pos = 0;
        while (pos < n)
        {
            int next = n;
            while (midiIt != midi.end())
            {
                const auto meta = *midiIt;
                const int t = meta.samplePosition - start;
                if (t <= pos) { handleMidi (meta.getMessage(), pos); ++midiIt; }
                else          { next = std::min (t, n); break; }
            }
            voice.subOut = sub.data() + pos;
            voice.render (l + pos, next - pos);
            pos = next;
        }
        std::copy (l, l + n, r);

        // Old-school chain: distort -> EQ -> distort again -> multiband saturation -> compressor -> EQ 2 -> space (stereo from here on)
        clip1.process (l, r, n);
        for (int i = 0; i < n; ++i) m[i] = 0.5f * (l[i] + r[i]);
        analyzerPre.push (m, n);           // spectrum before the EQ (for the EQ display)
        equalisers[0].process (l, r, n);
        clip2.process (l, r, n);
        saturator.process (l, r, n);
        compressor.process (l, r, n);      // multiband up / down compression (off by default)
        for (int i = 0; i < n; ++i) m[i] = 0.5f * (l[i] + r[i]);
        analyzerPre2.push (m, n);          // spectrum before EQ 2
        equalisers[1].process (l, r, n);   // EQ 2 (post), empty by default
        reverb.process (l, r, n);          // Space: stereo room reverb (off by default)

        // Clean sub layer, low-passed and delayed by the chain latency, added in mono
        {
            const int delay = juce::jlimit (0, (int) subDelay.size() - 1, chainLatency());
            const int len = (int) subDelay.size();
            for (int i = 0; i < n; ++i)
            {
                subDelay[(size_t) subDelayPos] = subLp2.lp (subLp1.lp (sub[(size_t) i]));
                const float s = subDelay[(size_t) ((subDelayPos - delay + len) % len)] * subGain.getNextValue();
                subDelayPos = (subDelayPos + 1) % len;
                l[i] += s;
                r[i] += s;
            }
        }

        for (int i = 0; i < n; ++i)
        {
            const float g = outGain.getNextValue();
            l[i] *= g;
            r[i] *= g;
        }
        clipper.process (l, r, n);         // optional output clipper (off by default)

        for (int i = 0; i < n; ++i)
        {
            m[i] = 0.5f * (l[i] + r[i]);
            blockPeak[0] = std::max (blockPeak[0], std::abs (l[i]));
            blockPeak[1] = std::max (blockPeak[1], std::abs (r[i]));
            if (i == captureArmAt) startScope();
            if (scopePos >= 0) feedScope (l[i], r[i]);
        }
        captureArmAt = -1;

        if (numChannels >= 2)
        {
            buffer.copyFrom (0, start, l, n);
            buffer.copyFrom (1, start, r, n);
            for (int ch = 2; ch < numChannels; ++ch)
                buffer.clear (ch, start, n);
        }
        else if (numChannels == 1)
            buffer.copyFrom (0, start, m, n);

        analyzer.push (m, n);
        tunerFeed.push (m, n);
        scopeFeedL.push (l, n);
        scopeFeedR.push (r, n);
    }

    for (int c = 0; c < 2; ++c)
        if (blockPeak[(size_t) c] > peaks[(size_t) c].load())
            peaks[(size_t) c].store (blockPeak[(size_t) c]);
}

void CodexProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void CodexProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            undoManager.clearUndoHistory(); // restoring a session is not an undoable edit
            presets.markClean();            // the restored sound is the reference for the "*" modified marker
        }
}

juce::AudioProcessorEditor* CodexProcessor::createEditor()
{
    return new CodexEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CodexProcessor();
}
