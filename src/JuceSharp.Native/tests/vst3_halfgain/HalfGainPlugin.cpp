// Test-only VST3 effect: output = input x 0.5. No editor, no parameters.
//
// Built by the native test configuration only (tests/CMakeLists.txt,
// dc_test_halfgain) into the build folder's test-plugins/, never shipped or
// installed. JuceSharp's VstSandboxSeatEndToEndTests loads it through the real
// juce_vst_host.exe to prove a sandboxed slot processes its own seat's audio
// and no other (DotCharter #646).

#include <juce_audio_processors/juce_audio_processors.h>

namespace {

class HalfGainProcessor final : public juce::AudioProcessor
{
public:
    HalfGainProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "DcTestHalfGain"; }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannelSet();
        return ! in.isDisabled() && in == layouts.getMainOutputChannelSet();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain(0.5f); }
    using AudioProcessor::processBlock;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HalfGainProcessor(); }
