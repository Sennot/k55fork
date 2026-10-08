#include <Geode/Geode.hpp>
#include <Geode/loader/ModEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr double DEFAULT_MAX_CPS = 16.6667;
constexpr double DEFAULT_HOLD_FRACTION = 0.5;
constexpr size_t MAX_LIST_ROWS = 6;

// Gate source ids: a key code for keys with their own cap, POINTER for
// mouse / touch input, DEFAULT for everything that uses the shared cap.
constexpr int SOURCE_DEFAULT = -1;
constexpr int SOURCE_POINTER = static_cast<int>(KEY_None);

struct PercentPoint {
    double percent;
    double cps;
};

struct KeyCap {
    int key;
    double cps;
};

struct Config {
    bool enabled = true;
    bool debounce = false;
    double baseCps = DEFAULT_MAX_CPS;
    double holdFraction = DEFAULT_HOLD_FRACTION;
    std::vector<PercentPoint> percentPoints;
    std::unordered_map<int, double> keyCaps;
};

std::vector<PercentPoint> loadPercentPoints() {
    std::vector<PercentPoint> points;
    auto json = Mod::get()->getSavedValue<matjson::Value>("percent-points", matjson::Value::array());
    if (!json.isArray()) {
        return points;
    }
    for (auto const& entry : json) {
        auto percent = entry["percent"].asDouble();
        auto cps = entry["cps"].asDouble();
        if (percent.isOk() && cps.isOk()) {
            points.push_back({percent.unwrap(), cps.unwrap()});
        }
    }
    return points;
}

void savePercentPoints(std::vector<PercentPoint> const& points) {
    auto json = matjson::Value::array();
    for (auto const& point : points) {
        json.push(matjson::makeObject({{"percent", point.percent}, {"cps", point.cps}}));
    }
    Mod::get()->setSavedValue("percent-points", json);
}

std::vector<KeyCap> loadKeyCaps() {
    std::vector<KeyCap> caps;
    auto json = Mod::get()->getSavedValue<matjson::Value>("key-caps", matjson::Value::array());
    if (!json.isArray()) {
        return caps;
    }
    for (auto const& entry : json) {
        auto key = entry["key"].asInt();
        auto cps = entry["cps"].asDouble();
        if (key.isOk() && cps.isOk()) {
            caps.push_back({static_cast<int>(key.unwrap()), cps.unwrap()});
        }
    }
    return caps;
}

void saveKeyCaps(std::vector<KeyCap> const& caps) {
    auto json = matjson::Value::array();
    for (auto const& cap : caps) {
        json.push(matjson::makeObject({{"key", cap.key}, {"cps", cap.cps}}));
    }
    Mod::get()->setSavedValue("key-caps", json);
}

Config loadConfig() {
    auto mod = Mod::get();
    Config c;
    c.enabled = mod->getSavedValue<bool>("enabled", true);
    c.debounce = mod->getSavedValue<bool>("debounce", false);
    c.baseCps = std::max(0.1, mod->getSavedValue<double>("max-cps", DEFAULT_MAX_CPS));
    c.holdFraction = std::clamp(
        mod->getSavedValue<double>("hold-fraction", DEFAULT_HOLD_FRACTION), 0.1, 0.9
    );

    c.percentPoints = loadPercentPoints();
    std::stable_sort(c.percentPoints.begin(), c.percentPoints.end(), [](auto const& a, auto const& b) {
        return a.percent < b.percent;
    });
    for (auto const& cap : loadKeyCaps()) {
        c.keyCaps[cap.key] = cap.cps;
    }
    return c;
}

struct Locks {
    double press = 0.0;
    double release = 0.0;
};

struct Gate {
    bool held = false;
    bool queued = false;
    bool player1 = true;
    int pendingButton = static_cast<int>(PlayerButton::Jump);
    double unlockFrame = 0.0;
    Locks locks;
};

