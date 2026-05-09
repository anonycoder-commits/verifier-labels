#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <matjson.hpp>
#include <matjson/std.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>

using namespace geode::prelude;
using namespace matjson;

static constexpr const char* CACHE_FILE = "verifier_cache.json";
static constexpr const char* CLASSIC_API = "https://api.aredl.net/v2/api/aredl/levels";
static constexpr const char* PLATFORMER_API = "https://api.aredl.net/v2/api/arepl/levels";
static constexpr long long CACHE_EXPIRY = 1800;

struct VerifierData {
    std::string verifier;
    std::string video;
    bool legacy = false;
    long long timestamp = 0;
};

template<>
struct matjson::Serialize<VerifierData> {
    static Result<VerifierData> from_json(Value const& v) {
        if (!v.isObject()) return Err("expected object");
        return Ok(VerifierData{
            v.contains("verifier") ? v["verifier"].asString().unwrapOr("") : "",
            v.contains("video") ? v["video"].asString().unwrapOr("") : "",
            v.contains("legacy") ? v["legacy"].asBool().unwrapOr(false) : false,
            v.contains("timestamp") ? static_cast<long long>(v["timestamp"].asInt().unwrapOr(0)) : 0
        });
    }
    static Value to_json(VerifierData const& d) {
        return makeObject({
            {"verifier", d.verifier},
            {"video", d.video},
            {"legacy", d.legacy},
            {"timestamp", d.timestamp}
        });
    }
};

static std::unordered_map<std::string, VerifierData> s_cache;

static void saveCache() {
    if (Mod::get()->getSettingValue<bool>("disable-cache")) return;
    auto obj = Value::object();
    for (auto const& [k, d] : s_cache) {
        obj.set(k, Serialize<VerifierData>::to_json(d));
    }
    auto path = Mod::get()->getSaveDir() / CACHE_FILE;
    if (auto res = file::writeString(path, obj.dump(NO_INDENTATION)); !res) {
        log::error("Failed to save cache: {}", res.unwrapErr());
    }
}

static void loadCache() {
    if (Mod::get()->getSettingValue<bool>("disable-cache")) return;
    auto path = Mod::get()->getSaveDir() / CACHE_FILE;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return;
    if (auto res = file::readJson(path)) {
        auto root = res.unwrap();
        if (!root.isObject()) return;
        for (auto const& [k, v] : root) {
            if (auto parsed = Serialize<VerifierData>::from_json(v)) {
                s_cache[k] = parsed.unwrap();
            }
        }
    }
}

static long long nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

$execute {
    loadCache();
}

class $modify(VerifierInfoLayer, LevelInfoLayer) {
    struct Fields {
        CCLabelBMFont* m_label = nullptr;
        CCMenuItemSpriteExtra* m_labelBtn = nullptr;
        CCMenuItemSpriteExtra* m_ytBtn = nullptr;
        async::TaskHolder<web::WebResponse> m_soloTask;
        async::TaskHolder<web::WebResponse> m_duoTask;
        std::string m_videoUrl;
        bool m_duo = false;
    };

    bool init(GJGameLevel* level, bool p1);

    void buildUI() {
        auto menu = CCMenu::create();
        menu->setID("verifier-menu"_spr);
        menu->setContentSize({0, 0});
        menu->ignoreAnchorPointForPosition(false);

        m_fields->m_label = CCLabelBMFont::create("", "goldFont.fnt");
        m_fields->m_label->setScale(0.4f);
        m_fields->m_label->setID("verifier-label"_spr);

        bool left = Mod::get()->getSettingValue<std::string>("label-alignment") == "Left";
        m_fields->m_label->setAnchorPoint(left ? ccp(0, 0.5f) : ccp(0.5f, 0.5f));

        m_fields->m_labelBtn = CCMenuItemSpriteExtra::create(
            m_fields->m_label, this, menu_selector(VerifierInfoLayer::onToggleMode)
        );
        m_fields->m_labelBtn->setID("verifier-label-btn"_spr);
        m_fields->m_labelBtn->setVisible(false);

        if (auto ytIcon = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png")) {
            ytIcon->setScale(0.32f);
            m_fields->m_ytBtn = CCMenuItemSpriteExtra::create(
                ytIcon, this, menu_selector(VerifierInfoLayer::onVideo)
            );
            if (m_fields->m_ytBtn) {
                m_fields->m_ytBtn->setID("verifier-yt-btn"_spr);
                m_fields->m_ytBtn->setVisible(false);
                menu->addChild(m_fields->m_ytBtn);
            }
        }

        menu->addChild(m_fields->m_labelBtn);
        this->addChild(menu);

        if (auto anchor = this->getChildByID("creator-info-menu")) {
            auto yOff = static_cast<float>(Mod::get()->getSettingValue<double>("y-offset"));
            menu->setPosition(anchor->getPosition() + ccp(0, yOff));
        }
    }

    void onToggleMode(CCObject* sender) {
        if (!m_level || !m_level->m_twoPlayerMode) return;
        m_fields->m_duo = !m_fields->m_duo;
        refreshLabel();

        if (auto node = typeinfo_cast<CCNode*>(sender)) {
            node->stopAllActions();
            node->runAction(CCSequence::create(
                CCScaleTo::create(0.05f, 1.1f),
                CCEaseBackOut::create(CCScaleTo::create(0.2f, 1.0f)),
                nullptr
            ));
        }
    }

    std::string levelKey() {
        return std::to_string(m_level->m_levelID) + (m_fields->m_duo ? "_2p" : "");
    }

