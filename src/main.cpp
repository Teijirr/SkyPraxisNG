#include <SimpleIni.h>
#include <unordered_set>

// ====================== GLOBALS ======================

RE::BGSExplosion* g_explosion = nullptr;

// ---- Settings ----

std::uint32_t                                      g_rollThreshold = 25;
std::unordered_set<std::string>                    g_excludedMods;
std::unordered_set<std::string>                    g_includedMods;
std::vector<std::pair<std::uint32_t, std::string>> g_includedSpellConfig;
std::unordered_set<RE::SpellItem*>                 g_includedSpellForms;

// ---- Spell Data ----

struct SpellData
{
    std::string    name;
    std::uint32_t  school;
    std::uint32_t  minSkill;
    std::string    sourceMod;
    RE::SpellItem* form;
};

std::vector<SpellData> g_spells;

// ---- Spell Cast Tracking ----

std::unordered_map<std::uint32_t, std::uint32_t> g_spellCastCountByTier;
std::mutex g_spellCountMutex;

// ====================== SERIALIZATION ======================

constexpr std::uint32_t kPraxisSerializationType = 'PRAX';
constexpr std::uint32_t kPraxisSerializationVersion = 1;

// ====================== UTILITIES ======================

void PlaySound(const char* soundName)
{
    auto* audioManager = RE::BSAudioManager::GetSingleton();
    if (!audioManager) return;

    RE::BSSoundHandle handle;
    audioManager->GetSoundHandleByName(handle, soundName, 0);
    if (handle.IsValid())
        handle.Play();
}

// ====================== SPELL SCANNING ======================

std::unordered_set<RE::SpellItem*> CollectLearnableSpells(RE::TESDataHandler* dataHandler)
{
    std::unordered_set<RE::SpellItem*> learnableSpells;
    for (auto* book : dataHandler->GetFormArray<RE::TESObjectBOOK>()) {
        if (book && book->TeachesSpell()) {
            if (auto* spell = book->GetSpell())
                learnableSpells.insert(spell);
        }
    }
    SKSE::log::info("Found {} spell tomes", learnableSpells.size());
    return learnableSpells;
}

void ScanAndRegisterSpells(RE::TESDataHandler* dataHandler,
    const std::unordered_set<RE::SpellItem*>& learnableSpells)
{
    constexpr std::array<std::uint32_t, 5> validSchools = { 18, 19, 20, 21, 22 };
    std::set<std::tuple<std::string, std::uint32_t>> seen;
    std::uint32_t count = 0;

    for (auto* spell : dataHandler->GetFormArray<RE::SpellItem>()) {
        if (!spell) continue;

        const char* sourceMod = "unknown";
        if (auto* file = spell->GetFile(0))
            sourceMod = file->fileName;

        bool forceInclude = false;
        if (sourceMod && !g_includedMods.empty() && g_includedMods.count(std::string(sourceMod))) {
            forceInclude = true;
        }

        if (!forceInclude && g_includedSpellForms.count(spell))
            forceInclude = true;

        if (spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell &&
            spell->GetSpellType() != RE::MagicSystem::SpellType::kLesserPower &&
            spell->GetSpellType() != RE::MagicSystem::SpellType::kVoicePower)
        {
            continue;
        }

        if (spell->data.costOverride == 0 && !forceInclude) continue;
        if (!learnableSpells.count(spell) && !forceInclude) continue;

        auto school = static_cast<std::uint32_t>(spell->GetAssociatedSkill());
        if (!std::count(validSchools.begin(), validSchools.end(), school)) {
            if (!forceInclude)
                continue;
            else
                school = 20; // Default to Destruction for forced includes
        }

        std::uint32_t minSkill = 0;
        auto* effect = spell->GetCostliestEffectItem();
        if (effect && effect->baseEffect)
            minSkill = static_cast<std::uint32_t>(effect->baseEffect->data.minimumSkill);

        if (minSkill != 0 && minSkill != 25 && minSkill != 50 && minSkill != 75 && minSkill != 100) {
            minSkill = 0;
        }

        std::string name = spell->GetName();

        if (name.empty()) continue;
        if (!seen.insert({ name, school }).second) continue;
        if (sourceMod && g_excludedMods.count(std::string(sourceMod))) continue;

        SKSE::log::info("  [{}] '{}' | school={} | minSkill={} | mod={}", count, name, school, minSkill, sourceMod);
        g_spells.push_back({ name, school, minSkill, sourceMod, spell });
        ++count;
    }

    SKSE::log::info("Scan complete. {} spells found.", count);
}

