#include "PluginEditor.h"

namespace {
const juce::Colour bgTop(0xff090910), bgBottom(0xff171326), card(0xff171624), card2(0xff201b31);
const juce::Colour purple(0xffa45bff), violet(0xff713cff), pink(0xffe45cff);
const juce::Colour white(0xfff8f4ff), muted(0xffaaa2bd), line(0xff39304c);

void styleLabel(juce::Label& label, const juce::String& value, float size, juce::Colour colour, bool bold = true)
{
    label.setText(value, juce::dontSendNotification);
    label.setFont(juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)));
    label.setColour(juce::Label::textColourId, colour);
}
}

TunePXOCobiAudioProcessorEditor::TunePXOCobiAudioProcessorEditor(TunePXOCobiAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(820, 570);
    setResizable(true, true);
    setResizeLimits(700, 500, 1100, 760);

    styleLabel(title, "TunePXO", 31.0f, white);
    styleLabel(subtitle, "COBI  /  VOCAL TUNING SYSTEM", 11.0f, purple);
    styleLabel(engineLabel, "PITCH ENGINE", 10.5f, muted);
    styleLabel(modeLabel, "TUNING STYLE", 10.5f, muted);
    styleLabel(keyLabel, "KEY", 10.5f, muted);
    styleLabel(scaleLabel, "SCALE", 10.5f, muted);
    styleLabel(retuneLabel, "RETUNE SPEED", 10.5f, muted);
    styleLabel(humanizeLabel, "HUMANIZE", 10.5f, muted);
    styleLabel(mixLabel, "CORRECTION MIX", 10.5f, muted);
    styleLabel(pitchCaption, "LIVE PITCH MONITOR", 10.0f, muted);
    styleLabel(pitchValue, "--", 34.0f, white);
    styleLabel(status, "REAL-TIME PITCH DETECTION + CORRECTION ENGINE  /  BETA", 9.5f, muted, false);

    for (auto* c : { &title, &subtitle, &engineLabel, &modeLabel, &keyLabel, &scaleLabel, &retuneLabel,
                     &humanizeLabel, &mixLabel, &pitchCaption, &pitchValue, &status })
        addAndMakeVisible(*c);

    engineBox.addItem("LEGACY", 1);
    engineBox.addItem("PSOLA", 2);
    modeBox.addItem("NATURAL", 1);
    modeBox.addItem("HARD TUNE", 2);
    keyBox.addItemList(juce::StringArray{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
    scaleBox.addItemList(juce::StringArray{ "Major", "Minor", "Chromatic" }, 1);

    for (auto* c : { &engineBox, &modeBox, &keyBox, &scaleBox }) {
        c->setColour(juce::ComboBox::backgroundColourId, card2);
        c->setColour(juce::ComboBox::textColourId, white);
        c->setColour(juce::ComboBox::outlineColourId, purple);
        c->setColour(juce::ComboBox::arrowColourId, purple);
        c->setColour(juce::ComboBox::focusedOutlineColourId, pink);
        addAndMakeVisible(*c);
    }

    auto setupSlider = [this](juce::Slider& s) {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 82, 24);
        s.setColour(juce::Slider::rotarySliderFillColourId, purple);
        s.setColour(juce::Slider::rotarySliderOutlineColourId, line);
        s.setColour(juce::Slider::thumbColourId, white);
        s.setColour(juce::Slider::textBoxTextColourId, white);
        s.setColour(juce::Slider::textBoxBackgroundColourId, card2);
        s.setColour(juce::Slider::textBoxOutlineColourId, line);
        addAndMakeVisible(s);
    };
    setupSlider(retuneSlider);
    setupSlider(humanizeSlider);
    setupSlider(mixSlider);

    auto& state = audioProcessor.parameters;
    engineAttachment = std::make_unique<ComboAttachment>(state, "ENGINE", engineBox);
    modeAttachment = std::make_unique<ComboAttachment>(state, "MODE", modeBox);
    keyAttachment = std::make_unique<ComboAttachment>(state, "KEY", keyBox);
    scaleAttachment = std::make_unique<ComboAttachment>(state, "SCALE", scaleBox);
    retuneAttachment = std::make_unique<SliderAttachment>(state, "RETUNE", retuneSlider);
    humanizeAttachment = std::make_unique<SliderAttachment>(state, "HUMANIZE", humanizeSlider);
    mixAttachment = std::make_unique<SliderAttachment>(state, "MIX", mixSlider);
    startTimerHz(15);
}

void TunePXOCobiAudioProcessorEditor::timerCallback()
{
    static const juce::StringArray noteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int midi = audioProcessor.getDetectedMidiNote();
    const float hz = audioProcessor.getDetectedFrequency();
    if (midi < 0 || hz <= 0.0f) {
        pitchValue.setText("--", juce::dontSendNotification);
        return;
    }
    const int octave = midi / 12 - 1;
    pitchValue.setText(noteNames[midi % 12] + juce::String(octave) + "  " + juce::String(hz, 1) + " Hz", juce::dontSendNotification);
}

void TunePXOCobiAudioProcessorEditor::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient bg(bgTop, 0.0f, 0.0f, bgBottom, 0.0f, bounds.getHeight(), false);
    g.setGradientFill(bg);
    g.fillAll();

    // Soft neon ambience behind the main panel.
    g.setColour(purple.withAlpha(0.10f));
    g.fillEllipse(bounds.getWidth() - 250.0f, -90.0f, 330.0f, 260.0f);
    g.setColour(pink.withAlpha(0.06f));
    g.fillEllipse(-110.0f, bounds.getHeight() - 220.0f, 300.0f, 270.0f);

    auto panel = juce::Rectangle<float>(24.0f, 105.0f, bounds.getWidth() - 48.0f, bounds.getHeight() - 142.0f);
    g.setColour(card.withAlpha(0.98f));
    g.fillRoundedRectangle(panel, 22.0f);
    g.setColour(line);
    g.drawRoundedRectangle(panel, 22.0f, 1.0f);

    juce::ColourGradient neon(purple, panel.getX(), panel.getY(), pink, panel.getRight(), panel.getY(), false);
    g.setGradientFill(neon);
    g.fillRoundedRectangle(panel.getX() + 1.0f, panel.getY(), panel.getWidth() - 2.0f, 4.0f, 2.0f);

    auto monitor = juce::Rectangle<float>(panel.getX() + 22.0f, panel.getY() + 150.0f, panel.getWidth() - 44.0f, 94.0f);
    g.setColour(card2);
    g.fillRoundedRectangle(monitor, 14.0f);
    g.setColour(line);
    g.drawRoundedRectangle(monitor, 14.0f, 1.0f);

    // Decorative pitch-grid preview; live pitch tracking will be connected in the DSP milestone.
    g.setColour(purple.withAlpha(0.16f));
    for (int i = 1; i < 8; ++i) {
        const float y = monitor.getY() + (monitor.getHeight() * (float)i / 8.0f);
        g.drawHorizontalLine((int)y, monitor.getX() + 12.0f, monitor.getRight() - 12.0f);
    }
    juce::Path trace;
    const float mid = monitor.getCentreY();
    trace.startNewSubPath(monitor.getX() + 18.0f, mid + 12.0f);
    trace.cubicTo(monitor.getX() + 100.0f, mid - 26.0f, monitor.getX() + 145.0f, mid + 30.0f,
                  monitor.getX() + 220.0f, mid - 8.0f);
    trace.cubicTo(monitor.getX() + 300.0f, mid - 42.0f, monitor.getX() + 360.0f, mid + 25.0f,
                  monitor.getX() + 440.0f, mid - 14.0f);
    g.setColour(purple.withAlpha(0.85f));
    g.strokePath(trace, juce::PathStrokeType(2.2f));
    g.setColour(pink);
    g.fillEllipse(monitor.getX() + 216.0f, mid - 12.0f, 8.0f, 8.0f);
}

