#include "MainComponent.h"

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
            setResizeLimits(1100, 720, 4000, 3000);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    std::unique_ptr<MainWindow> window_;
};

START_JUCE_APPLICATION(AudioFinisherApplication)