// ====================== SPELL SELECTION & LEARNING ======================

std::vector<const SpellData*> SelectCandidateSpells(
    std::unordered_map<std::uint32_t, std::vector<const SpellData*>>& unlearnedBySchool,
    std::mt19937& rng)
{
    std::vector<const SpellData*> candidates;
    constexpr std::array<std::uint32_t, 5> schoolOrder = { 20, 18, 22, 19, 21 };

    // Pick 1 candidate from each available school
    for (auto schoolId : schoolOrder) {
        auto& pool = unlearnedBySchool[schoolId];
        if (pool.empty()) continue;

        std::uniform_int_distribution<std::size_t> dist(0, pool.size() - 1);
        std::size_t idx = dist(rng);
        candidates.push_back(pool[idx]);
        pool.erase(pool.begin() + idx);
    }

    // Fill remaining slots up to 5 candidates if necessary
    if (candidates.size() < 5) {
        std::vector<const SpellData*> backup;
        for (auto schoolId : schoolOrder) {
            auto& pool = unlearnedBySchool[schoolId];
            backup.insert(backup.end(), pool.begin(), pool.end());
        }

        while (candidates.size() < 5 && !backup.empty()) {
            std::uniform_int_distribution<std::size_t> dist(0, backup.size() - 1);
            std::size_t idx = dist(rng);
            candidates.push_back(backup[idx]);
            backup.erase(backup.begin() + idx);
        }
    }

    return candidates;
}

void PlayExplosionOnPlayer()
{
    if (!g_explosion) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    auto* cell = player->GetParentCell();
    auto* worldspace = player->GetWorldspace();
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler || !cell) return;

    RE::ObjectRefHandle handle = dataHandler->CreateReferenceAtLocation(
        g_explosion,
        player->GetPosition(),
        RE::NiPoint3{ 0.0f, 0.0f, 0.0f },
        cell,
        worldspace,
        nullptr,
        nullptr,
        RE::ObjectRefHandle{},
        false,
        true);

    RE::NiPointer<RE::TESObjectREFR> explosionRefPtr = handle.get();
    if (explosionRefPtr)
    {
        explosionRefPtr->SetActivationBlocked(true);
    }
}

void DebugNotification(const char* a_notification, const char* a_soundToPlay = nullptr, bool a_cancelIfAlreadyQueued = true)
{
    try
    {
        using func_t = decltype(&DebugNotification);
        static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
        return func(a_notification, a_soundToPlay, a_cancelIfAlreadyQueued);
    }
    catch (const std::exception& e)
    {
		SKSE::log::error("Failed to call DebugNotification. This may be due to an unsupported Skyrim version.");
    }
}

bool TryRollTier(std::uint32_t tier, RE::PlayerCharacter* player, std::mt19937& rng)
{
    std::unordered_map<std::uint32_t, std::vector<const SpellData*>> unlearnedBySchool;
    std::size_t totalUnlearned = 0;

    for (const auto& entry : g_spells) {
        if (entry.minSkill != tier) continue;
        if (player->HasSpell(entry.form)) continue;
        unlearnedBySchool[entry.school].push_back(&entry);
        totalUnlearned++;
    }

    if (totalUnlearned == 0) {
        SKSE::log::info("Tier {} fully learned. Escalating...", tier);
        return false;
    }

    auto candidates = SelectCandidateSpells(unlearnedBySchool, rng);
    if (!candidates.empty()) {
        std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
        const auto* chosenSpell = candidates[dist(rng)];

        if (chosenSpell && chosenSpell->form) {
            player->AddSpell(chosenSpell->form);
            PlaySound("UISkillIncreaseSD");

			const char* spellName = (chosenSpell->name + " !").c_str();

            DebugNotification(spellName);
            SKSE::log::info("Learned spell: {}", spellName);

            PlayExplosionOnPlayer();
        }
    }

    return true;
}

void OnSpellCastThresholdReached(std::uint32_t minSkill, RE::PlayerCharacter* player)
{
    constexpr std::array<std::uint32_t, 5> tierOrder = { 0, 25, 50, 75, 100 };
    static std::mt19937 rng(std::random_device{}());

    auto tierIt = std::find(tierOrder.begin(), tierOrder.end(), minSkill);
    if (tierIt == tierOrder.end())
        tierIt = tierOrder.begin();

    for (; tierIt != tierOrder.end(); ++tierIt) {
        if (TryRollTier(*tierIt, player, rng)) {
            break;
        }
    }
}

