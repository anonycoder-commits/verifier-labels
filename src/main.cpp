#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <matjson.hpp>
#include <matjson/std.hpp>

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <chrono>
#include <string>
#include <vector>
#include <filesystem>

using namespace geode::prelude;

namespace {
    constexpr auto CACHE_FILENAME = "verifier_cache.json";
    constexpr auto API_URL_CLASSIC = "https://api.aredl.net/v2/api/aredl/levels";
    constexpr auto API_URL_PLATFORMER = "https://api.aredl.net/v2/api/arepl/levels";
    constexpr auto USER_AGENT = "Geode-AREDL-Mod/3.0";

    constexpr auto LABEL_TEXT_SCALE = 0.4f;
    constexpr auto LEGACY_TEXT_SCALE = 0.35f;
    constexpr auto YOUTUBE_ICON_SCALE = 0.32f;
    constexpr auto LABEL_MAX_WIDTH = 200.f;
    constexpr auto YOUTUBE_ICON_X_OFFSET = 8.0f;
}

struct CacheEntry {
    std::string verifierText;
    std::string videoUrl;
    bool isLegacy;
    long long timestamp;
};

template<>
struct matjson::Serialize<CacheEntry> {
    static geode::Result<CacheEntry> from_json(matjson::Value const& value) {
        if (!value.isObject()) {
            return geode::Err("CacheEntry must be an object");
        }

        CacheEntry entry;
        entry.verifierText = "";
        entry.videoUrl = "";
        entry.isLegacy = false;
        entry.timestamp = 0;

        if (value.contains("verifier")) {
            auto verifier = value["verifier"];
            if (verifier.isString()) {
                entry.verifierText = verifier.asString().unwrapOr("");
            }
        }

        if (value.contains("video")) {
            auto video = value["video"];
            if (video.isString()) {
                entry.videoUrl = video.asString().unwrapOr("");
            }
        }

        if (value.contains("legacy")) {
            auto legacy = value["legacy"];
            if (legacy.isBool()) {
                entry.isLegacy = legacy.asBool().unwrapOr(false);
            }
        }

        if (value.contains("timestamp")) {
            auto timestamp = value["timestamp"];
            if (timestamp.isNumber()) {
                entry.timestamp = static_cast<long long>(timestamp.asInt().unwrapOr(0));
            }
        }

        return geode::Ok(entry);
    }

    static matjson::Value to_json(CacheEntry const& value) {
        matjson::Value obj = matjson::makeObject({
            {"verifier", value.verifierText},
            {"video", value.videoUrl},
            {"legacy", value.isLegacy},
            {"timestamp", value.timestamp}
        });
        return obj;
    }
};

class VerifierCache {
    std::unordered_map<std::string, CacheEntry> m_cache;
    mutable std::shared_mutex m_mutex;

public:
    static VerifierCache& get() {
        static VerifierCache instance;
        return instance;
    }

    std::optional<CacheEntry> fetch(const std::string& key) const {
        std::shared_lock lock(m_mutex);
        if (auto it = m_cache.find(key); it != m_cache.end()) return it->second;
        return std::nullopt;
    }

    void insert(const std::string& key, CacheEntry entry) {
        std::unique_lock lock(m_mutex);
        m_cache[key] = std::move(entry);
    }

    void clear() {
        std::unique_lock lock(m_mutex);
        m_cache.clear();
    }

    std::unordered_map<std::string, CacheEntry> dump() const {
        std::shared_lock lock(m_mutex);
        return m_cache;
    }
};

static std::filesystem::path getCachePath() {
    return Mod::get()->getSaveDir() / CACHE_FILENAME;
}

static void saveCache() {
    if (Mod::get()->getSettingValue<bool>("disable-cache")) return;

    auto path = getCachePath();
    auto data = VerifierCache::get().dump();

    matjson::Value jsonObj = matjson::Value::object();
    for (auto const& [key, entry] : data) {
        jsonObj.set(key, matjson::Serialize<CacheEntry>::to_json(entry));
    }

    auto result = file::writeString(path, jsonObj.dump(matjson::NO_INDENTATION));
    if (!result) {
        log::error("Failed to save cache: {}", result.unwrapErr());
    }
}

