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
    constexpr auto LEGACY_TEXT_SCALE = 0.35f;
    constexpr auto YOUTUBE_ICON_SCALE = 0.32f;
    constexpr auto LABEL_MAX_WIDTH = 200.f;
    constexpr auto YOUTUBE_ICON_X_OFFSET = 8.0f;
    constexpr auto FALLBACK_POSITION_X = 100.f;
    constexpr auto FALLBACK_POSITION_Y = 100.f;
}

struct CacheEntry {
    std::string verifierName;
    std::string videoUrl;
    bool isLegacy{};
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
        if (const auto it = m_cache.find(levelID); it != m_cache.end())
            return it->second;
        return std::nullopt;
    }

    void insert(const int levelID, CacheEntry entry) {
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
    const auto path = getCachePath();
    if (!std::filesystem::exists(path)) return;

    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto parts = string::split(line, std::string(1, CACHE_ENTRY_SEPARATOR));
        if (parts.size() < 4) continue;

        try {
            const int id = utils::numFromString<int>(parts[0]).unwrapOr(0);
            long long epoch = utils::numFromString<long long>(parts[1]).unwrapOr(0);
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
        const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(entry.fetchedAt.time_since_epoch()).count();
        out << id << CACHE_ENTRY_SEPARATOR << epoch << CACHE_ENTRY_SEPARATOR
            << entry.verifierName << CACHE_ENTRY_SEPARATOR << entry.videoUrl
            << CACHE_ENTRY_SEPARATOR << (entry.isLegacy ? "1" : "0") << '\n';
    }
}

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

class $modify(VerifierInfoLayer, LevelInfoLayer) {
    struct Fields {
        std::string m_videoUrl;
        CCLabelBMFont* m_label = nullptr;
        CCMenuItemSpriteExtra* m_youtubeBtn = nullptr;
        async::TaskHolder<web::WebResponse> m_webTask;
        bool m_cacheLoaded = false;
    };

    auto init(GJGameLevel *level, bool p1) -> bool {
        if (!LevelInfoLayer::init(level, p1)) return false;
        if (!Mod::get()->getSettingValue<bool>("show-label")) return true;

        if (!m_fields->m_cacheLoaded) {
            loadCache();
            m_fields->m_cacheLoaded = true;
        }

        buildUI();
        refreshLayout();

        if (level->m_levelID <= 0) return true;

        if (const auto entry = VerifierCache::get().fetch(level->m_levelID)) {
            if (!entry->verifierName.empty()) {
                updateUI(entry->verifierName, entry->videoUrl, entry->isLegacy);
            } else {
                m_fields->m_label->setVisible(false);
            }
        }
        else if (level->m_demonDifficulty >= 5) {
            m_fields->m_label->setVisible(true);
            m_fields->m_label->setString("Checking List...");
            fetchData(level);
        }

        return true;
    }

    void buildUI() {
        const auto label = CCLabelBMFont::create("", "goldFont.fnt");
        label->setScale(LABEL_TEXT_SCALE);
        label->setVisible(false);
        m_fields->m_label = label;

        const auto ytSprite = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png");
        ytSprite->setScale(YOUTUBE_ICON_SCALE);

        const auto ytBtn = CCMenuItemSpriteExtra::create(ytSprite, this, menu_selector(VerifierInfoLayer::onVideo));
        ytBtn->setVisible(false);
        m_fields->m_youtubeBtn = ytBtn;

        const auto menu = CCMenu::create();
        menu->setID("verifier-menu"_spr);
        menu->addChild(label);
        menu->addChild(ytBtn);
        this->addChild(menu);
    }

    void refreshLayout() {
        const auto menu = this->getChildByID("verifier-menu"_spr);
        if (!menu) return;

        if (const auto creatorMenu = this->getChildByID("creator-info-menu")) {
            const auto pos = creatorMenu->getPosition();
            const auto yOff = static_cast<float>(Mod::get()->getSettingValue<double>("y-offset"));
            float xOff = 0.f;

            if (Mod::get()->getSettingValue<std::string>("label-alignment") == "Left") {
                xOff = -20.f;
            }
            menu->setPosition({pos.x + xOff, pos.y + yOff});
        } else {
            menu->setPosition({FALLBACK_POSITION_X, FALLBACK_POSITION_Y});
        }
    }

