#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <matjson.hpp>
#include <matjson/std.hpp>

#include <chrono>
#include <shared_mutex>
#include <unordered_map>

using namespace geode::prelude;

static constexpr auto CACHE_FILE = "verifier_cache.json";
static constexpr auto CLASSIC_API = "https://api.aredl.net/v2/api/aredl/levels";
static constexpr auto PLATFORMER_API = "https://api.aredl.net/v2/api/arepl/levels";
static constexpr auto UA = "Geode-AREDL-Mod/3.0";

struct CacheEntry {
    std::string verifier;
    std::string video;
    bool legacy{};
    long long cachedAt{};
};

static std::string jsonStr(matjson::Value const& obj, std::string_view key) {
    if (obj.contains(key)) {
        if (auto val = obj[key]; val.isString()) return val.asString().unwrapOr("");
    }
    return "";
}

static bool jsonBool(matjson::Value const& obj, std::string_view key) {
    if (obj.contains(key)) {
        if (auto val = obj[key]; val.isBool()) return val.asBool().unwrapOr(false);
    }
    return false;
}

static long long jsonInt(matjson::Value const& obj, std::string_view key) {
    if (obj.contains(key)) {
        if (auto val = obj[key]; val.isNumber()) return static_cast<long long>(val.asInt().unwrapOr(0));
    }
    return 0;
}

template<>
struct matjson::Serialize<CacheEntry> {
    static Result<CacheEntry> from_json(Value const& val) {
        if (!val.isObject()) return Err("expected object");
        return geode::Ok(CacheEntry{
            jsonStr(val, "verifier"),
            jsonStr(val, "video"),
            jsonBool(val, "legacy"),
            jsonInt(val, "timestamp")
        });
    }

    static Value to_json(CacheEntry const& e) {
        return matjson::makeObject({
            {"verifier", e.verifier},
            {"video", e.video},
            {"legacy", e.legacy},
            {"timestamp", e.cachedAt}
        });
    }
};

class VerifierCache {
    std::unordered_map<std::string, CacheEntry> m_data;
    mutable std::shared_mutex m_mtx;

public:
    static VerifierCache& get() {
        static VerifierCache inst;
        return inst;
    }

    std::optional<CacheEntry> find(std::string const& key) const {
        std::shared_lock lk(m_mtx);
        auto it = m_data.find(key);
        return it != m_data.end() ? std::optional(it->second) : std::nullopt;
    }

    void store(std::string const& key, CacheEntry entry) {
        std::unique_lock lk(m_mtx);
        m_data[key] = std::move(entry);
    }

    std::unordered_map<std::string, CacheEntry> snapshot() const {
        std::shared_lock lk(m_mtx);
        return m_data;
    }
};

static long long now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static bool cachingEnabled() {
    return !Mod::get()->getSettingValue<bool>("disable-cache");
}

static std::filesystem::path cachePath() {
    return Mod::get()->getSaveDir() / CACHE_FILE;
}

static void saveCache() {
    if (!cachingEnabled()) return;

    auto all = VerifierCache::get().snapshot();
    auto obj = matjson::Value::object();
    for (auto const& [key, entry] : all) {
        obj.set(key, matjson::Serialize<CacheEntry>::to_json(entry));
    }

    if (auto res = file::writeString(cachePath(), obj.dump(matjson::NO_INDENTATION)); !res) {
        log::error("Failed to save cache: {}", res.unwrapErr());
    }
}

