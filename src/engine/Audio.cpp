// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "Audio.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <random>
#include <numeric>

namespace sv {

// Thread-local RNG for jitter and variant selection
static std::mt19937& rng() {
    static thread_local std::mt19937 gen(std::random_device{}());
    return gen;
}

// ── Bus name parsing ────────────────────────────────────────────

static SoundBus busFromString(const std::string& s) {
    if (s == "master")  return SoundBus::Master;
    if (s == "sfx")     return SoundBus::SFX;
    if (s == "ambient") return SoundBus::Ambient;
    if (s == "music")   return SoundBus::Music;
    if (s == "ui")      return SoundBus::UI;
    return SoundBus::SFX;
}

static VariantMode modeFromString(const std::string& s) {
    if (s == "round_robin") return VariantMode::RoundRobin;
    if (s == "shuffle")     return VariantMode::Shuffle;
    return VariantMode::Random;
}

// ── Init / Shutdown ─────────────────────────────────────────────

bool Audio::init()
{
    m_engine = new ma_engine;
    ma_engine_config cfg = ma_engine_config_init();
    cfg.listenerCount = 1;

    ma_result result = ma_engine_init(&cfg, m_engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[Audio] Failed to init miniaudio engine (error %d). Running silent.\n", result);
        delete m_engine;
        m_engine = nullptr;
        return false;
    }

    fprintf(stderr, "[Audio] miniaudio engine initialized.\n");
    return true;
}

void Audio::shutdown()
{
    if (!m_engine) return;

    stopAll();
    ma_engine_uninit(m_engine);
    delete m_engine;
    m_engine = nullptr;
    fprintf(stderr, "[Audio] Shutdown.\n");
}

// ── Bank loading ────────────────────────────────────────────────

bool Audio::loadBank(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "[Audio] Cannot open bank: %s\n", path.c_str());
        return false;
    }

    try {
        nlohmann::json root;
        f >> root;

        for (auto& [name, obj] : root.items()) {
            AudioEventDef def;
            def.name     = name;
            def.bus      = busFromString(obj.value("bus", "sfx"));
            def.spatial  = obj.value("spatial", false);
            def.looping  = obj.value("looping", false);
            def.maxVoices = obj.value("maxVoices", 8);
            def.cooldownMs = obj.value("cooldownMs", 0);
            def.attenuationProfile = obj.value("attenuationProfile", "default");

            if (obj.contains("tags")) {
                for (auto& t : obj["tags"])
                    def.tags.push_back(t.get<std::string>());
            }

            if (obj.contains("variants")) {
                for (auto& vobj : obj["variants"]) {
                    AudioVariant var;
                    if (vobj.contains("files")) {
                        for (auto& fp : vobj["files"])
                            var.files.push_back(fp.get<std::string>());
                    }
                    var.mode          = modeFromString(vobj.value("mode", "random"));
                    var.weight        = vobj.value("weight", 1.0f);
                    var.pitchJitter   = vobj.value("pitchJitter", 0.0f);
                    var.volumeJitterDb = vobj.value("volumeJitterDb", 0.0f);

                    if (vobj.contains("condition")) {
                        auto& cobj = vobj["condition"];
                        var.hasCondition    = true;
                        var.condition.param = cobj.value("param", "");
                        var.condition.min   = cobj.value("min", 0.0f);
                        var.condition.max   = cobj.value("max", 1.0f);
                    }

                    def.variants.push_back(std::move(var));
                }
            }

            m_events[name] = std::move(def);
        }

        fprintf(stderr, "[Audio] Loaded bank: %s (%zu events)\n", path.c_str(), m_events.size());
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[Audio] Bank parse error: %s\n", e.what());
        return false;
    }
}

// ── Variant selection ───────────────────────────────────────────

const AudioVariant* Audio::selectVariant(const AudioEventDef& def) const
{
    if (def.variants.empty()) return nullptr;

    // Filter variants by condition
    std::vector<const AudioVariant*> candidates;
    float totalWeight = 0.f;

    for (auto& var : def.variants) {
        if (var.hasCondition) {
            auto it = m_params.find(var.condition.param);
            float val = (it != m_params.end()) ? it->second : 0.f;
            if (val < var.condition.min || val > var.condition.max)
                continue;
        }
        candidates.push_back(&var);
        totalWeight += var.weight;
    }

    if (candidates.empty()) {
        // No condition matched — fall back to first variant
        return &def.variants[0];
    }

    if (candidates.size() == 1) return candidates[0];

    // Weighted random selection
    std::uniform_real_distribution<float> dist(0.f, totalWeight);
    float r = dist(rng());
    float accum = 0.f;
    for (auto* v : candidates) {
        accum += v->weight;
        if (r <= accum) return v;
    }
    return candidates.back();
}

