#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <matjson.hpp>

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace geode::prelude;

namespace {
    constexpr auto CACHE_FILENAME = "verifier_cache.txt";
    constexpr auto CACHE_ENTRY_SEPARATOR = '\t';

    constexpr auto API_URL_CLASSIC = "https://api.aredl.net/v2/api/aredl/levels";
    constexpr auto API_URL_PLATFORMER = "https://api.aredl.net/v2/api/arepl/levels";

    constexpr auto USER_AGENT = "Geode-AREDL-Mod/2.0";
    constexpr auto LABEL_TEXT_SCALE = 0.4f;
    constexpr auto YOUTUBE_ICON_SCALE = 0.32f;
    constexpr auto LABEL_MAX_WIDTH = 120.f;
    constexpr auto YOUTUBE_ICON_X_OFFSET = 8.0f;
    constexpr auto FALLBACK_POSITION_X = 100.f;
    constexpr auto FALLBACK_POSITION_Y = 100.f;
}

// ============================================================================
// CACHE
// ============================================================================

struct CacheEntry {
    std::string verifierName;
    std::string videoUrl;
    bool isLegacy;
    std::chrono::system_clock::time_point fetchedAt;
};

class VerifierCache {
    std::unordered_map<int, CacheEntry> m_cache;
    mutable std::shared_mutex m_mutex;

public:
    static VerifierCache& get() {
        static VerifierCache instance;
        return instance;
    }

    std::optional<CacheEntry> fetch(int levelID) const {
        std::shared_lock lock(m_mutex);
        if (auto it = m_cache.find(levelID); it != m_cache.end())
            return it->second;
        return std::nullopt;
    }

    void insert(int levelID, CacheEntry entry) {
        std::unique_lock lock(m_mutex);
        m_cache[levelID] = std::move(entry);
    }

    void clear() {
        std::unique_lock lock(m_mutex);
        m_cache.clear();
    }

    std::unordered_map<int, CacheEntry> dump() const {
        std::shared_lock lock(m_mutex);
        return m_cache;
    }
};

static std::filesystem::path getCachePath() {
    return Mod::get()->getSaveDir() / CACHE_FILENAME;
}

static void loadCache() {
    auto path = getCachePath();
    if (!std::filesystem::exists(path)) return;

    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto parts = string::split(line, std::string(1, CACHE_ENTRY_SEPARATOR));
        if (parts.size() < 4) continue;

        try {
            int id = std::stoi(parts[0]);
            long long epoch = std::stoll(parts[1]);
            bool legacy = (parts.size() >= 5 && parts[4] == "1");

            VerifierCache::get().insert(id, {
                parts[2], parts[3], legacy,
                std::chrono::system_clock::time_point(std::chrono::seconds(epoch))
            });
        } catch (...) {}
    }
}

static void saveCache() {
    std::ofstream out(getCachePath(), std::ios::trunc);
    for (const auto& [id, entry] : VerifierCache::get().dump()) {
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(entry.fetchedAt.time_since_epoch()).count();
        out << id << CACHE_ENTRY_SEPARATOR << epoch << CACHE_ENTRY_SEPARATOR
            << entry.verifierName << CACHE_ENTRY_SEPARATOR << entry.videoUrl
            << CACHE_ENTRY_SEPARATOR << (entry.isLegacy ? "1" : "0") << '\n';
    }
}

// ============================================================================
// SETTINGS
// ============================================================================

$execute {
    listenForSettingChanges<bool>("clear-cache-btn", [](bool value) {
        if (value) {
            VerifierCache::get().clear();
            std::error_code ec;
            std::filesystem::remove(getCachePath(), ec);

            Loader::get()->queueInMainThread([] {
                FLAlertLayer::create("Success", "Verifier cache cleared.", "OK")->show();
                Mod::get()->setSettingValue("clear-cache-btn", false);
            });
        }
    });
}

// ============================================================================
// HOOKS
// ============================================================================

class $modify(VerifierInfoLayer, LevelInfoLayer) {
    struct Fields {
        std::string m_videoUrl;
        CCLabelBMFont* m_label = nullptr;
        CCMenuItemSpriteExtra* m_youtubeBtn = nullptr;
        async::TaskHolder<web::WebResponse> m_webTask;
        bool m_cacheLoaded = false;
    };

    bool init(GJGameLevel* level, bool p1) {
        if (!LevelInfoLayer::init(level, p1)) return false;
        if (!Mod::get()->getSettingValue<bool>("show-label")) return true;

        if (!m_fields->m_cacheLoaded) {
            loadCache();
            m_fields->m_cacheLoaded = true;
        }

        buildUI();
        refreshLayout();

        if (level->m_levelID <= 0) return true;

        if (auto entry = VerifierCache::get().fetch(level->m_levelID)) {
            updateUI(entry->verifierName, entry->videoUrl, entry->isLegacy);
        }
        else if (level->m_demonDifficulty == 6) {
            m_fields->m_label->setVisible(true);
            fetchData(level);
        }

        return true;
    }

