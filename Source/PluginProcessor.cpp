#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace {
constexpr float twoPi = juce::MathConstants<float>::twoPi;
constexpr int analysisDecimation = 2;
constexpr int minPitchLag = 12;
constexpr int maxPitchLag = 300;
constexpr float minimumCorrelation = 0.62f;
}

TunePXOCobiAudioProcessor::TunePXOCobiAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout TunePXOCobiAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<Choice>("MODE", "Tuning Style", juce::StringArray{ "Natural", "Hard Tune" }, 0));
    layout.add(std::make_unique<Choice>("KEY", "Key",
        juce::StringArray{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    layout.add(std::make_unique<Choice>("SCALE", "Scale",
        juce::StringArray{ "Major", "Minor", "Chromatic" }, 0));
    layout.add(std::make_unique<Float>("RETUNE", "Retune Speed",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 35.0f));
    layout.add(std::make_unique<Float>("HUMANIZE", "Humanize",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 20.0f));
    layout.add(std::make_unique<Float>("MIX", "Correction Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    return layout;
}

void TunePXOCobiAudioProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    for (auto& channel : delayBuffers)
        channel.fill(0.0f);
    pitchBuffer.fill(0.0f);
    pitchWritePosition = samplesSinceAnalysis = delayWritePosition = 0;
    shifterPhase = 0.0f;
    smoothedMidiNote = -1.0f;
    detectedMidiNote.store(-1, std::memory_order_relaxed);
    detectedFrequency.store(0.0f, std::memory_order_relaxed);
}

void TunePXOCobiAudioProcessor::releaseResources() {}

int TunePXOCobiAudioProcessor::quantiseMidiNote(float midiNote, int key, int scale) const noexcept
{
    static constexpr bool major[12] = { true, false, true, false, true, true, false, true, false, true, false, true };
    static constexpr bool minor[12] = { true, false, true, true, false, true, false, true, true, false, true, false };
    const int nearest = (int)std::lround(midiNote);
    if (scale == 2) return nearest;
    const bool* allowed = scale == 1 ? minor : major;
    int best = nearest;
    float bestDistance = 1000.0f;
    for (int offset = -6; offset <= 6; ++offset) {
        const int candidate = nearest + offset;
        int pitchClass = (candidate - key) % 12;
        if (pitchClass < 0) pitchClass += 12;
        if (allowed[pitchClass] && std::abs(candidate - midiNote) < (float)bestDistance) {
            best = candidate;
            bestDistance = std::abs((float)candidate - midiNote);
        }
    }
    return best;
}

void TunePXOCobiAudioProcessor::analysePitch() noexcept
{
    // The detector runs at half the audio sample rate to keep CPU use modest.
    float bestCorrelation = 0.0f;
    int bestLag = 0;
    float mean = 0.0f;
    for (float sample : pitchBuffer) mean += sample;
    mean /= (float)pitchWindowSize;

    float energy = 0.0f;
    for (float sample : pitchBuffer) {
        const float centred = sample - mean;
        energy += centred * centred;
    }
    if (energy < 0.00002f) {
        detectedMidiNote.store(-1, std::memory_order_relaxed);
        detectedFrequency.store(0.0f, std::memory_order_relaxed);
        return;
    }

    for (int lag = minPitchLag; lag <= maxPitchLag; ++lag) {
        float correlation = 0.0f;
        float leftEnergy = 0.0f, rightEnergy = 0.0f;
        for (int i = 0; i < pitchWindowSize - lag; ++i) {
            const float a = pitchBuffer[(pitchWritePosition + i) % pitchWindowSize] - mean;
            const float b = pitchBuffer[(pitchWritePosition + i + lag) % pitchWindowSize] - mean;
            correlation += a * b;
            leftEnergy += a * a;
            rightEnergy += b * b;
        }
        const float normaliser = std::sqrt(leftEnergy * rightEnergy) + 1.0e-9f;
        correlation /= normaliser;
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }

    if (bestLag == 0 || bestCorrelation < minimumCorrelation) {
        detectedMidiNote.store(-1, std::memory_order_relaxed);
        detectedFrequency.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const float frequency = (float)(currentSampleRate / (double)(analysisDecimation * bestLag));
    if (frequency < 65.0f || frequency > 1000.0f) {
        detectedMidiNote.store(-1, std::memory_order_relaxed);
        detectedFrequency.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const float midi = 69.0f + 12.0f * std::log2(frequency / 440.0f);
    detectedFrequency.store(frequency, std::memory_order_relaxed);
    detectedMidiNote.store((int)std::lround(midi), std::memory_order_relaxed);
}

float TunePXOCobiAudioProcessor::readDelay(int channel, float delaySamples) const noexcept
{
    delaySamples = juce::jlimit(2.0f, (float)delayBufferSize - 2.0f, delaySamples);
    const float readPosition = (float)delayWritePosition - delaySamples;
    const float wrapped = readPosition < 0.0f ? readPosition + (float)delayBufferSize : readPosition;
    const int indexA = ((int)wrapped) % delayBufferSize;
    const int indexB = (indexA + 1) % delayBufferSize;
    const float fraction = wrapped - std::floor(wrapped);
    const float a = delayBuffers[(size_t)channel][(size_t)indexA];
    const float b = delayBuffers[(size_t)channel][(size_t)indexB];
    return a + (b - a) * fraction;
}

float TunePXOCobiAudioProcessor::processPitchShift(int channel, float input, float ratio) noexcept
{
    auto& line = delayBuffers[(size_t)channel];
    line[(size_t)delayWritePosition] = input;

    constexpr float minDelay = 48.0f;
    constexpr float delayRange = 256.0f;
    ratio = juce::jlimit(0.5f, 2.0f, ratio);
    const float phaseStep = (1.0f - ratio) / delayRange;

    float phaseA = shifterPhase;
    float phaseB = phaseA + 0.5f;
    if (phaseB >= 1.0f) phaseB -= 1.0f;

    const float delayA = minDelay + phaseA * delayRange;
    const float delayB = minDelay + phaseB * delayRange;
    const float weightA = 0.5f - 0.5f * std::cos(twoPi * phaseA);
    const float weightB = 1.0f - weightA;
    const float shifted = readDelay(channel, delayA) * weightA + readDelay(channel, delayB) * weightB;

    juce::ignoreUnused(phaseStep);
    return std::abs(ratio - 1.0f) < 0.0005f ? input : shifted;
}

void TunePXOCobiAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin(buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();
    const int mode = (int)parameters.getRawParameterValue("MODE")->load();
    const int key = (int)parameters.getRawParameterValue("KEY")->load();
    const int scale = (int)parameters.getRawParameterValue("SCALE")->load();
    const float retune = parameters.getRawParameterValue("RETUNE")->load();
    const float humanize = parameters.getRawParameterValue("HUMANIZE")->load();
    const float mix = parameters.getRawParameterValue("MIX")->load() * 0.01f;

    for (int i = 0; i < numSamples; ++i) {
        float mono = 0.0f;
        for (int ch = 0; ch < channels; ++ch) mono += buffer.getReadPointer(ch)[i];
        mono /= (float)juce::jmax(1, channels);

        if ((samplesSinceAnalysis++ % analysisDecimation) == 0) {
            pitchBuffer[(size_t)pitchWritePosition] = mono;
            pitchWritePosition = (pitchWritePosition + 1) % pitchWindowSize;
        }
        if (samplesSinceAnalysis >= 512) {
            samplesSinceAnalysis = 0;
            analysePitch();
        }
    }

    const int detected = detectedMidiNote.load(std::memory_order_relaxed);
    const float frequency = detectedFrequency.load(std::memory_order_relaxed);
    if (detected >= 0 && frequency > 0.0f) {
        const float originalMidi = 69.0f + 12.0f * std::log2(frequency / 440.0f);
        const float targetMidi = (float)quantiseMidiNote(originalMidi, key, scale);
        const float speed = juce::jlimit(0.0f, 1.0f, retune * 0.01f);
        const float naturalFactor = mode == 0 ? 1.0f : 2.5f;
        const float humanFactor = 1.0f - 0.82f * (humanize * 0.01f);
        const float smoothing = juce::jlimit(0.001f, 0.95f,
            (0.004f + speed * speed * 0.72f) * naturalFactor * humanFactor);
        if (smoothedMidiNote < 0.0f) smoothedMidiNote = originalMidi;
        smoothedMidiNote += (targetMidi - smoothedMidiNote) * smoothing;
        const float correctionSemitones = (smoothedMidiNote - originalMidi) * mix;
        const float ratio = std::pow(2.0f, correctionSemitones / 12.0f);

        for (int i = 0; i < numSamples; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                const float dry = buffer.getReadPointer(ch)[i];
                const float wet = processPitchShift(ch, dry, ratio);
                buffer.getWritePointer(ch)[i] = dry + (wet - dry) * mix;
            }
            shifterPhase += (1.0f - ratio) / 256.0f;
            if (shifterPhase >= 1.0f) shifterPhase -= 1.0f;
            if (shifterPhase < 0.0f) shifterPhase += 1.0f;
            delayWritePosition = (delayWritePosition + 1) % delayBufferSize;
        }
    } else {
        smoothedMidiNote = -1.0f;
        for (int i = 0; i < numSamples; ++i) {
            for (int ch = 0; ch < channels; ++ch)
                processPitchShift(ch, buffer.getReadPointer(ch)[i], 1.0f);
            delayWritePosition = (delayWritePosition + 1) % delayBufferSize;
        }
    }
}

juce::AudioProcessorEditor* TunePXOCobiAudioProcessor::createEditor()
{
    return new TunePXOCobiAudioProcessorEditor(*this);
}

void TunePXOCobiAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void TunePXOCobiAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TunePXOCobiAudioProcessor();
}