void TunePXOCobiAudioProcessorEditor::resized()
{
    auto w = getWidth();
    auto panelX = 48;
    title.setBounds(panelX, 18, 300, 42);
    subtitle.setBounds(panelX + 3, 58, 320, 20);

    engineLabel.setBounds(panelX + 2, 124, 130, 18);
    engineBox.setBounds(panelX, 146, 145, 36);
    modeLabel.setBounds(panelX + 165, 124, 130, 18);
    modeBox.setBounds(panelX + 163, 146, 175, 36);
    keyLabel.setBounds(panelX + 355, 124, 80, 18);
    keyBox.setBounds(panelX + 353, 146, 125, 36);
    scaleLabel.setBounds(panelX + 495, 124, 90, 18);
    scaleBox.setBounds(panelX + 493, 146, 155, 36);

    auto panelWidth = w - 96;
    auto third = panelWidth / 3;
    const int yLabel = 367;
    retuneLabel.setBounds(panelX + 8, yLabel, third - 16, 18);
    humanizeLabel.setBounds(panelX + third, yLabel, third - 16, 18);
    mixLabel.setBounds(panelX + third * 2, yLabel, third - 16, 18);
    retuneSlider.setBounds(panelX, yLabel + 18, third, 142);
    humanizeSlider.setBounds(panelX + third, yLabel + 18, third, 142);
    mixSlider.setBounds(panelX + third * 2, yLabel + 18, third, 142);

    pitchCaption.setBounds(w - 220, 28, 170, 18);
    pitchValue.setBounds(w - 220, 47, 170, 42);
    status.setBounds(panelX, getHeight() - 27, w - 96, 16);
}
