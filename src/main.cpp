#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <matjson.hpp>

#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <system_error>

using namespace geode::prelude;

// ============================================================================
// CONFIGURATION
// ============================================================================

namespace {
    constexpr auto CACHE_FILENAME = "verifier_cache.txt";
    constexpr auto CACHE_ENTRY_SEPARATOR = '\t';
    constexpr auto AREDL_API_BASE_URL = "https://api.aredl.net/v2/api/aredl/levels";
    constexpr auto USER_AGENT = "Mozilla/5.0";
    constexpr auto LABEL_TEXT_SCALE = 0.4f;
    constexpr auto YOUTUBE_ICON_SCALE = 0.32f;
    constexpr auto MENU_Y_OFFSET = -8.0f;
    constexpr auto LABEL_MAX_WIDTH = 120.f;
    constexpr auto YOUTUBE_ICON_X_OFFSET = 8.0f;
    constexpr auto FALLBACK_POSITION_X = 100.f;
    constexpr auto FALLBACK_POSITION_Y = 100.f;
}

// ============================================================================
// CUSTOM EXCEPTIONS
// ============================================================================

class CacheException : public std::runtime_error {
public:
    explicit CacheException(const std::string& message) : std::runtime_error(message) {}
};

class APIException : public std::runtime_error {
public:
    explicit APIException(const std::string& message) : std::runtime_error(message) {}
};

// ============================================================================
// CACHE SYSTEM
// ============================================================================

namespace {
    struct CacheEntry {
        std::string verifierName;
        std::string videoUrl;
        std::chrono::system_clock::time_point fetchedAt;
        
        CacheEntry() = default;
        CacheEntry(std::string verifier, std::string video, std::chrono::system_clock::time_point time)
            : verifierName(std::move(verifier)), videoUrl(std::move(video)), fetchedAt(time) {}
    };

    class VerifierCache {
    private:
        std::unordered_map<int, CacheEntry> m_cache;
        mutable std::shared_mutex m_mutex;
        
    public:
        static VerifierCache& getInstance() {
            static VerifierCache instance;
            return instance;
        }
        
        std::optional<CacheEntry> get(int levelID) const {
            std::shared_lock lock(m_mutex);
            auto it = m_cache.find(levelID);
            return it != m_cache.end() ? std::optional<CacheEntry>{it->second} : std::nullopt;
        }
        
        void put(int levelID, CacheEntry entry) {
            std::unique_lock lock(m_mutex);
            m_cache[levelID] = std::move(entry);
        }
        
        bool contains(int levelID) const {
            std::shared_lock lock(m_mutex);
            return m_cache.contains(levelID);
        }
        
        std::unordered_map<int, CacheEntry> getAll() const {
            std::shared_lock lock(m_mutex);
            return m_cache;
        }
    };
    
    static std::filesystem::path getCacheFilePath() {
        if (auto mod = geode::Mod::get()) {
            return mod->getSaveDir() / CACHE_FILENAME;
        }
        throw CacheException("Unable to get mod directory for cache file");
    }

    static void loadCacheFromDisk() {
        try {
            auto cachePath = getCacheFilePath();
            std::ifstream in(cachePath);
            if (!in) return;

            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;

                auto parts = geode::utils::string::split(line, std::string(1, CACHE_ENTRY_SEPARATOR));
                if (parts.size() < 4) continue;

                try {
                    int id = std::stoi(parts[0]);
                    long long epoch = std::stoll(parts[1]);
                    std::string verifier = parts[2];
                    std::string video = parts[3];

                    VerifierCache::getInstance().put(id, {
                        std::move(verifier), 
                        std::move(video), 
                        std::chrono::system_clock::time_point(std::chrono::seconds(epoch))
                    });
                } catch (const std::exception& e) {
                    log::warn("Failed to parse cache entry: {}", e.what());
                    continue;
                }
            }
        } catch (const std::exception& e) {
            log::error("Failed to load cache from disk: {}", e.what());
        }
    }

    static void saveCacheToDisk() {
        try {
            auto cachePath = getCacheFilePath();
            std::ofstream out(cachePath, std::ios::trunc);
            if (!out) {
                throw CacheException("Failed to open cache file for writing");
            }

            auto cache = VerifierCache::getInstance().getAll();
            for (auto const& [id, entry] : cache) {
                auto epoch = std::chrono::duration_cast<std::chrono::seconds>(entry.fetchedAt.time_since_epoch()).count();
                out << id << CACHE_ENTRY_SEPARATOR << epoch << CACHE_ENTRY_SEPARATOR 
                    << entry.verifierName << CACHE_ENTRY_SEPARATOR << entry.videoUrl << '\n';
            }
        } catch (const std::exception& e) {
            log::error("Failed to save cache to disk: {}", e.what());
        }
    }
}