Config config;
bool needsReset = false;
std::optional<enumKeyCodes> activeKey;

std::unordered_map<int64_t, Gate> gates;
uint64_t frameCount = 0;
double medianFrameTime = 1.0 / 240.0;
std::array<float, 9> frameTimes{};
size_t frameTimeIndex = 0;
bool replaying = false;

void applyConfigChange() {
    config = loadConfig();
    needsReset = true;
}

void setEnabled(bool enabled) {
    Mod::get()->setSavedValue<bool>("enabled", enabled);
    applyConfigChange();
}

void trackFrameTime(float dt) {
    frameTimes[frameTimeIndex % frameTimes.size()] = dt;
    frameTimeIndex++;

    auto count = std::min(frameTimeIndex, frameTimes.size());
    auto sorted = frameTimes;
    std::sort(sorted.begin(), sorted.begin() + count);
    medianFrameTime = sorted[count / 2];
}

double toFrames(double seconds) {
    return seconds / medianFrameTime;
}

bool unlocked(double unlockFrame) {
    return static_cast<double>(frameCount) >= unlockFrame - 0.1;
}

double carryUnlock(double unlockFrame) {
    return static_cast<double>(frameCount) <= unlockFrame + 1.0
        ? unlockFrame
        : static_cast<double>(frameCount);
}

int64_t gateKey(int source, bool player1) {
    return (static_cast<int64_t>(source) << 1) | (player1 ? 1 : 0);
}

double sharedCps() {
    auto cps = config.baseCps;
    if (config.percentPoints.empty()) {
        return cps;
    }
    if (auto playLayer = PlayLayer::get()) {
        auto percent = static_cast<double>(playLayer->getCurrentPercent());
        for (auto const& point : config.percentPoints) {
            if (percent < point.percent) {
                break;
            }
            cps = point.cps;
        }
    }
    return cps;
}

// Picks the gate and cap for the input currently being handled. 0 CPS means uncapped.
std::pair<int, double> resolveSource() {
    int key = activeKey ? static_cast<int>(*activeKey) : SOURCE_POINTER;
    if (auto it = config.keyCaps.find(key); it != config.keyCaps.end()) {
        return {key, it->second};
    }
    return {SOURCE_DEFAULT, sharedCps()};
}

Locks locksFor(double cps) {
    auto period = 1.0 / cps;
    return {period * config.holdFraction, period * (1.0 - config.holdFraction)};
}

bool filterEdge(bool press, int button, bool player1) {
    if (replaying || !config.enabled) {
        return true;
    }

    auto [source, cps] = resolveSource();
    if (cps <= 0.0) {
        return true;
    }

    auto& gate = gates[gateKey(source, player1)];
    gate.player1 = player1;
    gate.locks = locksFor(cps);

    if (config.debounce) {
        if (!press) {
            return true;
        }
        if (unlocked(gate.unlockFrame)) {
            gate.unlockFrame = carryUnlock(gate.unlockFrame)
                + toFrames(gate.locks.press + gate.locks.release);
            return true;
        }
        return false;
    }

    if (!gate.queued && unlocked(gate.unlockFrame)) {
        gate.held = press;
        gate.pendingButton = button;
        gate.unlockFrame = carryUnlock(gate.unlockFrame)
            + toFrames(press ? gate.locks.press : gate.locks.release);
        return true;
    }

    bool effectiveHeld = gate.queued ? !gate.held : gate.held;
    if (press == effectiveHeld) {
        return false;
    }

    if (!gate.queued) {
        gate.queued = true;
        gate.pendingButton = button;
    } else {
        gate.queued = false;
    }
    return false;
}

void replayEdge(GJBaseGameLayer* layer, Gate& gate) {
    gate.held = !gate.held;
    gate.queued = false;

    replaying = true;
    layer->handleButton(gate.held, gate.pendingButton, gate.player1);
    replaying = false;
}