const std::string& Audio::selectFile(const AudioVariant& var) const
{
    if (var.files.empty()) {
        static const std::string empty;
        return empty;
    }
    if (var.files.size() == 1) return var.files[0];

    switch (var.mode) {
        case VariantMode::RoundRobin: {
            int idx = var.shuffleIndex % (int)var.files.size();
            var.shuffleIndex++;
            return var.files[idx];
        }
        case VariantMode::Shuffle: {
            if (var.shuffleOrder.empty() || var.shuffleIndex >= (int)var.shuffleOrder.size()) {
                // Rebuild shuffle order
                var.shuffleOrder.resize(var.files.size());
                std::iota(var.shuffleOrder.begin(), var.shuffleOrder.end(), 0);
                std::shuffle(var.shuffleOrder.begin(), var.shuffleOrder.end(), rng());
                var.shuffleIndex = 0;
            }
            int idx = var.shuffleOrder[var.shuffleIndex++];
            return var.files[idx];
        }
        case VariantMode::Random:
        default: {
            std::uniform_int_distribution<int> dist(0, (int)var.files.size() - 1);
            return var.files[dist(rng())];
        }
    }
}

// ── Post event ──────────────────────────────────────────────────

SoundHandle Audio::postEvent(const char* eventName)
{
    return postEventInternal(eventName, nullptr);
}

SoundHandle Audio::postEventAt(const char* eventName, const glm::vec3& pos)
{
    return postEventInternal(eventName, &pos);
}

SoundHandle Audio::postEventInternal(const char* eventName, const glm::vec3* pos)
{
    if (!m_engine) return INVALID_SOUND;

    auto it = m_events.find(eventName);
    if (it == m_events.end()) {
        fprintf(stderr, "[Audio] Unknown event: %s\n", eventName);
        return INVALID_SOUND;
    }

    auto& def = it->second;

    // Max voices check
    if (def.activeVoices >= def.maxVoices) return INVALID_SOUND;

    // Cooldown check
    if (def.cooldownMs > 0) {
        double now = static_cast<double>(ma_engine_get_time_in_milliseconds(m_engine));
        if (now - def.lastPlayTime < (double)def.cooldownMs)
            return INVALID_SOUND;
        def.lastPlayTime = now;
    }

    // Select variant and file
    const AudioVariant* var = selectVariant(def);
    if (!var || var->files.empty()) return INVALID_SOUND;

    const std::string& file = selectFile(*var);
    if (file.empty()) return INVALID_SOUND;

    // Find free slot
    int slot = findFreeSlot();
    if (slot < 0) {
        fprintf(stderr, "[Audio] No free sound slots\n");
        return INVALID_SOUND;
    }

    // Build full path
    std::string fullPath = m_basePath + file;

    // Create and start sound
    ma_sound* snd = new ma_sound;
    ma_uint32 flags = 0;
    if (!def.spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result result = ma_sound_init_from_file(m_engine, fullPath.c_str(), flags, nullptr, nullptr, snd);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[Audio] Failed to load: %s (error %d)\n", fullPath.c_str(), result);
        delete snd;
        return INVALID_SOUND;
    }

    // Apply looping
    ma_sound_set_looping(snd, def.looping ? MA_TRUE : MA_FALSE);

    // Apply volume with jitter
    float vol = computeVolume(def.bus, 1.0f);
    if (var->volumeJitterDb > 0.f) {
        std::uniform_real_distribution<float> jDist(-var->volumeJitterDb, var->volumeJitterDb);
        float jitterDb = jDist(rng());
        vol *= std::pow(10.f, jitterDb / 20.f);
    }
    ma_sound_set_volume(snd, vol);

    // Apply pitch jitter
    if (var->pitchJitter > 0.f) {
        std::uniform_real_distribution<float> pDist(-var->pitchJitter, var->pitchJitter);
        float pitchShift = pDist(rng());
        // Convert semitones to pitch ratio
        ma_sound_set_pitch(snd, std::pow(2.f, pitchShift / 12.f));
    }

    // Apply spatial position
    if (pos && def.spatial) {
        ma_sound_set_position(snd, pos->x, pos->y, pos->z);
    }

    // Start playback
    ma_sound_start(snd);

    // Fill slot
    SoundHandle handle = m_nextHandle++;
    if (m_nextHandle == 0) m_nextHandle = 1; // skip 0 (invalid)

    auto& s = m_slots[slot];
    s.sound     = snd;
    s.handle    = handle;
    s.eventName = eventName;
    s.bus       = def.bus;
    s.active    = true;
    s.looping   = def.looping;
    s.tags      = def.tags;

    def.activeVoices++;

    return handle;
}

// ── Stop ────────────────────────────────────────────────────────