static void loadCache() {
    if (Mod::get()->getSettingValue<bool>("disable-cache")) return;

    auto path = getCachePath();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return;

    auto res = file::readJson(path);
    if (!res) return;

    auto json = res.unwrap();

    if (!json.isObject()) return;

    for (auto const& [key, value] : json) {
        auto entryRes = matjson::Serialize<CacheEntry>::from_json(value);
        if (entryRes) {
            VerifierCache::get().insert(key, entryRes.unwrap());
        }
    }
}

static void clearCache() {
    VerifierCache::get().clear();
    auto path = getCachePath();
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        std::filesystem::remove(path, ec);
    }
}

class $modify(VerifierInfoLayer, LevelInfoLayer) {
    struct Fields {
        CCLabelBMFont* m_label = nullptr;
        CCMenuItemSpriteExtra* m_labelBtn = nullptr;
        CCMenuItemSpriteExtra* m_youtubeBtn = nullptr;
        async::TaskHolder<web::WebResponse> m_soloTask;
        async::TaskHolder<web::WebResponse> m_duoTask;
        std::string m_currentVideo;
        bool m_is2PMode = false;
    };

    bool init(GJGameLevel* level, bool p1) {
        if (!LevelInfoLayer::init(level, p1)) return false;

        static bool loaded = (loadCache(), true);

        if (!Mod::get()->getSettingValue<bool>("show-label")) return true;

        buildUI();
        if (level->m_levelID > 0 && level->m_demonDifficulty >= 5) {
            refreshVerifierInfo();
            fetchData(std::to_string(level->m_levelID), false);
            if (level->m_twoPlayerMode) {
                fetchData(std::to_string(level->m_levelID) + "_2p", true);
            }
        }
        return true;
    }

    void buildUI() {
        auto menu = CCMenu::create();
        menu->setID("verifier-menu"_spr);

        m_fields->m_label = CCLabelBMFont::create("", "goldFont.fnt");
        m_fields->m_label->setScale(LABEL_TEXT_SCALE);

        auto alignment = Mod::get()->getSettingValue<std::string>("label-alignment");
        if (alignment == "Left") {
            m_fields->m_label->setAnchorPoint({0.0f, 0.5f});
        } else {
            m_fields->m_label->setAnchorPoint({0.5f, 0.5f});
        }

        m_fields->m_labelBtn = CCMenuItemSpriteExtra::create(m_fields->m_label, this, menu_selector(VerifierInfoLayer::onToggleMode));
        m_fields->m_labelBtn->setVisible(false);

        auto ytSprite = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png");
        ytSprite->setScale(YOUTUBE_ICON_SCALE);
        m_fields->m_youtubeBtn = CCMenuItemSpriteExtra::create(ytSprite, this, menu_selector(VerifierInfoLayer::onVideo));
        m_fields->m_youtubeBtn->setVisible(false);

        menu->addChild(m_fields->m_labelBtn);
        menu->addChild(m_fields->m_youtubeBtn);
        this->addChild(menu);

        if (auto creatorMenu = this->getChildByID("creator-info-menu")) {
            menu->setPosition(creatorMenu->getPosition() + ccp(0, (float)Mod::get()->getSettingValue<double>("y-offset")));
        }
    }

    void onToggleMode(CCObject* sender) {
        if (!m_level->m_twoPlayerMode) return;
        m_fields->m_is2PMode = !m_fields->m_is2PMode;
        refreshVerifierInfo();

        auto btn = static_cast<CCNode*>(sender);
        btn->stopAllActions();
        btn->runAction(CCSequence::create(
            CCScaleTo::create(0.05f, 1.1f),
            CCEaseBackOut::create(CCScaleTo::create(0.2f, 1.0f)),
            nullptr
        ));
    }

    void refreshVerifierInfo() {
        std::string key = std::to_string(m_level->m_levelID) + (m_fields->m_is2PMode ? "_2p" : "");

        if (!Mod::get()->getSettingValue<bool>("disable-cache")) {
            if (auto entry = VerifierCache::get().fetch(key)) {
                updateUI(entry.value(), m_fields->m_is2PMode);
                return;
            }
        }

        m_fields->m_label->setString("Checking...");
        m_fields->m_labelBtn->setVisible(true);
        m_fields->m_youtubeBtn->setVisible(false);
    }

