#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class TunePXOCobiAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TunePXOCobiAudioProcessorEditor(TunePXOCobiAudioProcessor&);
    ~TunePXOCobiAudioProcessorEditor() override = default;
    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    TunePXOCobiAudioProcessor& audioProcessor;
    juce::Label title, subtitle, engineLabel, modeLabel, keyLabel, scaleLabel, retuneLabel, humanizeLabel, mixLabel, pitchCaption, pitchValue, status;
    juce::ComboBox engineBox, modeBox, keyBox, scaleBox;
    juce::Slider retuneSlider, humanizeSlider, mixSlider;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<ComboAttachment> engineAttachment, modeAttachment, keyAttachment, scaleAttachment;
    std::unique_ptr<SliderAttachment> retuneAttachment, humanizeAttachment, mixAttachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TunePXOCobiAudioProcessorEditor)
};