void Audio::stopEvent(SoundHandle handle)
{
    if (!m_engine || handle == INVALID_SOUND) return;
    for (auto& s : m_slots) {
        if (s.active && s.handle == handle) {
            ma_sound_stop(s.sound);
            ma_sound_uninit(s.sound);
            delete s.sound;
            s.sound = nullptr;
            s.active = false;
            // Decrement active voices
            auto it = m_events.find(s.eventName);
            if (it != m_events.end() && it->second.activeVoices > 0)
                it->second.activeVoices--;
            return;
        }
    }
}

void Audio::stopByTag(const char* tag)
{
    if (!m_engine) return;
    std::string t(tag);
    for (auto& s : m_slots) {
        if (s.active) {
            for (auto& st : s.tags) {
                if (st == t) {
                    ma_sound_stop(s.sound);
                    ma_sound_uninit(s.sound);
                    delete s.sound;
                    s.sound = nullptr;
                    s.active = false;
                    auto it = m_events.find(s.eventName);
                    if (it != m_events.end() && it->second.activeVoices > 0)
                        it->second.activeVoices--;
                    break;
                }
            }
        }
    }
}

void Audio::stopAll()
{
    if (!m_engine) return;
    for (auto& s : m_slots) {
        if (s.active && s.sound) {
            ma_sound_stop(s.sound);
            ma_sound_uninit(s.sound);
            delete s.sound;
            s.sound = nullptr;
            s.active = false;
        }
    }
    // Reset all active voice counts
    for (auto& [name, def] : m_events)
        def.activeVoices = 0;
}

// ── Query ───────────────────────────────────────────────────────

bool Audio::isPlaying(SoundHandle handle) const
{
    if (!m_engine || handle == INVALID_SOUND) return false;
    for (auto& s : m_slots) {
        if (s.active && s.handle == handle)
            return true;
    }
    return false;
}

int Audio::activeSoundCount() const
{
    int count = 0;
    for (auto& s : m_slots)
        if (s.active) count++;
    return count;
}

// ── Parameters ──────────────────────────────────────────────────

void Audio::setParam(const char* name, float value)
{
    m_params[name] = value;
}

float Audio::getParam(const char* name) const
{
    auto it = m_params.find(name);
    return (it != m_params.end()) ? it->second : 0.f;
}

void Audio::setEnvironment(const char* tag, bool active)
{
    m_envTags[tag] = active;
}

bool Audio::getEnvironment(const char* tag) const
{
    auto it = m_envTags.find(tag);
    return (it != m_envTags.end()) ? it->second : false;
}

// ── Volume ──────────────────────────────────────────────────────

void Audio::setVolume(SoundBus bus, float vol)
{
    int idx = (int)bus;
    if (idx < 0 || idx >= (int)SoundBus::COUNT) return;
    m_busVolumes[idx] = std::max(0.f, std::min(1.f, vol));

    // Update all active sounds on this bus
    if (!m_engine) return;
    for (auto& s : m_slots) {
        if (s.active && s.sound && s.bus == bus) {
            ma_sound_set_volume(s.sound, computeVolume(bus, 1.0f));
        }
    }
}

float Audio::getVolume(SoundBus bus) const
{
    int idx = (int)bus;
    if (idx < 0 || idx >= (int)SoundBus::COUNT) return 0.f;
    return m_busVolumes[idx];
}

float Audio::computeVolume(SoundBus bus, float eventVol) const
{
    float master = m_busVolumes[(int)SoundBus::Master];
    float busVol = (bus == SoundBus::Master) ? 1.f : m_busVolumes[(int)bus];
    return master * busVol * eventVol;
}

// ── Frame update ────────────────────────────────────────────────

void Audio::update(const glm::vec3& listenerPos, const glm::vec3& forward, const glm::vec3& up)
{
    if (!m_engine) return;

    // Update listener transform
    ma_engine_listener_set_position(m_engine, 0, listenerPos.x, listenerPos.y, listenerPos.z);
    ma_engine_listener_set_direction(m_engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine, 0, up.x, up.y, up.z);

    // Cleanup finished non-looping sounds
    cleanupFinished();
}

void Audio::cleanupFinished()
{
    for (auto& s : m_slots) {
        if (s.active && s.sound && !s.looping) {
            if (!ma_sound_is_playing(s.sound)) {
                ma_sound_uninit(s.sound);
                delete s.sound;
                s.sound = nullptr;
                s.active = false;
                auto it = m_events.find(s.eventName);
                if (it != m_events.end() && it->second.activeVoices > 0)
                    it->second.activeVoices--;
            }
        }
    }
}

int Audio::findFreeSlot() const
{
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (!m_slots[i].active) return i;
    }
    return -1;
}

} // namespace sv