    void updateUI(const CacheEntry& entry, bool isDuo) {
        if (entry.verifierText.empty()) {
            if (isDuo == m_fields->m_is2PMode) {
                m_fields->m_labelBtn->setVisible(false);
                m_fields->m_youtubeBtn->setVisible(false);
            }
            return;
        }

        m_fields->m_currentVideo = entry.videoUrl;
        std::string prefix = isDuo ? "[2P] " : (m_level->m_twoPlayerMode ? "[Solo] Verified by: " : "Verified by: ");

        m_fields->m_label->setString((prefix + entry.verifierText).c_str());

        bool useLegacyColor = entry.isLegacy && Mod::get()->getSettingValue<bool>("legacy-color");
        m_fields->m_label->setFntFile(useLegacyColor ? "bigFont.fnt" : "goldFont.fnt");

        float scale = useLegacyColor ? LEGACY_TEXT_SCALE : LABEL_TEXT_SCALE;
        m_fields->m_label->setScale(scale);
        m_fields->m_label->limitLabelWidth(LABEL_MAX_WIDTH, scale, 0.1f);

        m_fields->m_labelBtn->setContentSize(m_fields->m_label->getScaledContentSize());
        m_fields->m_label->setPosition(m_fields->m_labelBtn->getContentSize() / 2);
        m_fields->m_labelBtn->setVisible(true);

        m_fields->m_labelBtn->setEnabled(m_level->m_twoPlayerMode);

        bool showYT = !entry.videoUrl.empty() && Mod::get()->getSettingValue<bool>("show-youtube");
        m_fields->m_youtubeBtn->setVisible(showYT);
        if (showYT) {
            float x = (m_fields->m_label->getScaledContentSize().width / 2) + YOUTUBE_ICON_X_OFFSET;
            m_fields->m_youtubeBtn->setPosition({x, 0});
        }
        m_fields->m_labelBtn->updateSprite();
    }

    void onVideo(CCObject*) {
        if (!m_fields->m_currentVideo.empty()) web::openLinkInBrowser(m_fields->m_currentVideo);
    }

    void fetchData(std::string key, bool isDuoRequest) {
        std::string url = fmt::format("{}/{}", m_level->isPlatformer() ? API_URL_PLATFORMER : API_URL_CLASSIC, key);
        auto& task = isDuoRequest ? m_fields->m_duoTask : m_fields->m_soloTask;

        task.spawn(web::WebRequest().userAgent(USER_AGENT).get(url), [this, key](web::WebResponse const& res) {
            if (!res.ok()) {
                long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                VerifierCache::get().insert(key, { "", "", false, now });
                saveCache();
                return;
            }

            auto json = res.json();
            if (!json.isOk()) return;
            auto root = json.unwrap();

            std::vector<std::string> names;
            std::string video = "";
            bool legacy = false;

            if (root.contains("legacy")) {
                auto legacyVal = root["legacy"];
                if (legacyVal.isBool()) {
                    legacy = legacyVal.asBool().unwrapOr(false);
                }
            }

            if (root.contains("verifications")) {
                auto verificationsVal = root["verifications"];
                if (verificationsVal.isArray()) {
                    for (auto const& v : verificationsVal) {
                        if (!v.isObject()) continue;

                        if (video.empty() && v.contains("video_url")) {
                            auto videoUrl = v["video_url"];
                            if (videoUrl.isString()) {
                                video = videoUrl.asString().unwrapOr("");
                            }
                        }

                        if (v.contains("submitted_by")) {
                            auto sub = v["submitted_by"];
                            if (sub.isObject()) {
                                std::string name = "Unknown";

                                if (sub.contains("global_name")) {
                                    auto globalName = sub["global_name"];
                                    if (globalName.isString()) {
                                        name = globalName.asString().unwrapOr("Unknown");
                                    }
                                } else if (sub.contains("username")) {
                                    auto username = sub["username"];
                                    if (username.isString()) {
                                        name = username.asString().unwrapOr("Unknown");
                                    }
                                }

                                if (std::find(names.begin(), names.end(), name) == names.end()) {
                                    names.push_back(name);
                                }
                            }
                        }
                    }
                }
            }

            std::string finalNames = names.empty() ? "" : (names.size() == 1 ? names[0] : names[0] + " & " + names[1]);
            long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            CacheEntry entry = { finalNames, video, legacy, now };

            VerifierCache::get().insert(key, entry);
            saveCache();

            Loader::get()->queueInMainThread([this, key] {
                std::string currentKey = std::to_string(m_level->m_levelID) + (m_fields->m_is2PMode ? "_2p" : "");
                if (key == currentKey) refreshVerifierInfo();
            });
        });
    }
};