    void buildUI() {
        auto label = CCLabelBMFont::create("Checking List...", "goldFont.fnt");
        label->setScale(LABEL_TEXT_SCALE);
        label->setVisible(false);
        m_fields->m_label = label;

        auto ytSprite = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png");
        ytSprite->setScale(YOUTUBE_ICON_SCALE);

        auto ytBtn = CCMenuItemSpriteExtra::create(ytSprite, this, menu_selector(VerifierInfoLayer::onVideo));
        ytBtn->setVisible(false);
        m_fields->m_youtubeBtn = ytBtn;

        auto menu = CCMenu::create();
        menu->setID("verifier-menu"_spr);
        menu->addChild(label);
        menu->addChild(ytBtn);
        this->addChild(menu);
    }

    void refreshLayout() {
        auto menu = this->getChildByID("verifier-menu"_spr);
        if (!menu) return;

        auto creatorMenu = this->getChildByID("creator-info-menu");
        if (creatorMenu) {
            auto pos = creatorMenu->getPosition();
            float yOff = static_cast<float>(Mod::get()->getSettingValue<double>("y-offset"));
            float xOff = 0.f;

            if (Mod::get()->getSettingValue<std::string>("label-alignment") == "Left") {
                xOff = -20.f;
            }
            menu->setPosition({pos.x + xOff, pos.y + yOff});
        } else {
            menu->setPosition({FALLBACK_POSITION_X, FALLBACK_POSITION_Y});
        }
    }

    void updateUI(const std::string& verifier, const std::string& video, bool isLegacy) {
        if (!m_fields->m_label) return;

        m_fields->m_videoUrl = video;

        std::string finalName = verifier;
        if (m_level->m_twoPlayerMode && !verifier.empty()) {
            finalName += " (Solo)";
        }

        std::string text = finalName.empty() ? "No Info" : fmt::format("Verified by: {}", finalName);

        m_fields->m_label->setString(text.c_str());
        m_fields->m_label->limitLabelWidth(LABEL_MAX_WIDTH, LABEL_TEXT_SCALE, 0.1f);
        m_fields->m_label->setVisible(true);

        bool gray = isLegacy && Mod::get()->getSettingValue<bool>("legacy-color");
        m_fields->m_label->setColor(gray ? ccColor3B{160, 160, 160} : ccColor3B{255, 255, 255});

        bool leftAlign = Mod::get()->getSettingValue<std::string>("label-alignment") == "Left";
        m_fields->m_label->setAnchorPoint(leftAlign ? CCPoint{0.f, 0.5f} : CCPoint{0.5f, 0.5f});
        m_fields->m_label->setPosition(leftAlign ? CCPoint{-50.f, 0.f} : CCPoint{0.f, 0.f});

        if (m_fields->m_youtubeBtn) {
            bool show = !video.empty() && Mod::get()->getSettingValue<bool>("show-youtube");
            m_fields->m_youtubeBtn->setVisible(show);

            if (show) {
                float w = m_fields->m_label->getScaledContentSize().width;
                float x = leftAlign ? (-50.f + w + YOUTUBE_ICON_X_OFFSET)
                                    : ((w / 2) + YOUTUBE_ICON_X_OFFSET);
                m_fields->m_youtubeBtn->setPosition({x, 0.f});
            }
        }
    }

    void onVideo(CCObject*) {
        if (!m_fields->m_videoUrl.empty()) web::openLinkInBrowser(m_fields->m_videoUrl);
    }

    void fetchData(GJGameLevel* level) {
        int id = level->m_levelID;
        bool platformer = level->isPlatformer();

        // Always fetch base endpoint to get Solo verification
        std::string url = fmt::format("{}/{}",
            platformer ? API_URL_PLATFORMER : API_URL_CLASSIC, id);

        m_fields->m_webTask.spawn(
            web::WebRequest().userAgent(USER_AGENT).get(url),
            [this, id](web::WebResponse res) {
                if (!res.ok()) {
                    if (res.code() == 404) {
                        VerifierCache::get().insert(id, {"", "", false, std::chrono::system_clock::now()});
                        Loader::get()->queueInMainThread([this] {
                            if (m_fields->m_label) m_fields->m_label->setVisible(false);
                        });
                    }
                    return;
                }

                auto json = res.json();
                if (!json.isOk()) return;

                auto data = json.unwrap();
                std::string verifier = "Unknown";
                std::string video = "";
                bool found = false;
                bool legacy = false;

                if (data.contains("legacy"))
                    legacy = data["legacy"].asBool().unwrapOr(false);

                if (data.contains("verifications")) {
                    if (auto arr = data["verifications"].asArray(); arr.isOk()) {
                        auto& list = arr.unwrap();
                        if (!list.empty()) {
                            auto& v = list[0];
                            video = v["video_url"].asString().unwrapOr("");
                            if (v.contains("submitted_by")) {
                                auto u = v["submitted_by"];
                                auto g = u["global_name"].asString();
                                verifier = g.isOk() ? g.unwrap() : u["name"].asString().unwrapOr("Unknown");
                                found = true;
                            }
                        }
                    }
                }

                if (found) {
                    VerifierCache::get().insert(id, {verifier, video, legacy, std::chrono::system_clock::now()});
                    saveCache();
                    Loader::get()->queueInMainThread([this, verifier, video, legacy] {
                        updateUI(verifier, video, legacy);
                    });
                }
            }
        );
    }
};