// ====================== SPELL CAST HANDLER ======================

class SpellCastEventHandler : public RE::BSTEventSink<RE::TESSpellCastEvent>
{
public:
    static SpellCastEventHandler* GetSingleton()
    {
        static SpellCastEventHandler instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESSpellCastEvent* a_event,
        RE::BSTEventSource<RE::TESSpellCastEvent>*) override
    {
        if (!a_event || !a_event->object)
            return RE::BSEventNotifyControl::kContinue;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (a_event->object.get() != player)
            return RE::BSEventNotifyControl::kContinue;

        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell);
        if (!spell) return RE::BSEventNotifyControl::kContinue;

        if (!IsValidMagicSchool(spell)) return RE::BSEventNotifyControl::kContinue;

        std::uint32_t minSkill = GetMinSkill(spell);
        std::lock_guard lock(g_spellCountMutex);
        auto& count = g_spellCastCountByTier[minSkill];
        if (++count >= g_rollThreshold) {
            count = 0;
            SKSE::GetTaskInterface()->AddTask([minSkill, player]() {
                OnSpellCastThresholdReached(minSkill, player);
            });
        }

        if (spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
            std::thread([spell, minSkill, player]() {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));

                    auto* casterL = player->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand);
                    auto* casterR = player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
                    bool stillCasting =
                        (casterL && casterL->currentSpell == spell && casterL->state == RE::MagicCaster::State::kCasting) ||
                        (casterR && casterR->currentSpell == spell && casterR->state == RE::MagicCaster::State::kCasting);

                    if (!stillCasting) break;

                    std::lock_guard lock(g_spellCountMutex);
                    auto& count = g_spellCastCountByTier[minSkill];
                    if (++count >= g_rollThreshold) {
                        count = 0;
                        SKSE::GetTaskInterface()->AddTask([minSkill, player]() {
                            OnSpellCastThresholdReached(minSkill, player);
                        });
                        break;
                    }
                }
                }).detach();
        }

        return RE::BSEventNotifyControl::kContinue;
    }

private:
    SpellCastEventHandler() = default;

    static bool IsValidMagicSchool(RE::SpellItem* spell)
    {
        constexpr std::array<std::uint32_t, 5> validSchools = { 18, 19, 20, 21, 22 };
        auto school = static_cast<std::uint32_t>(spell->GetAssociatedSkill());
        return std::count(validSchools.begin(), validSchools.end(), school) > 0;
    }

    static std::uint32_t GetMinSkill(RE::SpellItem* spell)
    {
        if (auto* effect = spell->GetCostliestEffectItem(); effect && effect->baseEffect)
            return static_cast<std::uint32_t>(effect->baseEffect->data.minimumSkill);
        return 0;
    }
};

// ====================== SERIALIZATION ======================

void PraxisSaveCallback(SKSE::SerializationInterface* a_intfc)
{
    if (!a_intfc->OpenRecord(kPraxisSerializationType, kPraxisSerializationVersion)) {
        SKSE::log::error("Failed to open Praxis save record");
        return;
    }

    std::uint32_t size = static_cast<std::uint32_t>(g_spellCastCountByTier.size());
    if (!a_intfc->WriteRecordData(&size, sizeof(size))) {
        SKSE::log::error("Failed to write map size to save game.");
        return;
    }

    for (const auto& [tier, count] : g_spellCastCountByTier) {
        a_intfc->WriteRecordData(&tier, sizeof(tier));
        a_intfc->WriteRecordData(&count, sizeof(count));
    }
}

void PraxisLoadCallback(SKSE::SerializationInterface* a_intfc)
{
    g_spellCastCountByTier.clear();

    std::uint32_t type, version, length;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (type != kPraxisSerializationType) continue;

        if (version != kPraxisSerializationVersion) {
            SKSE::log::warn("Outdated save version for Praxis. Skipping.");
            continue;
        }

        std::uint32_t size = 0;
        if (!a_intfc->ReadRecordData(&size, sizeof(size))) {
            SKSE::log::error("Failed to read map size block.");
            return;
        }

        for (std::uint32_t i = 0; i < size; ++i) {
            std::uint32_t tier = 0, count = 0;
            a_intfc->ReadRecordData(&tier, sizeof(tier));
            a_intfc->ReadRecordData(&count, sizeof(count));
            g_spellCastCountByTier[tier] = count;
        }
    }
}

