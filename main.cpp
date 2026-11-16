#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>

using namespace geode::prelude;

// ---------------------------------------------------------------
// Datos: nivel -> inicio (start pos) -> final -> cuántas veces
// Los porcentajes se guardan en centésimas (20.01% = 2001)
// para evitar errores con decimales.
// ---------------------------------------------------------------
using RunMap = std::map<int, std::map<int, int>>;
static std::map<int, RunMap> g_runs;

static int toKey(float percent) {
    int key = static_cast<int>(std::round(percent * 100.f));
    if (key < 0) key = 0;
    if (key > 10000) key = 10000;
    return key;
}

// 1000 -> "10",  2001 -> "20.01"
static std::string pct(int key) {
    if (key % 100 == 0) return fmt::format("{}", key / 100);
    return fmt::format("{:.2f}", key / 100.f);
}

// Cargar y guardar en el archivo de datos del mod (persiste entre sesiones)
static void loadRuns() {
    g_runs.clear();
    auto data = Mod::get()->getSavedValue<std::string>("runs-data", "");
    std::istringstream in(data);
    int level, start, end, count;
    while (in >> level >> start >> end >> count) {
        g_runs[level][start][end] = count;
    }
}

static void saveRuns() {
    std::ostringstream out;
    for (auto const& [level, runMap] : g_runs) {
        for (auto const& [start, endMap] : runMap) {
            for (auto const& [end, count] : endMap) {
                out << level << ' ' << start << ' ' << end << ' ' << count << '\n';
            }
        }
    }
    Mod::get()->setSavedValue<std::string>("runs-data", out.str());
}

// Mejor final alcanzado desde un start pos de un nivel
static int bestOf(int level, int startKey) {
    auto levelIt = g_runs.find(level);
    if (levelIt == g_runs.end()) return 0;
    auto startIt = levelIt->second.find(startKey);
    if (startIt == levelIt->second.end() || startIt->second.empty()) return 0;
    return startIt->second.rbegin()->first;
}

// Texto de los paneles. limit = 5 -> mejores 5, limit = 0 -> todas
static std::string buildRuns(int level, int limit) {
    auto levelIt = g_runs.find(level);
    if (levelIt == g_runs.end() || levelIt->second.empty()) {
        return "Todavia no hay runs guardadas de este nivel.";
    }

    std::string out;
    for (auto const& [start, endMap] : levelIt->second) {
        out += fmt::format("<cy>desde el {}%</c>\n", pct(start));
        int shown = 0;
        for (auto it = endMap.rbegin(); it != endMap.rend(); ++it) {
            if (limit > 0 && shown >= limit) break;
            out += fmt::format("{}-{}%", pct(start), pct(it->first));
            if (it->second > 1) out += fmt::format(" x{}", it->second);
            out += "\n";
            shown++;
        }
        out += "\n";
    }
    return out;
}

$on_mod(Loaded) {
    loadRuns();
}

// ---------------------------------------------------------------
// Contador en pantalla + registro de runs
// ---------------------------------------------------------------
class $modify(TrackerPlayLayer, PlayLayer) {
    struct Fields {
        bool inRun = false;
        int startKey = 0;
        int levelID = 0;
        CCLabelBMFont* label = nullptr;
    };

    void updateLabel(int startKey, int bestKey) {
        if (!m_fields->label) return;
        auto text = fmt::format(
            "desde el {}% - mejor progreso: {}%",
            pct(startKey), pct(bestKey)
        );
        m_fields->label->setString(text.c_str());
        m_fields->label->setVisible(true);
    }

    void recordRun(float endPercent) {
        m_fields->inRun = false;
        // Los niveles locales (id 0) no se registran
        if (m_fields->levelID == 0) return;

        int endKey = toKey(endPercent);
        int startKey = m_fields->startKey;

        g_runs[m_fields->levelID][startKey][endKey] += 1;
        saveRuns();

        // Congela el texto con el mejor progreso de este inicio
        this->updateLabel(startKey, bestOf(m_fields->levelID, startKey));
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->levelID = m_level->m_levelID.value();

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.4f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setOpacity(170);
        auto winSize = CCDirector::get()->getWinSize();
        label->setPosition({10.f, winSize.height - 40.f});
        label->setVisible(false);
        m_uiLayer->addChild(label, 100);
        m_fields->label = label;

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        // En modo práctica no cuenta (los checkpoints cambian todo el tiempo)
        if (m_isPracticeMode) {
            m_fields->inRun = false;
            if (m_fields->label) m_fields->label->setVisible(false);
            return;
        }

        m_fields->startKey = toKey(this->getCurrentPercent());
        m_fields->inRun = true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (m_fields->inRun && !m_isPracticeMode && object != m_anticheatSpike) {
            this->recordRun(this->getCurrentPercent());
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (m_fields->inRun && !m_isPracticeMode) {
            this->recordRun(100.f);
        }
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (!m_fields->label) return;
        if (m_isPracticeMode) {
            m_fields->label->setVisible(false);
            return;
        }
        // Si moriste, el texto se queda congelado hasta tu próximo intento
        if (!m_fields->inRun) return;

        int current = toKey(this->getCurrentPercent());
        int bestKey = std::max(current, bestOf(m_fields->levelID, m_fields->startKey));
        this->updateLabel(m_fields->startKey, bestKey);
    }
};

// ---------------------------------------------------------------
// Botones en el menú de pausa: "Top 5" y "Todas"
// ---------------------------------------------------------------
class $modify(TrackerPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();
        menu->setPosition({75.f, 60.f});

        auto topBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Top 5"),
            this,
            menu_selector(TrackerPauseLayer::onTopRuns)
        );
        topBtn->setPosition({0.f, 22.f});
        menu->addChild(topBtn);

        auto allBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Todas"),
            this,
            menu_selector(TrackerPauseLayer::onAllRuns)
        );
        allBtn->setPosition({0.f, -22.f});
        menu->addChild(allBtn);

        this->addChild(menu);
    }

    void showRuns(char const* title, int limit) {
        auto play = PlayLayer::get();
        if (!play) return;
        int level = play->m_level->m_levelID.value();
        auto text = buildRuns(level, limit);
        FLAlertLayer::create(
            nullptr, title, text, "Cerrar", nullptr, 400.f, true, 240.f, 1.f
        )->show();
    }

    void onTopRuns(CCObject*) {
        this->showRuns("Mejores runs", 5);
    }

    void onAllRuns(CCObject*) {
        this->showRuns("Todas las runs", 0);
    }
};