void flushGate(GJBaseGameLayer* layer, Gate& gate) {
    while (gate.queued && unlocked(gate.unlockFrame)) {
        replayEdge(layer, gate);
        gate.unlockFrame += toFrames(gate.held ? gate.locks.press : gate.locks.release);
    }
}

// Lets every queued edge through right away so no button gets stuck, then forgets all gates.
void releaseAll(GJBaseGameLayer* layer) {
    for (auto& [key, gate] : gates) {
        if (gate.queued) {
            replayEdge(layer, gate);
        }
    }
    gates.clear();
    needsReset = false;
}

std::string keyName(int key) {
    if (key == SOURCE_POINTER) {
        return "Mouse / Touch";
    }
    return Keybind(static_cast<enumKeyCodes>(key), KeyboardModifier::None).toString();
}

CCLabelBMFont* addLabel(CCNode* parent, char const* text, CCPoint pos, float scale, char const* font = "bigFont.fnt") {
    auto label = CCLabelBMFont::create(text, font);
    label->setScale(scale);
    label->setPosition(pos);
    parent->addChild(label);
    return label;
}

CCMenuItemSpriteExtra* addTextButton(CCMenu* menu, char const* text, CCPoint pos, CCObject* target, SEL_MenuHandler callback) {
    auto sprite = ButtonSprite::create(text, "bigFont.fnt", "GJ_button_04.png", 0.6f);
    sprite->setScale(0.7f);
    auto button = CCMenuItemSpriteExtra::create(sprite, target, callback);
    button->setPosition(pos);
    menu->addChild(button);
    return button;
}

TextInput* addNumberInput(CCNode* parent, float width, CCPoint pos, bool isFloat, size_t maxChars) {
    auto input = TextInput::create(width, "0");
    input->setCommonFilter(isFloat ? CommonFilter::Float : CommonFilter::Uint);
    input->setMaxCharCount(maxChars);
    input->setScale(0.7f);
    input->setPosition(pos);
    parent->addChild(input);
    return input;
}

}

$on_mod(DataLoaded) {
    config = loadConfig();
}

$on_mod(Loaded) {
    config = loadConfig();

    listenForKeybindSettingPresses("toggle-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            setEnabled(!config.enabled);
            Notification::create(
                config.enabled ? "CPS Cap: ON" : "CPS Cap: OFF", NotificationIcon::Info, 0.5f
            )->show();
        }
        return true;
    });
}

class $modify(CappedBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!filterEdge(down, button, isPlayer1)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void update(float dt) {
        if (dt > 0.f) {
            frameCount++;
            trackFrameTime(dt);
        }

        if (needsReset || (!config.enabled && !gates.empty())) {
            releaseAll(this);
        } else {
            for (auto& [key, gate] : gates) {
                flushGate(this, gate);
            }
        }

        GJBaseGameLayer::update(dt);
    }
};

class $modify(CappedUILayer, UILayer) {
    void handleKeypress(enumKeyCodes key, bool down, double timestamp) {
        auto previous = activeKey;
        activeKey = key;
        UILayer::handleKeypress(key, down, timestamp);
        activeKey = previous;
    }
};

class $modify(CappedPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        gates.clear();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        gates.clear();
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        gates.clear();
        PlayLayer::resetLevelFromStart();
    }

    void onQuit() {
        gates.clear();
        PlayLayer::onQuit();
    }
};

class KeyCapturePopup : public geode::Popup {
protected:
    std::function<void(int)> m_callback;

    bool init(std::function<void(int)> callback) {
        if (!Popup::init(240.f, 140.f)) {
            return false;
        }
        m_callback = std::move(callback);
        this->setTitle("Press a key");

        addLabel(m_mainLayer, "Esc to cancel", {120.f, 82.f}, 0.5f, "goldFont.fnt");
        addTextButton(
            m_buttonMenu, "Mouse / Touch", {120.f, 38.f}, this, menu_selector(KeyCapturePopup::onPointer)
        );
        return true;
    }