// ============================================================================
// VERIFIER INFO LAYER
// ============================================================================

class $modify(VerifierInfoLayer, LevelInfoLayer) {
    struct Fields {
        std::string m_videoUrl;
        CCLabelBMFont* m_label;
        CCSprite* m_youtubeIcon;
        CCMenuItemSpriteExtra* m_youtubeBtn;
        async::TaskHolder<web::WebResponse> m_webTask;
        bool m_cacheInitialized;
        
        Fields() : m_label(nullptr), m_youtubeIcon(nullptr), m_youtubeBtn(nullptr), m_cacheInitialized(false) {}
    };

    bool init(GJGameLevel* level, bool p1);
    void updateUI(const std::string& verifier, const std::string& video);
    void onVideoClick(CCObject* sender);
    void fetchAREDLData(int levelID);
    void initializeCacheIfNeeded();
    void createUIElements();
    void positionUIElements();
};

// ============================================================================
// IMPLEMENTATION
// ============================================================================

void VerifierInfoLayer::initializeCacheIfNeeded() {
    if (!m_fields->m_cacheInitialized) {
        loadCacheFromDisk();
        m_fields->m_cacheInitialized = true;
    }
}

void VerifierInfoLayer::createUIElements() {
    // Create loading label
    auto label = CCLabelBMFont::create(". . .", "goldFont.fnt");
    label->setScale(LABEL_TEXT_SCALE);
    m_fields->m_label = label;

    // Create YouTube button with sprite
    auto youtubeIcon = CCSprite::createWithSpriteFrameName("gj_ytIcon_001.png");
    if (!youtubeIcon) {
        log::warn("Failed to load YouTube icon sprite");
        return;
    }
    youtubeIcon->setScale(YOUTUBE_ICON_SCALE);
    
    auto youtubeBtn = CCMenuItemSpriteExtra::create(
        youtubeIcon,
        this,
        menu_selector(VerifierInfoLayer::onVideoClick)
    );
    youtubeBtn->setVisible(false);
    m_fields->m_youtubeBtn = youtubeBtn;
    m_fields->m_youtubeIcon = youtubeIcon;

    // Create menu and add elements
    auto menu = CCMenu::create();
    menu->setID("verifier-menu"_spr);
    menu->addChild(label);
    menu->addChild(youtubeBtn);
    this->addChild(menu);
}

void VerifierInfoLayer::positionUIElements() {
    if (auto menu = this->getChildByID("verifier-menu"_spr)) {
        if (auto creatorMenu = this->getChildByID("creator-info-menu")) {
            auto creatorPos = creatorMenu->getPosition();
            auto yOffset = static_cast<float>(Mod::get()->getSettingValue<double>("y-offset"));
            menu->setPosition({creatorPos.x, creatorPos.y + yOffset});
        } else {
            menu->setPosition({FALLBACK_POSITION_X, FALLBACK_POSITION_Y});
            log::warn("Creator info menu not found, using fallback position");
        }
    }
}

bool VerifierInfoLayer::init(GJGameLevel* level, bool p1) {
    if (!LevelInfoLayer::init(level, p1)) return false;

    // Check if user disabled the mod in settings
    if (!Mod::get()->getSettingValue<bool>("show-label")) return true;

    try {
        initializeCacheIfNeeded();
        createUIElements();
        positionUIElements();

        // Validate level ID
        int levelID = level->m_levelID;
        if (levelID <= 0) {
            log::warn("Invalid level ID: {}", levelID);
            return true;
        }

        // Check cache first
        if (auto cachedEntry = VerifierCache::getInstance().get(levelID)) {
            updateUI(cachedEntry->verifierName, cachedEntry->videoUrl);
            return true;
        }

        // 6 = Extreme Demon
        if (level->m_demonDifficulty == 6) {
            m_fields->m_label->setVisible(true);
            fetchAREDLData(levelID);
        }

    } catch (const std::exception& e) {
        log::error("Error initializing VerifierInfoLayer: {}", e.what());
    }

    return true;
}

