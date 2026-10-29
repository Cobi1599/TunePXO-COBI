#include "PluginEditor.h"

namespace {
const juce::Colour background(0xff101018), panel(0xff1b1b29), purple(0xff9b5cff), text(0xfff4efff), muted(0xffaaa3b8);
void styleLabel(juce::Label& label, const juce::String& value, int size, juce::Colour colour)
{
    label.setText(value, juce::dontSendNotification);
    label.setFont(juce::Font((float) size, juce::Font::bold));
    label.setColour(juce::Label::textColourId, colour);
}
}

TunePXOCobiAudioProcessorEditor::TunePXOCobiAudioProcessorEditor(TunePXOCobiAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(620, 420);
    styleLabel(title, "TunePXO COBI", 28, text);
    styleLabel(subtitle, "VOCAL TUNING  /  VST3", 11, purple);
    styleLabel(keyLabel, "KEY", 12, muted);
    styleLabel(scaleLabel, "SCALE", 12, muted);
    styleLabel(retuneLabel, "RETUNE SPEED", 12, muted);
    styleLabel(humanizeLabel, "HUMANIZE", 12, muted);
    styleLabel(mixLabel, "CORRECTION", 12, muted);
    styleLabel(status, "ENGINE STATUS: UI / AUDIO PASS-THROUGH BUILD", 11, muted);
    for (auto* c : { &title, &subtitle, &keyLabel, &scaleLabel, &retuneLabel, &humanizeLabel, &mixLabel, &status })
        addAndMakeVisible(*c);

    keyBox.addItemList(juce::StringArray{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
    scaleBox.addItemList(juce::StringArray{ "Major", "Minor", "Chromatic" }, 1);
    for (auto* c : { &keyBox, &scaleBox }) {
        c->setColour(juce::ComboBox::backgroundColourId, panel);
        c->setColour(juce::ComboBox::textColourId, text);
        c->setColour(juce::ComboBox::outlineColourId, purple);
        addAndMakeVisible(*c);
    }
    auto setupSlider = [this](juce::Slider& s) {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 22);
        s.setColour(juce::Slider::rotarySliderFillColourId, purple);
        s.setColour(juce::Slider::thumbColourId, text);
        s.setColour(juce::Slider::textBoxTextColourId, text);
        s.setColour(juce::Slider::textBoxBackgroundColourId, panel);
        addAndMakeVisible(s);
    };
    setupSlider(retuneSlider); setupSlider(humanizeSlider); setupSlider(mixSlider);

    auto& state = audioProcessor.parameters;
    keyAttachment = std::make_unique<ComboAttachment>(state, "KEY", keyBox);
    scaleAttachment = std::make_unique<ComboAttachment>(state, "SCALE", scaleBox);
    retuneAttachment = std::make_unique<SliderAttachment>(state, "RETUNE", retuneSlider);
    humanizeAttachment = std::make_unique<SliderAttachment>(state, "HUMANIZE", humanizeSlider);
    mixAttachment = std::make_unique<SliderAttachment>(state, "MIX", mixSlider);
}

void TunePXOCobiAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(background);
    g.setColour(panel); g.fillRoundedRectangle(20.0f, 86.0f, 580.0f, 310.0f, 18.0f);
    g.setColour(purple); g.fillRoundedRectangle(20.0f, 86.0f, 580.0f, 4.0f, 2.0f);
    g.setColour(juce::Colour(0xff342747)); g.drawRoundedRectangle(20.0f, 86.0f, 580.0f, 310.0f, 18.0f, 1.0f);
}

void TunePXOCobiAudioProcessorEditor::resized()
{
    title.setBounds(24, 16, 340, 38);
    subtitle.setBounds(27, 52, 260, 20);
    keyLabel.setBounds(48, 112, 100, 20); keyBox.setBounds(48, 136, 130, 34);
    scaleLabel.setBounds(202, 112, 100, 20); scaleBox.setBounds(202, 136, 150, 34);
    retuneLabel.setBounds(55, 205, 130, 20); retuneSlider.setBounds(48, 225, 140, 145);
    humanizeLabel.setBounds(240, 205, 100, 20); humanizeSlider.setBounds(218, 225, 145, 145);
    mixLabel.setBounds(430, 205, 110, 20); mixSlider.setBounds(410, 225, 145, 145);
    status.setBounds(26, 390, 560, 20);
}