    void finish(int key) {
        auto callback = m_callback;
        this->onClose(nullptr);
        if (callback) {
            callback(key);
        }
    }

    void keyDown(enumKeyCodes key, double timestamp) override {
        // Escape goes through keyBackClicked, which closes the popup
        if (key == KEY_Escape || key == KEY_None || key == KEY_Unknown) {
            return;
        }
        this->finish(static_cast<int>(key));
    }

    void onPointer(CCObject*) {
        this->finish(SOURCE_POINTER);
    }

public:
    static KeyCapturePopup* create(std::function<void(int)> callback) {
        auto ret = new KeyCapturePopup();
        if (ret->init(std::move(callback))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class PercentCapsPopup : public geode::Popup {
protected:
    std::vector<PercentPoint> m_points;
    std::vector<CCNode*> m_rowNodes;
    CCMenuItemSpriteExtra* m_addBtn = nullptr;

    bool init() {
        if (!Popup::init(300.f, 260.f)) {
            return false;
        }
        this->setTitle("Percent Caps");

        addLabel(m_mainLayer, "Max CPS is used until the first point. 0 CPS = no cap", {150.f, 216.f}, 0.3f, "goldFont.fnt");
        addLabel(m_mainLayer, "From %", {95.f, 196.f}, 0.4f);
        addLabel(m_mainLayer, "CPS", {185.f, 196.f}, 0.4f);

        m_addBtn = addTextButton(m_buttonMenu, "Add", {150.f, 24.f}, this, menu_selector(PercentCapsPopup::onAdd));

        m_points = loadPercentPoints();
        this->buildRows();
        return true;
    }

    void save() {
        savePercentPoints(m_points);
        applyConfigChange();
    }

    void buildRows() {
        for (auto node : m_rowNodes) {
            node->removeFromParent();
        }
        m_rowNodes.clear();

        for (size_t i = 0; i < m_points.size(); i++) {
            auto y = 172.f - static_cast<float>(i) * 25.f;

            auto percentInput = addNumberInput(m_mainLayer, 60.f, {95.f, y}, false, 3);
            percentInput->setString(fmt::format("{:.0f}", m_points[i].percent));
            percentInput->setCallback([this, i](std::string const& str) {
                if (auto num = numFromString<double>(str)) {
                    m_points[i].percent = std::clamp(*num, 0.0, 100.0);
                    this->save();
                }
            });
            m_rowNodes.push_back(percentInput);

            auto cpsInput = addNumberInput(m_mainLayer, 80.f, {185.f, y}, true, 7);
            cpsInput->setString(fmt::format("{:.2f}", m_points[i].cps));
            cpsInput->setCallback([this, i](std::string const& str) {
                if (auto num = numFromString<double>(str)) {
                    m_points[i].cps = std::clamp(*num, 0.0, 240.0);
                    this->save();
                }
            });
            m_rowNodes.push_back(cpsInput);

            auto deleteSpr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png");
            deleteSpr->setScale(0.6f);
            auto deleteBtn = CCMenuItemSpriteExtra::create(
                deleteSpr, this, menu_selector(PercentCapsPopup::onDelete)
            );
            deleteBtn->setTag(static_cast<int>(i));
            deleteBtn->setPosition({255.f, y});
            m_buttonMenu->addChild(deleteBtn);
            m_rowNodes.push_back(deleteBtn);
        }

        m_addBtn->setVisible(m_points.size() < MAX_LIST_ROWS);
    }

    void onAdd(CCObject*) {
        if (m_points.size() >= MAX_LIST_ROWS) {
            return;
        }
        auto percent = m_points.empty() ? 50.0 : std::min(100.0, m_points.back().percent + 10.0);
        m_points.push_back({percent, config.baseCps});
        this->save();
        this->buildRows();
    }

    void onDelete(CCObject* sender) {
        auto index = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (index < m_points.size()) {
            m_points.erase(m_points.begin() + index);
            this->save();
            this->buildRows();
        }
    }

public:
    static PercentCapsPopup* create() {
        auto ret = new PercentCapsPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class KeyCapsPopup : public geode::Popup {
protected:
    std::vector<KeyCap> m_caps;
    std::vector<CCNode*> m_rowNodes;
    CCMenuItemSpriteExtra* m_addBtn = nullptr;

    bool init() {
        if (!Popup::init(300.f, 260.f)) {
            return false;
        }
        this->setTitle("Key Caps");

        addLabel(m_mainLayer, "Other inputs use Max CPS. 0 CPS = no cap", {150.f, 216.f}, 0.3f, "goldFont.fnt");
        addLabel(m_mainLayer, "Key", {95.f, 196.f}, 0.4f);
        addLabel(m_mainLayer, "CPS", {195.f, 196.f}, 0.4f);

        m_addBtn = addTextButton(m_buttonMenu, "Add Key", {150.f, 24.f}, this, menu_selector(KeyCapsPopup::onAdd));

        m_caps = loadKeyCaps();
        this->buildRows();
        return true;
    }

    void save() {
        saveKeyCaps(m_caps);
        applyConfigChange();
    }

    void buildRows() {
        for (auto node : m_rowNodes) {
            node->removeFromParent();
        }
        m_rowNodes.clear();

        for (size_t i = 0; i < m_caps.size(); i++) {
            auto y = 172.f - static_cast<float>(i) * 25.f;

            auto name = addLabel(m_mainLayer, keyName(m_caps[i].key).c_str(), {95.f, y}, 0.4f);
            name->limitLabelWidth(110.f, 0.4f, 0.1f);
            m_rowNodes.push_back(name);

            auto cpsInput = addNumberInput(m_mainLayer, 80.f, {195.f, y}, true, 7);
            cpsInput->setString(fmt::format("{:.2f}", m_caps[i].cps));
            cpsInput->setCallback([this, i](std::string const& str) {
                if (auto num = numFromString<double>(str)) {
                    m_caps[i].cps = std::clamp(*num, 0.0, 240.0);
                    this->save();
                }
            });
            m_rowNodes.push_back(cpsInput);

            auto deleteSpr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png");
            deleteSpr->setScale(0.6f);
            auto deleteBtn = CCMenuItemSpriteExtra::create(
                deleteSpr, this, menu_selector(KeyCapsPopup::onDelete)
            );
            deleteBtn->setTag(static_cast<int>(i));
            deleteBtn->setPosition({262.f, y});
            m_buttonMenu->addChild(deleteBtn);
            m_rowNodes.push_back(deleteBtn);
        }

        m_addBtn->setVisible(m_caps.size() < MAX_LIST_ROWS);
    }

    void onAdd(CCObject*) {
        if (m_caps.size() >= MAX_LIST_ROWS) {
            return;
        }
        Ref<KeyCapsPopup> self = this;
        KeyCapturePopup::create([self](int key) {
            auto exists = std::any_of(self->m_caps.begin(), self->m_caps.end(), [key](auto const& cap) {
                return cap.key == key;
            });
            if (exists || self->m_caps.size() >= MAX_LIST_ROWS) {
                return;
            }
            self->m_caps.push_back({key, config.baseCps});
            self->save();
            self->buildRows();
        })->show();
    }

    void onDelete(CCObject* sender) {
        auto index = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (index < m_caps.size()) {
            m_caps.erase(m_caps.begin() + index);
            this->save();
            this->buildRows();
        }
    }

public:
    static KeyCapsPopup* create() {
        auto ret = new KeyCapsPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class CPSCapPopup : public geode::Popup {
protected:
    TextInput* m_cpsInput = nullptr;
    TextInput* m_fracInput = nullptr;
    Slider* m_cpsSlider = nullptr;
    Slider* m_fracSlider = nullptr;

    static double cpsFromSlider(float v) {
        return 1.0 + static_cast<double>(v) * 239.0;
    }
    static float sliderFromCps(double cps) {
        return static_cast<float>(std::clamp((cps - 1.0) / 239.0, 0.0, 1.0));
    }
    static double fracFromSlider(float v) {
        return 0.1 + static_cast<double>(v) * 0.8;
    }
    static float sliderFromFrac(double frac) {
        return static_cast<float>(std::clamp((frac - 0.1) / 0.8, 0.0, 1.0));
    }

    void refreshCps(bool updateSlider) {
        auto cps = Mod::get()->getSavedValue<double>("max-cps", DEFAULT_MAX_CPS);
        m_cpsInput->setString(fmt::format("{:.2f}", cps));
        if (updateSlider) {
            m_cpsSlider->setValue(sliderFromCps(cps));
        }
    }

    void refreshFrac(bool updateSlider) {
        auto frac = Mod::get()->getSavedValue<double>("hold-fraction", DEFAULT_HOLD_FRACTION);
        m_fracInput->setString(fmt::format("{:.0f}", frac * 100.0));
        if (updateSlider) {
            m_fracSlider->setValue(sliderFromFrac(frac));
        }
    }

    CCMenuItemToggler* addToggle(
        CCMenu* menu, char const* label, char const* key, bool def, float y
    ) {
        auto toggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(CPSCapPopup::onToggle), 0.7f
        );
        toggle->setUserObject(CCString::create(key));
        toggle->toggle(Mod::get()->getSavedValue<bool>(key, def));
        toggle->setPosition({45.f, y});
        menu->addChild(toggle);

        auto text = CCLabelBMFont::create(label, "bigFont.fnt");
        text->setScale(0.45f);
        text->setAnchorPoint({0.f, 0.5f});
        text->setPosition({65.f, y});
        m_mainLayer->addChild(text);
        return toggle;
    }

    bool init() {
        if (!Popup::init(320.f, 260.f)) {
            return false;
        }
        this->setTitle("CPS Cap");
        auto menu = m_buttonMenu;

        auto cpsLabel = CCLabelBMFont::create("Max CPS", "bigFont.fnt");
        cpsLabel->setScale(0.45f);
        cpsLabel->setAnchorPoint({0.f, 0.5f});
        cpsLabel->setPosition({65.f, 216.f});
        m_mainLayer->addChild(cpsLabel);

        m_cpsInput = TextInput::create(70.f, "16.67");
        m_cpsInput->setCommonFilter(CommonFilter::Float);
        m_cpsInput->setMaxCharCount(7);
        m_cpsInput->setScale(0.8f);
        m_cpsInput->setPosition({250.f, 216.f});
        m_cpsInput->setCallback([this](std::string const& str) {
            this->onCpsInput(str);
        });
        m_mainLayer->addChild(m_cpsInput);

        m_cpsSlider = Slider::create(this, menu_selector(CPSCapPopup::onCpsSlider), 0.8f);
        m_cpsSlider->setPosition({150.f, 188.f});
        m_mainLayer->addChild(m_cpsSlider);

        auto k55Spr = CCSprite::createWithSpriteFrameName("geode.loader/reset-gold.png");
        k55Spr->setScale(0.7f);
        auto k55Btn = CCMenuItemSpriteExtra::create(
            k55Spr, this, menu_selector(CPSCapPopup::onK55Reset)
        );
        k55Btn->setPosition({258.f, 188.f});
        menu->addChild(k55Btn);

        auto fracLabel = CCLabelBMFont::create("Press/Release Split", "bigFont.fnt");
        fracLabel->setScale(0.45f);
        fracLabel->setAnchorPoint({0.f, 0.5f});
        fracLabel->setPosition({65.f, 154.f});
        m_mainLayer->addChild(fracLabel);

        m_fracInput = TextInput::create(70.f, "50");
        m_fracInput->setCommonFilter(CommonFilter::Uint);
        m_fracInput->setMaxCharCount(2);
        m_fracInput->setScale(0.8f);
        m_fracInput->setPosition({250.f, 154.f});
        m_fracInput->setCallback([this](std::string const& str) {
            this->onFracInput(str);
        });
        m_mainLayer->addChild(m_fracInput);

        m_fracSlider = Slider::create(this, menu_selector(CPSCapPopup::onFracSlider), 0.8f);
        m_fracSlider->setPosition({150.f, 126.f});
        m_mainLayer->addChild(m_fracSlider);

        addToggle(menu, "Enable CPS Cap", "enabled", true, 94.f);
        addToggle(menu, "Debounce", "debounce", false, 66.f);

        addTextButton(menu, "Percent", {70.f, 28.f}, this, menu_selector(CPSCapPopup::onPercentCaps));
        addTextButton(menu, "Keys", {160.f, 28.f}, this, menu_selector(CPSCapPopup::onKeyCaps));
        addTextButton(menu, "Keybind", {250.f, 28.f}, this, menu_selector(CPSCapPopup::onKeybind));

        refreshCps(true);
        refreshFrac(true);
        return true;
    }

    void onToggle(CCObject* sender) {
        auto toggle = static_cast<CCMenuItemToggler*>(sender);
        auto key = static_cast<CCString*>(toggle->getUserObject())->getCString();
        Mod::get()->setSavedValue<bool>(key, !toggle->isToggled());
        applyConfigChange();
    }

    void onCpsSlider(CCObject* sender) {
        auto value = static_cast<SliderThumb*>(sender)->getValue();
        Mod::get()->setSavedValue<double>("max-cps", cpsFromSlider(value));
        applyConfigChange();
        refreshCps(false);
    }

    void onFracSlider(CCObject* sender) {
        auto value = static_cast<SliderThumb*>(sender)->getValue();
        Mod::get()->setSavedValue<double>("hold-fraction", fracFromSlider(value));
        applyConfigChange();
        refreshFrac(false);
    }

    void onCpsInput(std::string const& str) {
        if (auto num = numFromString<double>(str)) {
            auto cps = std::clamp(*num, 1.0, 240.0);
            Mod::get()->setSavedValue<double>("max-cps", cps);
            applyConfigChange();
            m_cpsSlider->setValue(sliderFromCps(cps));
        }
    }

    void onFracInput(std::string const& str) {
        if (auto num = numFromString<double>(str)) {
            auto frac = std::clamp(*num / 100.0, 0.1, 0.9);
            Mod::get()->setSavedValue<double>("hold-fraction", frac);
            applyConfigChange();
            m_fracSlider->setValue(sliderFromFrac(frac));
        }
    }

    void onK55Reset(CCObject*) {
        Mod::get()->setSavedValue<double>("max-cps", DEFAULT_MAX_CPS);
        applyConfigChange();
        refreshCps(true);
    }

    void onPercentCaps(CCObject*) {
        PercentCapsPopup::create()->show();
    }

    void onKeyCaps(CCObject*) {
        KeyCapsPopup::create()->show();
    }

    void onKeybind(CCObject*) {
        openKeybindsPopup(KeybindCategory::Gameplay, Mod::get());
    }

public:
    static CPSCapPopup* create() {
        auto ret = new CPSCapPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(CappedPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto sprite = CCSprite::create("MenuCap.png"_spr);
        if (!sprite) return;
        sprite->setScale(30.f / sprite->getContentSize().height);
        auto button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(CappedPauseLayer::onCPSCap)
        );
        button->setID("cps-cap-button"_spr);

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(button);
            menu->updateLayout();
        }
    }

    void onCPSCap(CCObject*) {
        CPSCapPopup::create()->show();
    }
};
