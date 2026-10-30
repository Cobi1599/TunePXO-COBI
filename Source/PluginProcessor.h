#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include "PsolaRealtimeAdapter.h"

class TunePXOCobiAudioProcessor final : public juce::AudioProcessor
{
public:
    TunePXOCobiAudioProcessor();
    ~TunePXOCobiAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TunePXO COBI"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.08; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;
    int getDetectedMidiNote() const noexcept { return detectedMidiNote.load(std::memory_order_relaxed); }
    float getDetectedFrequency() const noexcept { return detectedFrequency.load(std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void analysePitch() noexcept;
    float readDelay(int channel, float delaySamples) const noexcept;
    float processPitchShift(int channel, float input, float ratio) noexcept;
    int quantiseMidiNote(float midiNote, int key, int scale) const noexcept;

    static constexpr int pitchWindowSize = 512;
    PsolaRealtimeAdapter psola;
    juce::AudioBuffer<float> psolaScratch;
    static constexpr int dryRingSize = 16384;
    std::array<std::array<float, dryRingSize>, 2> psolaDryRing {};
    int dryWritePosition = 0;
    int psolaLatencySamples = 2502;
    int lastPsolaPeriod = 0;
    std::array<std::array<float, dryRingSize>, 2> legacyLatencyRing {};
    int legacyLatencyWritePosition = 0;
    static constexpr int delayBufferSize = 2048;
    std::array<float, pitchWindowSize> pitchBuffer {};
    int pitchWritePosition = 0;
    int samplesSinceAnalysis = 0;
    int audioSampleCounter = 0;
    int pitchSamplesWritten = 0;
    std::array<std::array<float, delayBufferSize>, 2> delayBuffers {};
    int delayWritePosition = 0;
    float shifterPhase = 0.0f;
    float smoothedMidiNote = -1.0f;
    float smoothedCorrectionSemitones = 0.0f;
    float smoothedPitchRatio = 1.0f;
    float heldTargetPitchRatio = 1.0f;
    int samplesSinceValidPitch = 0;
    double currentSampleRate = 44100.0;
    std::atomic<int> detectedMidiNote { -1 };
    std::atomic<float> detectedFrequency { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TunePXOCobiAudioProcessor)
};