    void refreshLabel() {
        if (!m_level) return;

        bool cacheDisabled = Mod::get()->getSettingValue<bool>("disable-cache");
        if (!cacheDisabled) {
            auto it = s_cache.find(levelKey());
            if (it != s_cache.end() && (nowSec() - it->second.timestamp) <= CACHE_EXPIRY) {
                applyData(it->second);
                return;
            }
        }

        m_fields->m_label->setString("Checking...");
        m_fields->m_labelBtn->setVisible(true);
        if (m_fields->m_ytBtn) m_fields->m_ytBtn->setVisible(false);
    }

    void applyData(VerifierData const& d) {
        auto hide = [&] {
            m_fields->m_labelBtn->setVisible(false);
            if (m_fields->m_ytBtn) m_fields->m_ytBtn->setVisible(false);
        };

        if (d.verifier.empty()) { hide(); return; }

        if (m_level && m_level->m_demonDifficulty == 5 && !d.legacy) { hide(); return; }

        m_fields->m_videoUrl = d.video;

        auto prefix = m_fields->m_duo
            ? "[2P] "
            : (m_level->m_twoPlayerMode ? "[Solo] Verified by: " : "Verified by: ");

        m_fields->m_label->setString((prefix + d.verifier).c_str());

        bool grayed = d.legacy && Mod::get()->getSettingValue<bool>("legacy-color");
        m_fields->m_label->setFntFile(grayed ? "bigFont.fnt" : "goldFont.fnt");

        float scale = grayed ? 0.35f : 0.4f;
        m_fields->m_label->setScale(scale);
        m_fields->m_label->limitLabelWidth(200.f, scale, 0.1f);

        m_fields->m_labelBtn->setContentSize(m_fields->m_label->getScaledContentSize());
        m_fields->m_label->setPosition(m_fields->m_labelBtn->getContentSize() / 2);
        m_fields->m_labelBtn->setVisible(true);
        m_fields->m_labelBtn->setEnabled(m_level && m_level->m_twoPlayerMode);

        if (m_fields->m_ytBtn) {
            bool showYt = !d.video.empty() && Mod::get()->getSettingValue<bool>("show-youtube");
            m_fields->m_ytBtn->setVisible(showYt);
            if (showYt) {
                m_fields->m_ytBtn->setPosition({
                    m_fields->m_label->getScaledContentSize().width / 2 + 8.f, 0
                });
            }
        }

        m_fields->m_labelBtn->updateSprite();
    }

    void onVideo(CCObject*) {
        if (!m_fields->m_videoUrl.empty()) {
            web::openLinkInBrowser(m_fields->m_videoUrl);
        }
    }

    void fetchLevel(std::string key, bool duo) {
        auto url = std::string(m_level->isPlatformer() ? PLATFORMER_API : CLASSIC_API) + "/" + key;
        auto& holder = duo ? m_fields->m_duoTask : m_fields->m_soloTask;

        holder.spawn(web::WebRequest().userAgent("Geode-AREDL-Mod/1.0.1").get(url), [this, key](web::WebResponse res) {
            if (!res.ok()) {
                log::debug("API request failed for {}: {}", key, res.code());
                s_cache[key] = {"", "", false, nowSec()};
                saveCache();
                if (key == levelKey()) refreshLabel();
                return;
            }

            auto parsed = res.json();
            if (!parsed.isOk()) {
                log::debug("Failed to parse JSON response for {}", key);
                s_cache[key] = {"", "", false, nowSec()};
                saveCache();
                if (key == levelKey()) refreshLabel();
                return;
            }
            auto root = parsed.unwrap();

            std::vector<std::string> names;
            std::string video;
            bool legacy = root.contains("legacy") && root["legacy"].asBool().unwrapOr(false);

            if (root.contains("verifications")) {
                auto arr = root["verifications"];
                if (arr.isArray()) {
                    for (auto const& v : arr) {
                        if (!v.isObject()) continue;
                        if (video.empty() && v.contains("video_url")) {
                            video = v["video_url"].asString().unwrapOr("");
                        }
                        if (v.contains("submitted_by")) {
                            auto sub = v["submitted_by"];
                            if (!sub.isObject()) continue;
                            std::string name = sub.contains("global_name")
                                ? sub["global_name"].asString().unwrapOr("") : "";
                            if (name.empty()) name = sub.contains("username")
                                ? sub["username"].asString().unwrapOr("") : "";
                            if (name.empty()) name = "Unknown";
                            if (std::ranges::find(names, name) == names.end()) {
                                names.push_back(name);
                            }
                        }
                    }
                }
            }

            std::string display;
            if (names.size() == 1) display = names[0];
            else if (names.size() >= 2) display = names[0] + " & " + names[1];

            s_cache[key] = {display, video, legacy, nowSec()};
            saveCache();
            if (key == levelKey()) refreshLabel();
        });
    }
};

bool VerifierInfoLayer::init(GJGameLevel* level, bool p1) {
    if (!LevelInfoLayer::init(level, p1)) return false;
    if (!Mod::get()->getSettingValue<bool>("show-label")) return true;
    if (!m_level) return true;

    buildUI();

    if (m_level->m_levelID > 0 && m_level->m_demonDifficulty >= 5) {
        refreshLabel();
        auto id = std::to_string(m_level->m_levelID);
        fetchLevel(id, false);
        if (m_level->m_twoPlayerMode) {
            fetchLevel(id + "_2p", true);
        }
    }

    return true;
}