#include "UserPresets.h"

namespace userpresets {

juce::File folder()
{
    auto f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("AudioFinisher").getChildFile("Presets");
    f.createDirectory();
    return f;
}

juce::var controlsToVar(const ac::UserControls& c)
{
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> sec;
    for (bool b : c.sectionOn) sec.add(b);
    o->setProperty("sections", sec);
    o->setProperty("cleanup", c.cleanup);
    o->setProperty("tone", c.tone);
    o->setProperty("dynamics", c.dynamics);
    o->setProperty("tilt", c.tiltDb);
    if (c.loudnessMode) o->setProperty("loudnessMode", int(*c.loudnessMode));
    if (c.targetLufs) o->setProperty("targetLufs", *c.targetLufs);
    if (c.ceilingDbTP) o->setProperty("ceiling", *c.ceilingDbTP);
    o->setProperty("outputSampleRate", c.outputSampleRate);
    auto* ov = new juce::DynamicObject();
    for (auto& [id, v] : c.overrides)
        if (v != ac::Override::Auto) ov->setProperty(ac::stageKey(id), v == ac::Override::On ? "on" : "off");
    o->setProperty("overrides", juce::var(ov));
    return juce::var(o);
}

ac::UserControls controlsFromVar(const juce::var& v)
{
    ac::UserControls c;
    if (auto* sec = v["sections"].getArray())
        for (int i = 0; i < 4 && i < sec->size(); ++i) c.sectionOn[i] = bool((*sec)[i]);
    c.cleanup = double(v.getProperty("cleanup", 1.0));
    c.tone = double(v.getProperty("tone", 1.0));
    c.dynamics = double(v.getProperty("dynamics", 1.0));
    c.tiltDb = double(v.getProperty("tilt", 0.0));
    if (v.hasProperty("loudnessMode")) c.loudnessMode = ac::LoudnessMode(int(v["loudnessMode"]));
    if (v.hasProperty("targetLufs")) c.targetLufs = double(v["targetLufs"]);
    if (v.hasProperty("ceiling")) c.ceilingDbTP = double(v["ceiling"]);
    c.outputSampleRate = double(v.getProperty("outputSampleRate", 0.0));
    if (auto* ov = v["overrides"].getDynamicObject())
        for (auto& p : ov->getProperties())
            if (auto id = ac::stageFromKey(p.name.toString().toStdString()))
                c.overrides[*id] = p.value.toString() == "on" ? ac::Override::On : ac::Override::Off;
    return c;
}

std::vector<UserPreset> loadAll()
{
    std::vector<UserPreset> out;
    for (auto& f : folder().findChildFiles(juce::File::findFiles, false, "*.json"))
    {
        const auto v = juce::JSON::parse(f);
        if (!v.isObject()) continue;
        UserPreset up;
        up.file = f;
        const auto defJson = juce::JSON::toString(v["preset"], false).toStdString();
        std::string err;
        if (!ac::presetFromJson(defJson, up.def, &err)) continue;
        up.controls = controlsFromVar(v["controls"]);
        out.push_back(std::move(up));
    }
    std::sort(out.begin(), out.end(), [](const UserPreset& a, const UserPreset& b) { return a.def.name < b.def.name; });
    return out;
}

bool save(const juce::String& name, const ac::PresetDef& base, const ac::UserControls& controls, juce::String& error)
{
    ac::PresetDef def = base;
    def.name = name.toStdString();
    def.id = "user." + juce::File::createLegalFileName(name).replaceCharacter(' ', '_').toLowerCase().toStdString();
    const auto presetVar = juce::JSON::parse(juce::String(ac::presetToJson(def)));
    auto* root = new juce::DynamicObject();
    root->setProperty("format", "AudioFinisher user preset v1");
    root->setProperty("basePreset", juce::String(base.id));
    root->setProperty("preset", presetVar);
    root->setProperty("controls", controlsToVar(controls));
    const auto f = folder().getChildFile(juce::File::createLegalFileName(name) + ".json");
    if (!f.replaceWithText(juce::JSON::toString(juce::var(root))))
    {
        error = "Could not write " + f.getFullPathName();
        return false;
    }
    return true;
}

bool remove(const UserPreset& p) { return p.file.deleteFile(); }

} // namespace userpresets