void PraxisRevertCallback(SKSE::SerializationInterface*)
{
    g_spellCastCountByTier.clear();
}

// ====================== INIT ======================

void OnDataLoaded()
{
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        SKSE::log::error("TESDataHandler not available!");
        return;
    }

    g_explosion = RE::TESForm::LookupByEditorID<RE::BGSExplosion>("ExplosionIllusionLight01");

    for (const auto& [localId, modName] : g_includedSpellConfig) {
        RE::SpellItem* resolvedSpell = dataHandler->LookupForm<RE::SpellItem>(localId, modName);

        if (!resolvedSpell) {
            if (auto* shout = dataHandler->LookupForm<RE::TESShout>(localId, modName)) {
                for (const auto& variation : shout->variations) {
                    if (variation.spell) {
                        g_includedSpellForms.insert(variation.spell);
                    }
                }
                continue;
            }
        }

        if (resolvedSpell) {
            g_includedSpellForms.insert(resolvedSpell);
        }
        else {
            SKSE::log::warn("Praxis: could not resolve included spell/shout 0x{:X} in '{}'", localId, modName);
        }
    }

    auto learnableSpells = CollectLearnableSpells(dataHandler);
    ScanAndRegisterSpells(dataHandler, learnableSpells);

    RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(SpellCastEventHandler::GetSingleton());
}

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    if (a_msg->type == SKSE::MessagingInterface::kDataLoaded)
        OnDataLoaded();
}

void LoadSettings()
{
    CSimpleIniA ini;
    ini.SetUnicode();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const std::string path = "Data/SKSE/Plugins/" + std::string(plugin->GetName()) + ".ini";
    ini.LoadFile(path.c_str());

    g_rollThreshold = static_cast<std::uint32_t>(ini.GetDoubleValue("General", "iRollThreshold", 50));

    const std::string excludedModsStr = ini.GetValue("General", "sExcludeMods", "");
    if (!excludedModsStr.empty()) {
        std::stringstream ss(excludedModsStr);
        std::string modName;
        while (std::getline(ss, modName, ',')) {
            auto start = std::find_if(modName.begin(), modName.end(), [](unsigned char c) { return !std::isspace(c); });
            auto end = std::find_if(modName.rbegin(), modName.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
            if (start < end)
                g_excludedMods.insert(std::string(start, end));
        }
    }

    const std::string includedModsStr = ini.GetValue("General", "sIncludeMods", "");
    if (!includedModsStr.empty()) {
        std::stringstream ss(includedModsStr);
        std::string modName;
        while (std::getline(ss, modName, ',')) {
            auto start = std::find_if(modName.begin(), modName.end(), [](unsigned char c) { return !std::isspace(c); });
            auto end = std::find_if(modName.rbegin(), modName.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
            if (start < end)
                g_includedMods.insert(std::string(start, end));
        }
    }

    const std::string includedSpellsStr = ini.GetValue("General", "sIncludeSpells", "");
    if (!includedSpellsStr.empty()) {
        std::stringstream ss(includedSpellsStr);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto start = std::find_if(token.begin(), token.end(), [](unsigned char c) { return !std::isspace(c); });
            auto end = std::find_if(token.rbegin(), token.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
            if (start >= end) continue;
            std::string entry(start, end);

            auto sep = entry.find('~');
            if (sep == std::string::npos) continue;

            std::string idStr = entry.substr(0, sep);
            std::string modStr = entry.substr(sep + 1);

            try {
                std::uint32_t localId = static_cast<std::uint32_t>(std::stoul(idStr, nullptr, 16));
                g_includedSpellConfig.emplace_back(localId, modStr);
            }
            catch (...) {
                SKSE::log::warn("Praxis: failed to parse sIncludeSpells entry '{}'", entry);
            }
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    LoadSettings();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener("SKSE", MessageHandler))
        return false;

    auto* serialization = SKSE::GetSerializationInterface();
    if (serialization) {
        serialization->SetUniqueID(kPraxisSerializationType);
        serialization->SetSaveCallback(PraxisSaveCallback);
        serialization->SetLoadCallback(PraxisLoadCallback);
        serialization->SetRevertCallback(PraxisRevertCallback);
        SKSE::log::info("Praxis serialization registered");
    }

    return true;
}