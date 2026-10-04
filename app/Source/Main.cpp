#include "MainComponent.h"
#include "Settings.h"

class AudioFinisherApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        if (window_)
            if (auto* mc = dynamic_cast<MainComponent*>(window_->getContentComponent()))
                mc->handleCommandLine(juce::StringArray::fromTokens(commandLine, true));
    }

    void initialise(const juce::String& commandLine) override
    {
        // Interface size on top of the OS display scaling (persisted). A
        // "--scale x" argument overrides it for this run (used for UI checks).
        double scale = settings::uiScale();
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        if (const int i = args.indexOf("--scale"); i >= 0 && i + 1 < args.size()) scale = args[i + 1].getDoubleValue();
        juce::Desktop::getInstance().setGlobalScaleFactor(float(juce::jlimit(0.5, 2.0, scale)));
        window_ = std::make_unique<MainWindow>(getApplicationName());
        // Files passed on the command line (or "Open with") are added to the session.
        if (auto* mc = dynamic_cast<MainComponent*>(window_->getContentComponent()))
            mc->handleCommandLine(juce::StringArray::fromTokens(commandLine, true));
    }
    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name, juce::Colour(0xff15171c), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(940, 600, 10000, 10000);
            // Fit inside the usable screen area at the current scaling
            // (e.g. 1920x1080 at 125 % = 1536x~830 logical pixels).
            settings::fitToScreen(*this, 1500, 940, true);
            setVisible(true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    std::unique_ptr<MainWindow> window_;
};

START_JUCE_APPLICATION(AudioFinisherApplication)
