#include "PluginProcessor.h"
#include "GUI/PluginEditor.h"

HardKickProcessor::HardKickProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "HardKickState", Params::createLayout())
{
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

    for (int s = 0; s < 2; ++s)
        clipParams[s] = { p (ID::clip (s + 1, "On")), p (ID::clip (s + 1, "Mode")), p (ID::clip (s + 1, "Drive")),
                          p (ID::clip (s + 1, "Bias")), p (ID::clip (s + 1, "Mix")), p (ID::clip (s + 1, "Out")) };

    for (int b = 0; b < Params::numBands; ++b)
        bandParams[b] = { p (ID::band (b, "On")), p (ID::band (b, "Type")), p (ID::band (b, "Freq")),
                          p (ID::band (b, "Gain")), p (ID::band (b, "Q")), p (ID::band (b, "Slope")) };
}

bool HardKickProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void HardKickProcessor::prepareToPlay (double sampleRate, int)
{
    mono.assign ((size_t) chunkSize, 0.0f);

    voice.prepare (sampleRate);
    updateParameters();
    clip1.prepare (sampleRate, chunkSize);
    clip2.prepare (sampleRate, chunkSize);
    equaliser.prepare (sampleRate);
    updateParameters();

    outGain.reset (sampleRate, 0.02);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outGainParam->load()));

    setLatencySamples (juce::roundToInt (clip1.getLatencyInSamples() + clip2.getLatencyInSamples()));
}

int HardKickProcessor::getKeyNote() const
{
    return Params::keyBaseNote + (int) key->load() + 12 * ((int) octave->load() - 1);
}

void HardKickProcessor::updateParameters()
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
    s.pitchAmountSemis = pAmount->load();
    s.clickLevel       = clickLevel->load();
    s.clickDecayMs     = clickDecay->load();
    s.clickToneHz      = clickTone->load();
    voice.setSettings (s);

    voice.pitchEnv.setParameters (pAttack->load(), pDecay->load(), pSustain->load(), pRelease->load(), pCurve->load());
    if (s.engine == KickVoice::Engine909) // amp ADSR is a gate: attack + release only
        voice.ampEnv.setParameters (aAttack->load(), 1.0f, 1.0f, aRelease->load(), aCurve->load());
    else
        voice.ampEnv.setParameters (aAttack->load(), aDecay->load(), aSustain->load(), aRelease->load(), aCurve->load());

    ClipDistortion* clips[] = { &clip1, &clip2 };
    for (int i = 0; i < 2; ++i)
    {
        const auto& c = clipParams[i];
        clips[i]->setParameters (c.on->load() > 0.5f, (int) c.mode->load(), c.drive->load(),
                                 c.bias->load(), c.mix->load(), c.out->load());
    }

    for (int b = 0; b < Params::numBands; ++b)
    {
        const auto& bp = bandParams[b];
        eq::BandSettings bs;
        bs.on     = bp.on->load() > 0.5f;
        bs.type   = (eq::BandType) (int) bp.type->load();
        bs.freq   = bp.freq->load();
        bs.gainDb = bp.gain->load();
        bs.q      = bp.q->load();
        bs.slope  = (int) bp.slope->load();
        equaliser.setBand (b, bs);
    }

    outGain.setTargetValue (juce::Decibels::decibelsToGain (outGainParam->load()));
}

void HardKickProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        const bool followMidi = keyMode->load() > 0.5f;
        voice.noteOn (m.getNoteNumber(), (float) (followMidi ? m.getNoteNumber() : getKeyNote()));
    }
    else if (m.isNoteOff())
        voice.noteOff (m.getNoteNumber());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        voice.allNotesOff();
}

void HardKickProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    updateParameters();

    // GUI "KICK" button always plays the selected key (uses an out-of-range note id so MIDI can't cut it)
    constexpr int previewId = 128;
    if (previewOn.exchange (false))  voice.noteOn (previewId, (float) getKeyNote());
    if (previewOff.exchange (false)) voice.noteOff (previewId);

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    auto midiIt = midi.begin();
    float blockPeak = 0.0f;

    for (int start = 0; start < numSamples; start += chunkSize)
    {
        const int n = std::min (chunkSize, numSamples - start);
        float* x = mono.data();

        // Render the voice, splitting at MIDI events for sample-accurate triggering
        int pos = 0;
        while (pos < n)
        {
            int next = n;
            while (midiIt != midi.end())
            {
                const auto meta = *midiIt;
                const int t = meta.samplePosition - start;
                if (t <= pos) { handleMidi (meta.getMessage()); ++midiIt; }
                else          { next = std::min (t, n); break; }
            }
            voice.render (x + pos, next - pos);
            pos = next;
        }

        // Old-school chain: distort -> EQ -> distort again
        clip1.process (x, n);
        equaliser.process (x, n);
        clip2.process (x, n);

        for (int i = 0; i < n; ++i)
        {
            x[i] *= outGain.getNextValue();
            blockPeak = std::max (blockPeak, std::abs (x[i]));
        }

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.copyFrom (ch, start, x, n);

        analyzer.push (x, n);
        tunerFeed.push (x, n);
    }

    if (blockPeak > peak.load())
        peak.store (blockPeak);
}

void HardKickProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void HardKickProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* HardKickProcessor::createEditor()
{
    return new HardKickEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HardKickProcessor();
}