    void updateUI(const std::string& verifier, const std::string& video, const bool isLegacy) {
        if (!m_fields->m_label) return;

        if (verifier.empty()) {
            m_fields->m_label->setVisible(false);
            if (m_fields->m_youtubeBtn) m_fields->m_youtubeBtn->setVisible(false);
            return;
        }

        m_fields->m_videoUrl = video;

        std::string finalName = verifier;
        if (m_level->m_twoPlayerMode) {
            finalName += " (Solo)";
        }

        const std::string text = fmt::format("Verified by: {}", finalName);

        m_fields->m_label->setFntFile(isLegacy ? "bigFont.fnt" : "goldFont.fnt");
        m_fields->m_label->setString(text.c_str());

        const float scale = isLegacy ? LEGACY_TEXT_SCALE : LABEL_TEXT_SCALE;
        m_fields->m_label->setScale(scale);
        m_fields->m_label->limitLabelWidth(LABEL_MAX_WIDTH, scale, 0.1f);
        m_fields->m_label->setVisible(true);

        const bool gray = isLegacy && Mod::get()->getSettingValue<bool>("legacy-color");
        m_fields->m_label->setColor(gray ? ccColor3B{160, 160, 160} : ccColor3B{255, 255, 255});

        const bool leftAlign = Mod::get()->getSettingValue<std::string>("label-alignment") == "Left";
        m_fields->m_label->setAnchorPoint(leftAlign ? CCPoint{0.f, 0.5f} : CCPoint{0.5f, 0.5f});
        m_fields->m_label->setPosition(leftAlign ? CCPoint{-50.f, 0.f} : CCPoint{0.f, 0.f});

        if (m_fields->m_youtubeBtn) {
            const bool show = !video.empty() && Mod::get()->getSettingValue<bool>("show-youtube");
            m_fields->m_youtubeBtn->setVisible(show);

            if (show) {
                const float w = m_fields->m_label->getScaledContentSize().width;
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
        const bool platformer = level->isPlatformer();

        const std::string url = fmt::format("{}/{}",
            platformer ? API_URL_PLATFORMER : API_URL_CLASSIC, id);

        m_fields->m_webTask.spawn(
            web::WebRequest().userAgent(USER_AGENT).get(url),
            [this, id](const web::WebResponse& res) {
                if (!res.ok()) {
                    VerifierCache::get().insert(id, {"", "", false, std::chrono::system_clock::now()});
                    saveCache();
                    Loader::get()->queueInMainThread([this] {
                        if (m_fields->m_label) m_fields->m_label->setVisible(false);
                    });
                    return;
                }

                auto json = res.json();
                if (!json.isOk()) return;

                auto data = json.unwrap();
                std::string verifier;
                std::string video;
                bool legacy = false;

                if (data.contains("legacy"))
                    legacy = data["legacy"].asBool().unwrapOr(false);

                if (data.contains("verifications")) {
                    if (auto arr = data["verifications"].asArray(); arr.isOk()) {
                        if (auto& list = arr.unwrap(); !list.empty()) {
                            auto& v = list[0];
                            video = v["video_url"].asString().unwrapOr("");
                            if (v.contains("submitted_by")) {
                                const auto& u = v["submitted_by"];
                                auto g = u["global_name"].asString();
                                verifier = g.isOk() ? g.unwrap() : u["name"].asString().unwrapOr("Unknown");
                            }
                        }
                    }
                }

                VerifierCache::get().insert(id, {verifier, video, legacy, std::chrono::system_clock::now()});
                saveCache();

                Loader::get()->queueInMainThread([this, verifier, video, legacy] {
                    updateUI(verifier, video, legacy);
                });
            }
        );
    }
};