void VerifierInfoLayer::updateUI(const std::string& verifier, const std::string& video) {
    if (!m_fields->m_label) {
        log::error("Label is null in updateUI");
        return;
    }

    try {
        const auto displayText = fmt::format("Verified by: {}", verifier);
        m_fields->m_label->setString(displayText.c_str());
        m_fields->m_label->limitLabelWidth(LABEL_MAX_WIDTH, LABEL_TEXT_SCALE, 0.1f);
        m_fields->m_label->setVisible(true);
        // Position label at menu origin
        m_fields->m_label->setPosition({0.f, 0.f});
        m_fields->m_videoUrl = video;

        // Update YouTube button visibility and position
        if (m_fields->m_youtubeBtn) {
            // Only show if video exists AND setting is enabled
            bool showBtn = !video.empty() && Mod::get()->getSettingValue<bool>("show-youtube");
            
            m_fields->m_youtubeBtn->setVisible(showBtn);
            if (showBtn) {
                const auto labelSize = m_fields->m_label->getScaledContentSize();
                m_fields->m_youtubeBtn->setPosition({
                    (labelSize.width / 2) + YOUTUBE_ICON_X_OFFSET, 
                    0.f
                });
            }
        }
    } catch (const std::exception& e) {
        log::error("Error updating UI: {}", e.what());
    }
}

void VerifierInfoLayer::onVideoClick(CCObject* sender) {
    try {
        if (!m_fields->m_videoUrl.empty()) {
            web::openLinkInBrowser(m_fields->m_videoUrl);
        } else {
            log::warn("No video URL available for level");
        }
    } catch (const std::exception& e) {
        log::error("Error opening video link: {}", e.what());
    }
}

void VerifierInfoLayer::fetchAREDLData(int levelID) {
    try {
        const auto url = fmt::format("{}/{}", AREDL_API_BASE_URL, levelID);
        
        m_fields->m_webTask.spawn(
            web::WebRequest().userAgent(USER_AGENT).get(url),
            [this, levelID](web::WebResponse response) {
                try {
                    // Handle HTTP errors
                    if (!response.ok()) {
                        const auto errorMessage = response.code() == 404 ? "Not on AREDL" : "Error fetching";
                        if (m_fields->m_label) {
                            m_fields->m_label->setString(errorMessage);
                        }
                        log::warn("API request failed for level {}: {}", levelID, response.code());
                        return;
                    }

                    // Parse JSON response
                    auto jsonResult = response.json();
                    if (!jsonResult.isOk()) {
                        log::error("Failed to parse JSON response for level {}: {}", levelID, jsonResult.unwrapErr());
                        if (m_fields->m_label) {
                            m_fields->m_label->setString("Parse error");
                        }
                        return;
                    }

                    const auto& levelData = jsonResult.unwrap();
                    std::string verifierName = "Unknown";
                    std::string videoUrl;

                    // Extract verification data safely
                    if (levelData.contains("verifications")) {
                        const auto verifications = levelData["verifications"].asArray().unwrapOr(std::vector<matjson::Value>{});
                        if (!verifications.empty()) {
                            const auto& firstVerif = verifications[0];
                            videoUrl = firstVerif["video_url"].asString().unwrapOr("");

                            if (firstVerif.contains("submitted_by")) {
                                const auto& user = firstVerif["submitted_by"];
                                verifierName = user["global_name"].asString().unwrapOr(
                                    user["name"].asString().unwrapOr("Unknown")
                                );
                            }
                        }
                    }

                    // Update UI and cache
                    updateUI(verifierName, videoUrl);
                    VerifierCache::getInstance().put(levelID, {
                        verifierName, 
                        videoUrl, 
                        std::chrono::system_clock::now()
                    });
                    saveCacheToDisk();
                    
                } catch (const std::exception& e) {
                    log::error("Error processing API response for level {}: {}", levelID, e.what());
                    if (m_fields->m_label) {
                        m_fields->m_label->setString("Processing error");
                    }
                }
            }
        );
    } catch (const std::exception& e) {
        log::error("Error fetching AREDL data for level {}: {}", levelID, e.what());
        if (m_fields->m_label) {
            m_fields->m_label->setString("Fetch error");
        }
    }
}
