#include "PluginProcessor.h"
#include "PluginEditor.h"

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
    layout.add(std::make_unique<Float>("MIX", "Correction",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    return layout;
}

void TunePXOCobiAudioProcessor::prepareToPlay(double, int) {}
void TunePXOCobiAudioProcessor::releaseResources() {}

void TunePXOCobiAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // v0.1 is a clean pass-through scaffold. Pitch correction DSP is the next milestone.
    juce::ignoreUnused(buffer);
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
