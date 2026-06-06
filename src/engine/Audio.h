// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include "AudioTypes.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// Forward-declare miniaudio types (never leak into headers)
struct ma_engine;
struct ma_sound;

namespace sv {

// ── Audio event definition (parsed from bank.json) ──────────────

struct VariantCondition {
    std::string param;
    float       min = 0.f;
    float       max = 1.f;
};

struct AudioVariant {
    std::vector<std::string> files;
    VariantMode              mode        = VariantMode::Random;
    float                    weight      = 1.0f;
    float                    pitchJitter = 0.0f;
    float                    volumeJitterDb = 0.0f;
    bool                     hasCondition = false;
    VariantCondition         condition;
    // Shuffle state (for non-repeating playback)
    mutable std::vector<int> shuffleOrder;
    mutable int              shuffleIndex = 0;
};

struct AudioEventDef {
    std::string              name;
    SoundBus                 bus              = SoundBus::SFX;
    bool                     spatial          = false;
    bool                     looping          = false;
    int                      maxVoices        = 8;
    int                      cooldownMs       = 0;
    std::string              attenuationProfile = "default";
    std::vector<std::string> tags;
    std::vector<AudioVariant> variants;

    // Runtime cooldown tracking
    mutable double           lastPlayTime     = 0.0;
    mutable int              activeVoices     = 0;
};

// ── Sound slot (active playing sound) ───────────────────────────

struct SoundSlot {
    ma_sound*     sound      = nullptr;
    SoundHandle   handle     = 0;
    std::string   eventName;
    SoundBus      bus        = SoundBus::SFX;
    bool          active     = false;
    bool          looping    = false;
    std::vector<std::string> tags;
};

// ── Audio engine (Layer 3 — owns miniaudio) ─────────────────────

class Audio {
public:
    bool init();
    void shutdown();

    // Call each frame with listener transform (from active camera)
    void update(const glm::vec3& listenerPos, const glm::vec3& forward, const glm::vec3& up);

    // Load a JSON audio bank (event name → AudioEventDef)
    bool loadBank(const std::string& path);

    // Post a named audio event (global). Returns SoundHandle.
    SoundHandle postEvent(const char* eventName);
    // Post a spatial audio event at world position.
    SoundHandle postEventAt(const char* eventName, const glm::vec3& pos);

    // Stop a sound by handle
    void stopEvent(SoundHandle handle);
    // Stop all sounds with a given tag
    void stopByTag(const char* tag);
    // Stop all sounds
    void stopAll();

    // Check if a handle is still playing
    bool isPlaying(SoundHandle handle) const;

    // Named parameters (for variant conditions)
    void  setParam(const char* name, float value);
    float getParam(const char* name) const;

    // Environment tags (stored for future reverb/filter routing)
    void setEnvironment(const char* tag, bool active);
    bool getEnvironment(const char* tag) const;

    // Volume control by bus
    void  setVolume(SoundBus bus, float vol);
    float getVolume(SoundBus bus) const;
    void  setVolume(int bus, float vol)  { if (bus >= 0 && bus < (int)SoundBus::COUNT) setVolume((SoundBus)bus, vol); }
    float getVolume(int bus) const       { if (bus >= 0 && bus < (int)SoundBus::COUNT) return getVolume((SoundBus)bus); return 0.f; }

    int activeSoundCount() const;

private:
    SoundHandle postEventInternal(const char* eventName, const glm::vec3* pos);
    int         findFreeSlot() const;
    void        cleanupFinished();
    float       computeVolume(SoundBus bus, float eventVol) const;
    const AudioVariant* selectVariant(const AudioEventDef& def) const;
    const std::string&  selectFile(const AudioVariant& var) const;

    ma_engine*          m_engine = nullptr;
    SoundHandle         m_nextHandle = 1;

    static constexpr int MAX_SOUNDS = 128;
    SoundSlot           m_slots[MAX_SOUNDS];

    // Event bank
    std::unordered_map<std::string, AudioEventDef> m_events;

    // Named parameters
    std::unordered_map<std::string, float> m_params;

    // Environment tags
    std::unordered_map<std::string, bool> m_envTags;

    // Per-bus volumes (default 1.0)
    float m_busVolumes[(int)SoundBus::COUNT] = { 1.f, 1.f, 1.f, 1.f, 1.f };

    // Audio file base path
    std::string m_basePath = "data/audio/";
};

} // namespace sv