static void loadCache() {
    if (!cachingEnabled()) return;

    std::error_code ec;
    if (!std::filesystem::exists(cachePath(), ec) || ec) return;

    auto res = file::readJson(cachePath());
    if (!res) return;

    auto root = res.unwrap();
    if (!root.isObject()) return;

    for (auto const& [key, val] : root) {
        if (auto parsed = matjson::Serialize<CacheEntry>::from_json(val)) {
            VerifierCache::get().store(key, parsed.unwrap());
        }
    }
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

        m_fields->m_label = CCLabelBMFont::create("", "goldFont.fnt");
        m_fields->m_label->setScale(0.4f);

        bool leftAlign = Mod::get()->getSettingValue<std::string>("label-alignment") == "Left";
        m_fields->m_label->setAnchorPoint(leftAlign ? ccp(0, 0.5f) : ccp(0.5f, 0.5f));

        m_fields->m_labelBtn = CCMenuItemSpriteExtra::create(
            m_fields->m_label, this, menu_selector(VerifierInfoLayer::onToggleMode)
        );
        m_fields->m_labelBtn->setVisible(false);

        auto ytIcon = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png");
        ytIcon->setScale(0.32f);
        m_fields->m_ytBtn = CCMenuItemSpriteExtra::create(
            ytIcon, this, menu_selector(VerifierInfoLayer::onVideo)
        );
        m_fields->m_ytBtn->setVisible(false);

        menu->addChild(m_fields->m_labelBtn);
        menu->addChild(m_fields->m_ytBtn);
        this->addChild(menu);

        if (auto anchor = this->getChildByID("creator-info-menu")) {
            float yOff = static_cast<float>(Mod::get()->getSettingValue<double>("y-offset"));
            menu->setPosition(anchor->getPosition() + ccp(0, yOff));
        }
    }

    void onToggleMode(CCObject* sender) {
        if (!m_level->m_twoPlayerMode) return;

        m_fields->m_duo = !m_fields->m_duo;
        refreshVerifierInfo();

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

    void refreshVerifierInfo() {
        if (cachingEnabled()) {
            if (auto cached = VerifierCache::get().find(levelKey())) {
                applyEntry(*cached, m_fields->m_duo);
                return;
            }
        }

        m_fields->m_label->setString("Checking...");
        m_fields->m_labelBtn->setVisible(true);
        m_fields->m_ytBtn->setVisible(false);
    }

    void applyEntry(CacheEntry const& entry, bool duo) {
        if (entry.verifier.empty()) {
            if (duo == m_fields->m_duo) {
                m_fields->m_labelBtn->setVisible(false);
                m_fields->m_ytBtn->setVisible(false);
            }
            return;
        }

        m_fields->m_videoUrl = entry.video;

        std::string prefix = duo
            ? "[2P] "
            : (m_level->m_twoPlayerMode ? "[Solo] Verified by: " : "Verified by: ");

        m_fields->m_label->setString((prefix + entry.verifier).c_str());

        bool grayed = entry.legacy && Mod::get()->getSettingValue<bool>("legacy-color");
        m_fields->m_label->setFntFile(grayed ? "bigFont.fnt" : "goldFont.fnt");

        float scale = grayed ? 0.35f : 0.4f;
        m_fields->m_label->setScale(scale);
        m_fields->m_label->limitLabelWidth(200.f, scale, 0.1f);

        m_fields->m_labelBtn->setContentSize(m_fields->m_label->getScaledContentSize());
        m_fields->m_label->setPosition(m_fields->m_labelBtn->getContentSize() / 2);
        m_fields->m_labelBtn->setVisible(true);
        m_fields->m_labelBtn->setEnabled(m_level->m_twoPlayerMode);

        bool showVideo = !entry.video.empty() && Mod::get()->getSettingValue<bool>("show-youtube");
        m_fields->m_ytBtn->setVisible(showVideo);
        if (showVideo) {
            m_fields->m_ytBtn->setPosition({
                m_fields->m_label->getScaledContentSize().width / 2 + 8.f, 0
            });
        }

        m_fields->m_labelBtn->updateSprite();
    }

    void onVideo(CCObject*) {
        if (!m_fields->m_videoUrl.empty()) {
            web::openLinkInBrowser(m_fields->m_videoUrl);
        }
    }

    void fetchData(std::string key, bool duo) {
        auto baseUrl = m_level->isPlatformer() ? PLATFORMER_API : CLASSIC_API;
        auto url = fmt::format(fmt::runtime("{}/{}"), baseUrl, key);
        auto& holder = duo ? m_fields->m_duoTask : m_fields->m_soloTask;

        holder.spawn(web::WebRequest().userAgent(UA).get(url), [this, key](web::WebResponse res) {
            if (!res.ok()) {
                VerifierCache::get().store(key, {"", "", false, now()});
                saveCache();
                return;
            }

            auto parsed = res.json();
            if (!parsed.isOk()) return;
            auto root = parsed.unwrap();

            std::vector<std::string> names;
            std::string video;
            bool legacy = jsonBool(root, "legacy");

            if (root.contains("verifications")) {
                auto arr = root["verifications"];
                if (arr.isArray()) {
                    for (auto const& v : arr) {
                        if (!v.isObject()) continue;

                        if (video.empty()) {
                            video = jsonStr(v, "video_url");
                        }

                        if (v.contains("submitted_by")) {
                            auto sub = v["submitted_by"];
                            if (!sub.isObject()) continue;

                            std::string name = jsonStr(sub, "global_name");
                            if (name.empty()) name = jsonStr(sub, "username");
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

            VerifierCache::get().store(key, {display, video, legacy, now()});
            saveCache();

            if (key == levelKey()) refreshVerifierInfo();
        });
    }
};

bool VerifierInfoLayer::init(GJGameLevel* level, bool p1) {
    if (!LevelInfoLayer::init(level, p1)) return false;
    if (!Mod::get()->getSettingValue<bool>("show-label")) return true;

    buildUI();

    if (level->m_levelID > 0 && level->m_demonDifficulty >= 5) {
        refreshVerifierInfo();

        auto id = std::to_string(level->m_levelID);
        fetchData(id, false);

        if (level->m_twoPlayerMode) {
            fetchData(id + "_2p", true);
        }
    }

    return true;
}