// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// Skinned mesh animation test harness (extended ).
// extended with AssetBrowser drag source + ThumbnailCache drop
// target + offscreen GPU thumbnail bake (renders mesh into a 256x256 PNG
// sibling at "<asset>.thumb.png").
// Loads a CC5 rigged .glb, plays animations via AnimationStateMachine or
// blend tree, renders via SkinnedMeshPass, and shows an ImGui debug overlay.
// IK (two-bone + aim) and root motion extraction.
// adds `--render-golden <dir>` which captures 4 deterministic
// PNG goldens (skinned_mesh, shadow_pass, post_process, imgui_layer) via
// the existing offscreen bake targets and exits. Tests in sv_tests diff
// these captures against committed tests/golden/*.png via the svtest
// PngDiff helper.

#include "SceneUBO.h"

#include <engine/CrtCompat.h>
#include <engine/EngineBase.h>
#include <engine/BaseSystemContext.h> // sv::PerformanceContext
#include <engine/Window.h>
#include <engine/Camera.h>
#include <engine/vk/VkContext.h>
#include <engine/vk/VkSwapchain.h>
#include <engine/vk/VkBuffer.h>
#include <engine/vk/VkTexture.h>
#include <engine/vk/VkMesh.h>
#include <engine/vk/VkShader.h>
#include <engine/AnimationTypes.h>
#include <engine/AnimationSystem.h>
#include <engine/AnimatorComponent.h>
#include <engine/AnimationStateMachine.h>
#include <engine/BlendTree.h>
#include <engine/passes/SkinnedMeshPass.h>
#include <engine/MaterialPipeline.h>
#include <engine/Types.h>
#include <engine/ui/HelpOverlay.h>
#include <engine/ui/ImGuiLayer.h>
#include <engine/vk/PipelineCache.h> //
//
#include <engine/AssetBrowser.h>
#include <engine/AssetWatcher.h>
#include <engine/ThumbnailCache.h>
// MsQuic client + replication wire
#include <engine/net/MsQuicTransport.h>
#include <engine/net/ReplicationProtocol.h>
#include <engine/CameraComponent.h>
#include <engine/MaterialComponent.h>
#include <engine/NetTransform.h>
#include <engine/ParentLink.h>
#include <engine/ReplicationRegistry.h>
// collaborative edit transaction wire types + permission scope
#include <engine/EditTransaction.h>
#include <engine/PermissionScope.h>
// asset sync helpers
#include <engine/AssetPersistence.h>
#include <engine/AssetUploadClient.h>
#include <engine/Sha256.h>
// plain-TCP editor bridge for Blender live link
#include <engine/net/EditorBridge.h>
#include <StratumVVersion.h>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <GLFW/glfw3.h>

// stb_image_write implementation lives in stratumv.lib (DevServer.cpp).
// Lab harness only needs the function declarations.
#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ── Asset path ────────────────────────────────────────────────────
// Default sample mesh path, relative to the working directory.
static const char* ASSET_PATH = "assets/sample_character.fbx";


static constexpr uint32_t MAX_FRAMES = 2;

// ── Frame sync ────────────────────────────────────────────────────
struct FrameSync {
    VkCommandBuffer cmd            = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    // renderFinished lives per swapchain IMAGE (not per frame in
    // flight) — see TestEngine::m_renderFinishedPerImage. A per-frame
    // semaphore can be re-signaled while an earlier present still has
    // it pending (VUID-vkQueueSubmit-pSignalSemaphores-00067).
    VkFence         inFlight       = VK_NULL_HANDLE;
};

// ══════════════════════════════════════════════════════════════════
class TestEngine : public sv::EngineBase {
public:
    TestEngine() {
        setDevServerPort(0); // disable DevServer for test

        // Harness-specific help topics, appended after the engine
        // defaults (Getting started, Camera). See docs/UI_STYLE.md
        // for the user-facing text rules.
        m_helpOverlay.addTopic("Editing",
            "Once connected with Editor permissions, the arrow keys "
            "nudge your avatar in the horizontal plane, and the "
            "buttons in the Network Demo panel do the same.\n\n"
            "Ctrl+Z asks the server to undo the last edit and "
            "Ctrl+Y to redo it.");
        m_helpOverlay.addTopic("Panels",
            "Asset Browser lists importable assets and bakes "
            "thumbnails. Thumbnail Bake shows the bake queue. "
            "Animation Debug exposes the skeleton and blend state. "
            "Network Demo shows the connection, your identity and "
            "the edit counters. Replicated Assets tracks files "
            "received from the server.");
        m_helpOverlay.addTopic("Quitting",
            "Press Esc to close the viewer.");
    }

    // Open the help overlay at startup (--show-help). Used by the
    // visual verification rig and handy for first-time users.
    void setShowHelp(bool v) { m_helpOverlay.setVisible(v); }

    // visual-checkpoint helpers — consumed by main.
    void setAutoBake(bool v) { m_autoBake = v; }
    void setAutoExit(bool v) { m_autoExit = v; }

    // auto-upload the given relative path at `frame`. Used
    // by the s_edit2b_checkpoint.png capture rig to drive an upload
    // without manual ImGui interaction. Empty path disables the
    // auto-upload behaviour.
    void setAutoUploadPath(const std::string& relPath) { m_autoUploadRelPath = relPath; }
    void setAutoUploadFrame(uint32_t frame)            { m_autoUploadFrame = frame; }

    // Override the PPM output filename (default "capture.ppm"). Used
    // when running multiple clients side by side.
    void setCaptureName(const std::string& name) { m_captureName = name; }

    // Override the frame number at which the auto-exit capture
    // triggers. Useful for staggered two-client rigs where the
    // capture window needs to outlast the orchestration overhead.
    void setCaptureFrame(uint32_t frame) { m_captureFrameOverride = frame; }

    // golden capture mode. When a non-empty directory is
    // supplied, onInit forces the scene into a deterministic state
    // (paused animation, rest pose, zero dt) and onFrame captures the
    // four goldens and exits after a few warm-up frames. The directory
    // must already exist — the CI fixture creates it before launch.
    void setRenderGoldensMode(const std::string& outDir)
    {
        m_renderGoldensMode = !outDir.empty();
        m_goldenOutDir      = outDir;
    }

    // point the harness at a running stratumv_server. When
    // non-empty, onInit() spins up a client-role sv::net::Transport,
    // connects to the given host:port, installs a datagram handler, and
    // starts receiving server-owned NetTransform snapshots that the
    // network demo ImGui panel visualises.
    void setNetworkTarget(const std::string& hostColonPort)
    {
        m_netConnectTarget = hostColonPort;
    }
    void setNetworkTickHz(uint32_t hz)
    {
        if (hz > 0) m_netTickHz = hz;
    }

    // optional plain-TCP bridge for Blender live link.
    // Non-zero port enables the bridge; the listener starts up as part
    // of initNetworking() after the QUIC connection is welcomed.
    void setEditorBridgePort(uint16_t port)
    {
        m_editorBridgePort = port;
    }

protected:
    bool onInit() override;
    void onShutdown() override;
    bool onFrame(float dt) override;

private:
    void initFrameSync();
    void initDescriptors();
    void updateSceneUBO(float dt);
    void recordFrame(uint32_t imageIndex);
    void drawDebugUI();

    //
    void initNetworking();      // called from onInit if --connect was given
    void drainNetInbox();       // drain datagram queue each frame
    void drawNetworkDemoPanel();// ImGui overlay + 2D XZ canvas
    void shutdownNetworking();  // called from onShutdown

    // reliable-stream message dispatch + edit transactions
    void drainNetReliableInbox();          // handles Spawn/Despawn/Undo/Redo
    void sendAvatarMove(float dx, float dy, float dz); // SetField request
    void sendUndoRequest();                              // Undo transaction
    void sendRedoRequest();                              // Redo transaction

    // asset sync — upload, receive, draw
    bool uploadAssetFromDisk(const std::string& relPath,
                             const std::string& absPath);
    void drainAssetInbox();                // runs every frame before asset panel
    void drawReplicatedAssetsPanel();     // ImGui panel listing received assets

    // editor bridge hooks — called from the main
    // thread at well-defined points in the frame. The UI status
    // line is drawn inline inside drawNetworkDemoPanel().
    void maybeStartEditorBridge();        // fires once after welcome
    void publishEntityToBridge(uint32_t entityId);
    void pumpBridgeMoves();               // translate MoveSelf -> SetField
    void pumpBridgeAssets();              // asset push
    void pumpBridgeParents();             // parent sync
    void pumpBridgeLights();              // light sync
    void pumpBridgeCameras();             // camera sync
    void pumpBridgeMaterials();           // material sync
    bool uploadAssetBytes(const std::string& relPath,
                          sv::AssetKind      kind,
                          const uint8_t*     data,
                          size_t             size);

    // Core
    sv::Window    m_window;
    sv::VkCtx     m_vkCtx;
    sv::VkSwap    m_swapchain;
    sv::Camera    m_camera;

    // Frame sync
    FrameSync m_frames[MAX_FRAMES];
    std::vector<VkSemaphore> m_renderFinishedPerImage; // one per swapchain image
    uint32_t  m_currentFrame = 0;
    uint32_t  m_frameCount   = 0;

    // Depth + motion vector dummy
    sv::DepthImage m_depth;
    sv::ColorImage m_motionVec;

    // Scene UBO
    SceneUBO  m_sceneUBO{};
    sv::VkBuf m_uboBuffers[MAX_FRAMES];
    VkDescriptorSetLayout m_sceneDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool        = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSets[MAX_FRAMES]{};

    // Mesh + skeleton + animation
    sv::VkMesh          m_mesh;
    sv::SkeletonHandle  m_skeleton;
    sv::AnimationSystem m_animSys;
    sv::AnimationInstance m_animInst;
    std::vector<sv::AnimationClip> m_clips;
    std::unique_ptr<sv::AnimationStateMachine> m_stateMachine;
    bool m_paused = false;

    // Blend tree + partial blending
    std::unique_ptr<sv::BlendSpace1D> m_blendSpace;
    std::unique_ptr<sv::RestPoseNode> m_restPoseNode;
    std::unique_ptr<sv::ClipNode>     m_upperClipNode; // for partial blend demo
    ozz::vector<ozz::math::SimdFloat4> m_upperBodyMask;
    std::vector<std::string> m_jointNames; // for ImGui combo
    int   m_splitJointIdx     = -1;
    float m_blendParam        = 0.0f;
    float m_upperLayerWeight  = 0.5f;
    bool  m_useBlendTree      = false;
    bool  m_enablePartialBlend = false;

    // Skinned rendering
    sv::SkinnedMeshPass m_skinnedPass;
    uint32_t m_boneOffset = 0;

    // persistent VkPipelineCache. Loaded from disk before
    // SkinnedMeshPass::init, handed to the pass, then re-saved on
    // clean shutdown so the next run skips driver shader compilation.
    sv::PipelineCache m_pipelineCache;
    std::string       m_pipelineCachePath = "pipeline_cache.bin";
    double            m_skinnedPassInitMs = 0.0; // cold/warm boot measurement

    // Materials
    sv::MaterialPipeline              m_materialPipeline;
    std::vector<sv::SceneMaterialSet> m_materialSets;

    // IK
    bool  m_enableTwoBoneIK = false;
    bool  m_enableAimIK     = false;
    sv::TwoBoneIKSlot m_leftLegIK;    // left foot IK
    sv::TwoBoneIKSlot m_rightLegIK;   // right foot IK
    sv::AimIKSlot     m_headAimIK;    // head look-at
    float m_ikTargetY     = 40.0f;    // foot target height (raised to show IK effect)
    float m_ikTargetSpreadX = 20.0f;  // horizontal foot spread
    glm::vec3 m_aimTarget{0.0f, 150.0f, 200.0f}; // aim target position

    // Root motion
    bool      m_rootMotionEnabled = false;
    glm::vec3 m_rootMotionAccum{0.0f};      // accumulated root motion position
    glm::vec3 m_prevRootPos{0.0f};
    glm::quat m_prevRootRot{1.0f, 0.0f, 0.0f, 0.0f};
    sv::RootMotionDelta m_lastRootDelta;

    // ImGui
    sv::ImGuiLayer m_imguiLayer;

    // Help overlay (F1). Drawn only in the interactive frame path so
    // golden renders and headless runs never see it.
    sv::HelpOverlay m_helpOverlay;

    float m_totalTime = 0.0f;

    // Live performance counters. Populated in onFrame(dt)
    // (frame + avg fps) and recordFrame (draws + tris), displayed in
    // drawDebugUI via the "Performance" section. This is
    // the lab-harness owned instance — games wire their own live
    // PerformanceContext into their SystemContext.perf slot.
    sv::PerformanceContext m_perfContext;
    float                  m_cpuFrameStartSec = 0.0f; // for cpuFrameTimeMs
    uint32_t               m_drawsThisFrame   = 0;
    uint32_t               m_trisThisFrame    = 0;

    // ── replication client state ────────────────────
    // Populated by --connect host:port at main() parse time; the
    // transport + connection are brought up inside onInit() and
    // torn down inside onShutdown().
    std::string                         m_netConnectTarget;  // "" = offline
    uint32_t                            m_netTickHz = 30;    // assumed server rate
    std::unique_ptr<sv::net::Transport> m_netTransport;
    sv::net::Connection                 m_netConn;           // live connection
    bool                                m_netConnected = false;

    // Lock-protected inbound datagram queue. MsQuic's worker thread
    // pushes into m_netInbox; the main thread drains it each frame.
    std::mutex                              m_netInboxMu;
    std::vector<std::vector<uint8_t>>       m_netInbox;
    std::atomic<uint64_t>                   m_netInboundTotal{0};

    // Replicated component state. Double-buffered for client-side
    // interpolation: m_netPrev = last-decoded snapshot, m_netCurrent
    // = most-recent snapshot; displayNetTransform() lerps between
    // them by wall-clock alpha since the last decode.
    sv::NetTransform m_netPrev{};
    sv::NetTransform m_netCurrent{};
    uint32_t         m_netLastTick    = 0;      // max tick index seen
    bool             m_netHaveData    = false;  // set on first valid decode
    double           m_netLastDecodeWallSec = 0.0; // glfwGetTime at decode

    // Visible-facing counters.
    uint64_t m_netDatagramsReceived = 0;
    uint64_t m_netBytesReceived     = 0;
    uint64_t m_netFramesDecoded     = 0;
    uint64_t m_netFramesDropped     = 0;

    // ── schema handshake state ────────────────────
    // A reliable one-shot preamble arrives on a QUIC stream right
    // after the TLS handshake completes. The MsQuic worker thread
    // stashes the parsed result + any diagnostic string into these
    // fields (under m_netSchemaMu), and the main thread renders the
    // state in drawNetworkDemoPanel.
    enum class NetSchemaState : uint8_t {
        Pending   = 0,   // connected but preamble has not arrived yet
        Ok        = 1,   // preamble matched the local registry
        Mismatch  = 2,   // preamble disagrees — connection will close
        Unknown   = 3,   // server-only type flagged (soft diagnostic)
    };
    std::mutex       m_netSchemaMu;
    NetSchemaState   m_netSchemaState   = NetSchemaState::Pending;
    std::string      m_netSchemaDetail;     // e.g. "NetTransform v0x1234 got 0x5678"
    uint32_t         m_netSchemaSemver  = 0;  // packed semver from preamble
    uint16_t         m_netSchemaTypeCount = 0;

    // ── multi-entity replicated state ────────────────────
    // The server now broadcasts snapshots for multiple entities
    // (the orbiting cube on entityId 1 plus per-client avatars on
    // entityId 100+). Each inbound datagram is routed into this
    // map keyed by entityId. drawNetworkDemoPanel iterates the
    // map and renders a dot + nameplate per entity.
    struct ClientEntity {
        uint32_t              entityId      = 0;
        uint32_t              ownerClientId = 0;   // 0 = server-authoritative
        bool                  alive         = true;
        sv::NetTransform      prevState     {};
        sv::NetTransform      currentState  {};
        sv::ParentLink parent {}; //
        sv::LightComponent light {}; // — default disabled
        sv::CameraComponent camera {}; // — default fovDeg=0 = no override
        sv::MaterialComponent material {}; // — default strength=0 = no effect
        uint32_t              lastTick      = 0;
        bool                  haveData      = false;
        double                lastDecodeWallSec = 0.0;
    };
    std::unordered_map<uint32_t, ClientEntity> m_netEntities;

    // ── client identity (from Welcome message) ─────────
    // Populated by the reliable-stream handler when the server
    // sends a kFrameWelcome message. 0 = not-yet-welcomed; the UI
    // disables the edit buttons until we know which avatar we own.
    std::mutex               m_netStateMu;
    uint32_t                 m_netClientId        = 0;
    sv::PermissionScope      m_netScope           = sv::PermissionScope::Spectator;
    uint32_t                 m_netAvatarEntityId  = 0;
    bool                     m_netWelcomed        = false;

    // ── inbound reliable-stream queue for edit txs ─────
    // Schema handshake is handled inline in the worker-thread
    // callback (existing behaviour); EditTransactions
    // are queued here for main-thread processing to avoid touching
    // the entity map under the worker thread.
    std::mutex                        m_netReliableInboxMu;
    std::vector<sv::EditTransaction>  m_netReliableInbox;

    // ── counters for the UI ─────────────────────────────
    uint64_t m_netSetFieldSent    = 0;
    uint64_t m_netUndoSent        = 0;
    uint64_t m_netRedoSent        = 0;
    uint64_t m_netReliableTxApplied = 0;

    // ── editor bridge state ──────────────────────
    // Non-zero port enables the bridge. The listener is started from
    // maybeStartEditorBridge() the first frame after the Welcome
    // message arrives so Hello / entity state / etc. are all filled
    // in before the first external client can connect.
    uint16_t                          m_editorBridgePort    = 0;
    std::unique_ptr<sv::net::EditorBridge> m_editorBridge;
    bool                              m_editorBridgeStarted = false;

    // counters for the ImGui panel.
    uint64_t m_bridgeMoveApplied   = 0;    // MoveSelf → SetField sent
    uint64_t m_bridgeStatePushed   = 0;    // EntityState frames broadcast

    // new counters for asset + parent forwarding.
    uint64_t m_bridgeAssetsReceived = 0;   // assets assembled from Blender
    uint64_t m_bridgeAssetsUploaded = 0;   // assets successfully pushed to server
    uint64_t m_bridgeParentChanges  = 0;   // parent-link setfields applied

    // light forwarding counters.
    // m_bridgeLightsSent bumps when the bridge pumps a SetLight request
    // through as a SetField transaction; m_bridgeLightsApplied bumps
    // when the lab client (including the observer, which has no bridge
    // at all) decodes an inbound LightComponent SetField broadcast.
    uint64_t m_bridgeLightsSent    = 0;
    uint64_t m_bridgeLightsApplied = 0;

    // camera + material forwarding counters. Same
    // shape as the lights pair — *Sent bumps when the bridge pumps a
    // local SetCamera / SetMaterial request out as a SetField, and
    // *Applied bumps on every client (including the observer) when an
    // inbound SetField broadcast is decoded.
    uint64_t m_bridgeCamerasSent      = 0;
    uint64_t m_bridgeCamerasApplied   = 0;
    uint64_t m_bridgeMaterialsSent    = 0;
    uint64_t m_bridgeMaterialsApplied = 0;

    // ── asset sync state ────────────────────────────────
    // Client-side CAS, one per harness instance. Holds both
    // locally-uploaded assets and assets received from the server.
    // The reliable-stream handler pushes inbound AssetAnnounce /
    // AssetChunk messages into the mutex-protected inboxes, and the
    // main thread drains them via drainAssetInbox() at the top of
    // onFrame().
    sv::AssetPersistence m_assetStore;

    // One AssetReceiver per in-flight hash (either client-originated
    // upload where the server has acked NeedChunks, or server-
    // originated broadcast assembly). Keyed by hex digest so both
    // paths can share the same map.
    std::unordered_map<std::string, sv::AssetReceiver> m_assetReceivers;

    // Announce messages the worker thread has pushed but the main
    // thread has not yet drained. Held as raw bytes + msg type so
    // the handler's copy stays simple.
    struct IncomingAssetMsg {
        uint8_t               msgType = 0;    // kFrameAssetAnnounce / Chunk / Ack
        std::vector<uint8_t>  bytes;
    };
    std::mutex                     m_assetInboxMu;
    std::vector<IncomingAssetMsg>  m_assetInbox;

    // UI state for the Replicated Assets panel. Names we have
    // already received + their record hash (first 12 hex chars) so
    // the panel can render a flat list without hitting the map in
    // every ImGui item.
    struct ReceivedAssetRow {
        std::string name;
        std::string hashPrefix;       // 12-char abbreviation
        uint32_t    byteSize = 0;
        uint8_t     assetKind = 0;
        bool        uploadedLocally = false;
    };
    std::vector<ReceivedAssetRow> m_receivedAssetRows;

    // Upload progress tracking. The Asset Browser panel surfaces a
    // small status line ("Uploaded N: <name>") to give the visual
    // checkpoint something to read without needing two ImGui panels
    // wired into the same drop zone.
    std::string m_lastUploadStatus;
    uint64_t    m_assetUploadsSent   = 0;
    uint64_t    m_assetChunksSent    = 0;
    uint64_t    m_assetsReceived     = 0;
    uint64_t    m_assetDedupHits     = 0;  // local dedup (Ack HaveIt from server)

    // One-shot framebuffer capture (saves PPM after N frames)
    void captureFramebuffer(uint32_t imageIndex);
    bool m_captured = false;
    // Visual checkpoint flags (CLI-controlled): auto-bake the first
    // mesh entry, then auto-exit a few frames after capture.
    bool m_autoBake = false;
    bool m_autoExit = false;

    // auto-upload for the checkpoint rig. When set, the
    // harness calls uploadAssetFromDisk on the entry at this
    // relativePath at frame m_autoUploadFrame (default 60 ≈ 1s at
    // 60fps; well before the networked-capture frame 300).
    std::string m_autoUploadRelPath;
    uint32_t    m_autoUploadFrame = 60;
    bool        m_autoUploadDone  = false;

    // Capture filename override (default "capture.ppm").
    std::string m_captureName = "capture.ppm";

    // Capture-frame override. 0 means "use the networked/offline
    // default" (300 or 15 respectively). Non-zero overrides both.
    uint32_t    m_captureFrameOverride = 0;

    // ── render-golden capture state ─────────────────────
    // When enabled via --render-golden <dir>, the harness pauses
    // animation, forces deterministic camera/time, runs captureGoldens
    // after a warm-up frame, and exits. Captures four PNGs into
    // m_goldenOutDir: skinned_mesh, shadow_pass, post_process, imgui_layer.
    bool        m_renderGoldensMode = false;
    std::string m_goldenOutDir;
    bool        m_goldensCaptured   = false;

    // Orchestrates the 4 captures. Returns true on full success, false
    // if any single capture fails (partial writes are still on disk).
    bool captureGoldens(const std::string& outDir);

    // Renders the skinned mesh through SkinnedMeshPass into m_thumbColor
    // + m_thumbDepth with a deterministic camera (mesh AABB fit). Leaves
    // both offscreen images in TRANSFER_SRC_OPTIMAL so the extract*
    // helpers can copy them to staging without re-transitioning.
    // This is a stripped-down cousin of bakeThumbnail — no PNG write,
    // no cache mark, no AABB debug log.
    bool renderSkinnedMeshOffscreen();

    // Renders a fixed ImGui window layout into m_thumbColor. Leaves the
    // color image in TRANSFER_SRC_OPTIMAL, ready for extractColorToPng.
    // Uses ImGuiLayer::render directly; the layout is described in
    // buildGoldenImGuiFrame() below so it stays isolated from
    // drawDebugUI() (which is full of live debug state).
    bool renderImGuiOverlayOffscreen();

    // Build the golden ImGui frame: a fixed-position window with a
    // small number of stable widgets. Called between ImGui::NewFrame
    // and ImGui::Render so the draw data is committed for the render
    // path. No interactive state leaks into the frame.
    void buildGoldenImGuiFrame();

    // Copy a color image (swapchain format) → staging → BGRA→RGBA
    // swizzle → stbi_write_png. The source image MUST already be in
    // TRANSFER_SRC_OPTIMAL layout.
    bool extractColorToPng(sv::ColorImage& src, const std::string& outPath);

    // Copy a depth image (D32_SFLOAT) → staging → linearize → grayscale
    // uint8 → stbi_write_png. Uses a simple `1 - depth` inversion so
    // nearer geometry is brighter and more visible, like a matte shadow
    // map preview. Source MUST be in TRANSFER_SRC_OPTIMAL layout.
    bool extractDepthToPng(sv::DepthImage& src, const std::string& outPath);

    // Synthesize a 256x256 HDR gradient on CPU, apply a Reinhard
    // tonemap + gamma 2.2, encode as 8-bit RGBA PNG. This is a
    // pure-CPU reference for the PostProcess chain — the engine
    // does not currently ship postprocess.vert / tonemap.frag, so
    // the golden only covers the tonemap math until those shaders
    // are added in a follow-up session.
    bool writePostProcessReferencePng(const std::string& outPath);

    // ── AssetBrowser + ThumbnailCache demo ──────────────
    sv::AssetBrowser   m_assetBrowser;
    sv::AssetWatcher   m_assetWatcher;
    sv::ThumbnailCache m_thumbnailCache;
    bool               m_browserScanned = false;

    // Offscreen 256x256 thumbnail render targets (matches the 2-color
    // attachment + depth setup of the existing SkinnedMeshPass pipeline).
    static constexpr uint32_t THUMB_W = 256;
    static constexpr uint32_t THUMB_H = 256;
    sv::ColorImage m_thumbColor;
    sv::ColorImage m_thumbMotion;
    sv::DepthImage m_thumbDepth;

    // dedicated 512x512 render targets for golden image
    // captures. Separate from the thumbnail bake path so asset-browser
    // thumbnails stay at 256x256 (the size the AssetBrowser UI + cache
    // expect) while golden PNGs are large enough for a human reviewer
    // to actually read. Allocated only when --render-golden mode is
    // on; otherwise these stay empty / destroy is a no-op.
    static constexpr uint32_t GOLDEN_W = 512;
    static constexpr uint32_t GOLDEN_H = 512;
    sv::ColorImage m_goldenColor;
    sv::ColorImage m_goldenMotion;
    sv::DepthImage m_goldenDepth;

    // dedicated R8G8B8A8_SRGB target for texture-blit bakes.
    // VkTex loads source textures as RGBA8_SRGB, and blitting them into
    // the swapchain-format (usually BGRA8_SRGB) m_thumbColor would
    // require a component swap on readback. Using a matching format
    // eliminates the need and keeps the texture-bake PNG encode path
    // trivial (no BGRA→RGBA swizzle).
    sv::ColorImage m_thumbBlitColor;

    // Bake state — set from ImGui, processed at the next onFrame tick.
    bool        m_bakeRequested = false;
    std::string m_bakeRelPath;
    std::string m_bakeAbsPath;
    std::string m_lastBakeStatus;

    // Drop target state — last asset dropped onto the bake panel.
    std::string m_dropAssetRelPath;
    std::string m_dropAssetAbsPath;

    // Browser panel selection (index into m_assetBrowser.entries()).
    int m_browserSelected = -1;

    // Render the GPU thumbnail for the currently-loaded mesh into the
    // offscreen targets, copy to staging, encode PNG, and call
    // m_thumbnailCache.markBaked(). Returns true on success.
    bool bakeThumbnail(const std::string& relPath, const std::string& absPath);

    // load a texture asset via sv::VkTex and blit it into
    // m_thumbBlitColor (vkCmdBlitImage with linear filter). Demos the
    // second branch of GPU thumbnail baking — textures don't need
    // a full render pass, only a resize. Also calls markBaked().
    bool bakeTextureThumbnail(const std::string& relPath,
                              const std::string& absPath);

    // bake dispatch — routes to bakeThumbnail or
    // bakeTextureThumbnail based on the AssetKind. Returns true on
    // success and sets m_lastBakeStatus.
    bool dispatchBake(const std::string& relPath, const std::string& absPath);

    // ImGui asset panels (drag source + drop target + cache state).
    void drawAssetPanels();
};

// ══════════════════════════════════════════════════════════════════
bool TestEngine::onInit()
{
    // ── Window ────────────────────────────────────────────────────
    sv::WindowConfig wndCfg;
    wndCfg.title  = "StratumV Skinned Test";
    wndCfg.width  = 1280;
    wndCfg.height = 720;
    wndCfg.vsync  = true;
    if (!m_window.init(wndCfg)) {
        fprintf(stderr, "[TestEngine] Window init failed\n");
        return false;
    }

    // ── Vulkan ────────────────────────────────────────────────────
    if (!m_vkCtx.init(m_window.handle())) {
        fprintf(stderr, "[TestEngine] VkCtx init failed\n");
        return false;
    }
    if (!m_swapchain.init(m_vkCtx, wndCfg.width, wndCfg.height, true)) {
        fprintf(stderr, "[TestEngine] Swapchain init failed\n");
        return false;
    }

    sv::VkShader::initCompiler();

    VkDevice     device = m_vkCtx.device();
    VmaAllocator alloc  = m_vkCtx.allocator();

    // ── Depth buffer + motion vector dummy ────────────────────────
    m_depth     = sv::DepthImage::create(m_vkCtx, wndCfg.width, wndCfg.height);
    m_motionVec = sv::ColorImage::create(m_vkCtx, wndCfg.width, wndCfg.height,
                                          VK_FORMAT_R16G16_SFLOAT);

    // ── Frame sync ────────────────────────────────────────────────
    initFrameSync();

    // ── Descriptors ───────────────────────────────────────────────
    initDescriptors();

    // ── Load mesh (with textures) ───────────────────────────────────
    printf("[TestEngine] Loading %s ...\n", ASSET_PATH);
    if (!m_mesh.loadFromFile(m_vkCtx, ASSET_PATH, true)) {
        fprintf(stderr, "[TestEngine] Failed to load mesh\n");
        return false;
    }
    printf("[TestEngine] Mesh loaded: indexCount=%u, submeshes=%zu, materials=%zu, textures=%zu, isSkinned=%d\n",
           m_mesh.totalIndexCount(), m_mesh.submeshes().size(),
           m_mesh.materials().size(), m_mesh.textures().size(), m_mesh.isSkinned());

    if (!m_mesh.isSkinned()) {
        fprintf(stderr, "[TestEngine] Mesh is NOT skinned — aborting\n");
        return false;
    }

    const auto& skelData = m_mesh.skeleton();
    printf("[TestEngine] Skeleton: %d joints\n", skelData.jointCount());
    for (int i = 0; i < (std::min)(skelData.jointCount(), 10); i++)
        printf("  joint[%d] = '%s' (parent=%d)\n",
               i, skelData.joints[i].name.c_str(), skelData.joints[i].parent);
    if (skelData.jointCount() > 10)
        printf("  ... and %d more joints\n", skelData.jointCount() - 10);

    // ── Build ozz skeleton ────────────────────────────────────────
    m_skeleton = sv::buildSkeleton(skelData);
    if (!m_skeleton) {
        fprintf(stderr, "[TestEngine] buildSkeleton failed\n");
        return false;
    }
    printf("[TestEngine] ozz Skeleton: %d joints (ozz), %d data joints\n",
           m_skeleton.jointCount(), m_skeleton.dataJointCount());

    // ── Animation system + instance ───────────────────────────────
    m_animSys.init(device, alloc, 4096);
    m_animInst = m_animSys.createInstance(m_skeleton);

    // ── Load animation clips ────────────────
    {
        std::string ap(ASSET_PATH);
        auto dot = ap.find_last_of('.');
        std::string ext = (dot != std::string::npos) ? ap.substr(dot) : "";
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".fbx")
            m_clips = sv::loadFbxAnimations(ASSET_PATH, m_skeleton);
        else
            m_clips = sv::loadGltfAnimations(ASSET_PATH, m_skeleton);
    }
    if (m_clips.empty()) {
        printf("[TestEngine] No animations found in %s — T-pose fallback\n", ASSET_PATH);
    } else {
        printf("[TestEngine] Loaded %zu animation clips:\n", m_clips.size());
        for (size_t i = 0; i < m_clips.size(); i++)
            printf("  [%zu] '%s' (%.2fs)\n", i, m_clips[i].name.c_str(), m_clips[i].duration);

        // Create state machine with all clips as looping states
        m_stateMachine = std::make_unique<sv::AnimationStateMachine>();
        for (size_t i = 0; i < m_clips.size(); i++)
            m_stateMachine->addState(m_clips[i].name.c_str(), &m_clips[i], true, 1.0f);

        // Add transitions between consecutive clips (for testing)
        for (size_t i = 0; i + 1 < m_clips.size(); i++) {
            char trigger[32];
            snprintf(trigger, sizeof(trigger), "next_%zu", i);
            m_stateMachine->addTransition(
                m_clips[i].name.c_str(), m_clips[i + 1].name.c_str(),
                0.3f, trigger, sv::TransitionMode::Immediate);
        }

        m_stateMachine->setInitialState(m_clips[0].name.c_str());
    }

    // ── Blend tree nodes ──────────────────────────────────
    {
        // BlendSpace1D: rest pose at 0 → first clip at 1
        std::vector<sv::BlendEntry1D> entries;
        entries.push_back({nullptr, 0.0f}); // rest pose
        if (!m_clips.empty())
            entries.push_back({&m_clips[0], 1.0f}); // first clip
        m_blendSpace = std::make_unique<sv::BlendSpace1D>(entries);
        m_blendSpace->init(m_skeleton);

        // Rest pose node (for partial blend overlay)
        m_restPoseNode = std::make_unique<sv::RestPoseNode>();
        m_restPoseNode->init(m_skeleton);

        // Clip node for upper body demo
        if (!m_clips.empty()) {
            m_upperClipNode = std::make_unique<sv::ClipNode>(&m_clips[0], true, 1.0f);
            m_upperClipNode->init(m_skeleton);
        }

        // Collect joint names for ImGui
        auto jnames = m_skeleton.skeleton->joint_names();
        for (int i = 0; i < m_skeleton.jointCount(); i++)
            m_jointNames.push_back(jnames[i]);

        // Find a spine joint for body split
        const char* splitCandidates[] = {"spine_01", "spine_02", "spine_03",
            "Spine", "Spine1", "Spine2",
            "mixamorig:Spine", "mixamorig:Spine1", "CC_Base_Waist"};
        for (const char* name : splitCandidates) {
            for (int i = 0; i < (int)m_jointNames.size(); i++) {
                if (m_jointNames[i] == name) {
                    m_splitJointIdx = i;
                    break;
                }
            }
            if (m_splitJointIdx >= 0) break;
        }

        if (m_splitJointIdx >= 0) {
            m_upperBodyMask = sv::buildJointMask(
                m_skeleton, m_jointNames[m_splitJointIdx].c_str(), 1.0f, 0.0f);
            printf("[TestEngine] Upper body mask: split at '%s' (joint %d)\n",
                   m_jointNames[m_splitJointIdx].c_str(), m_splitJointIdx);
        } else {
            printf("[TestEngine] No spine joint found for body split\n");
        }
    }

    // ── IK joint detection ──────────────────────────────
    {
        auto jnames = m_skeleton.skeleton->joint_names();
        int numJ = m_skeleton.jointCount();

        // Helper: find ozz joint index by name substring search
        auto findJoint = [&](const char* candidates[], int count) -> int {
            for (int c = 0; c < count; c++) {
                for (int j = 0; j < numJ; j++) {
                    if (strcmp(jnames[j], candidates[c]) == 0) return j;
                }
            }
            return -1;
        };

        // Left leg: upper leg -> knee -> ankle
        // Support CC_Base (CC3/CC4), UE4-style (thigh_l), and Mixamo naming
        const char* lUpLeg[]  = {"thigh_l", "CC_Base_L_Thigh", "mixamorig:LeftUpLeg", "LeftUpLeg"};
        const char* lKnee[]   = {"calf_l", "CC_Base_L_Calf", "mixamorig:LeftLeg", "LeftLeg"};
        const char* lAnkle[]  = {"foot_l", "CC_Base_L_Foot", "mixamorig:LeftFoot", "LeftFoot"};
        m_leftLegIK.startJoint = findJoint(lUpLeg, 4);
        m_leftLegIK.midJoint   = findJoint(lKnee, 4);
        m_leftLegIK.endJoint   = findJoint(lAnkle, 4);
        m_leftLegIK.poleVector = glm::vec3(0.0f, 0.0f, 1.0f); // knees forward
        m_leftLegIK.midAxis    = glm::vec3(1.0f, 0.0f, 0.0f); // knee bends around local X
        m_leftLegIK.soften     = 0.97f;

        // Right leg
        const char* rUpLeg[]  = {"thigh_r", "CC_Base_R_Thigh", "mixamorig:RightUpLeg", "RightUpLeg"};
        const char* rKnee[]   = {"calf_r", "CC_Base_R_Calf", "mixamorig:RightLeg", "RightLeg"};
        const char* rAnkle[]  = {"foot_r", "CC_Base_R_Foot", "mixamorig:RightFoot", "RightFoot"};
        m_rightLegIK.startJoint = findJoint(rUpLeg, 4);
        m_rightLegIK.midJoint   = findJoint(rKnee, 4);
        m_rightLegIK.endJoint   = findJoint(rAnkle, 4);
        m_rightLegIK.poleVector = glm::vec3(0.0f, 0.0f, 1.0f);
        m_rightLegIK.midAxis    = glm::vec3(1.0f, 0.0f, 0.0f);
        m_rightLegIK.soften     = 0.97f;

        // Head aim
        const char* headNames[] = {"head", "CC_Base_Head", "mixamorig:Head", "Head"};
        m_headAimIK.joint = findJoint(headNames, 4);
        m_headAimIK.forward = glm::vec3(0.0f, 0.0f, 1.0f); // head faces +Z
        m_headAimIK.up      = glm::vec3(0.0f, 1.0f, 0.0f);

        bool leftOK  = m_leftLegIK.startJoint >= 0 && m_leftLegIK.midJoint >= 0 && m_leftLegIK.endJoint >= 0;
        bool rightOK = m_rightLegIK.startJoint >= 0 && m_rightLegIK.midJoint >= 0 && m_rightLegIK.endJoint >= 0;
        printf("[TestEngine] IK joints — left leg: %s (%d/%d/%d), right leg: %s (%d/%d/%d), head: %s (%d)\n",
               leftOK ? "OK" : "MISSING", m_leftLegIK.startJoint, m_leftLegIK.midJoint, m_leftLegIK.endJoint,
               rightOK ? "OK" : "MISSING", m_rightLegIK.startJoint, m_rightLegIK.midJoint, m_rightLegIK.endJoint,
               m_headAimIK.joint >= 0 ? "OK" : "MISSING", m_headAimIK.joint);
    }

    // ── Material pipeline ───────────────────────────────
    if (!m_materialPipeline.init(m_vkCtx, 64)) {
        fprintf(stderr, "[TestEngine] MaterialPipeline init failed\n");
        return false;
    }

    // Create per-submesh material descriptor sets
    for (const auto& sub : m_mesh.submeshes()) {
        int matIdx = sub.materialIndex;
        if (matIdx >= 0 && matIdx < (int)m_mesh.materials().size()) {
            m_materialSets.push_back(
                m_materialPipeline.createMaterialSet(m_vkCtx, m_mesh.materials()[matIdx], m_mesh));
        } else {
            // Use first material as fallback (or create default)
            m_materialSets.push_back(
                m_materialPipeline.createMaterialSet(m_vkCtx, m_mesh.materials()[0], m_mesh));
        }
    }
    printf("[TestEngine] Created %zu material descriptor sets\n", m_materialSets.size());

    // ── Pipeline cache ───────────────────────────────────
    // Load before building any graphics pipelines so the driver can
    // reuse compiled state from previous runs.
    m_pipelineCache.load(device, m_vkCtx.physicalDevice(), m_pipelineCachePath);

    // ── Skinned mesh pass ─────────────────────────────────────────
    // time the init call so we can quantify cold vs warm
    // boot cost once a cache has been written to disk.
    const auto passInitT0 = std::chrono::steady_clock::now();
    if (!m_skinnedPass.init(m_vkCtx, m_sceneDescLayout,
                             m_animSys.bonePaletteLayout(),
                             m_materialPipeline.layout(),
                             m_swapchain.format(),
                             m_pipelineCache.handle())) {
        fprintf(stderr, "[TestEngine] SkinnedMeshPass init failed\n");
        return false;
    }
    const auto passInitT1 = std::chrono::steady_clock::now();
    m_skinnedPassInitMs = std::chrono::duration<double, std::milli>(
                              passInitT1 - passInitT0).count();
    printf(" SkinnedMeshPass::init %.2f ms (pipeline cache: %s, %zu bytes)\n",
           m_skinnedPassInitMs,
           m_pipelineCache.loadedFromFile() ? "WARM from disk" : "COLD / empty",
           m_pipelineCache.lastLoadedBytes());

    // ── Camera ────────────────────────────────────────────────────
    m_camera.init(m_window.handle());
    m_camera.setPosition(glm::vec3(0.0f, 1.0f, 5.0f));

    // ── ImGui ─────────────────────────────────────────────────────
    m_imguiLayer.init(m_window.handle(), m_vkCtx,
                      m_swapchain.format(), m_swapchain.imageCount());

    // ── AssetBrowser scan + offscreen thumbnail targets ─
#ifdef STRATUMV_LAB_ASSET_DIR
    if (m_assetBrowser.scan(STRATUMV_LAB_ASSET_DIR)) {
        m_browserScanned = true;
        m_assetBrowser.attachWatcher(&m_assetWatcher);
        printf("[TestEngine] AssetBrowser scanned %zu entries from %s\n",
               m_assetBrowser.size(), STRATUMV_LAB_ASSET_DIR);
    } else {
        printf("[TestEngine] AssetBrowser scan FAILED for %s\n",
               STRATUMV_LAB_ASSET_DIR);
    }
#else
    printf("[TestEngine] STRATUMV_LAB_ASSET_DIR undefined — skipping browser scan\n");
#endif

    // Offscreen render targets for thumbnail bake. Color uses the swapchain
    // format so the existing SkinnedMeshPass pipeline (built with
    // m_swapchain.format()) accepts the attachment without rebuild. Motion
    // matches the 2nd color attachment of the pipeline (R16G16_SFLOAT).
    // We add TRANSFER_SRC to the color image so we can copy it to a
    // staging buffer after the offscreen pass completes.
    m_thumbColor  = sv::ColorImage::create(m_vkCtx, THUMB_W, THUMB_H,
        m_swapchain.format(), VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    m_thumbMotion = sv::ColorImage::create(m_vkCtx, THUMB_W, THUMB_H,
        VK_FORMAT_R16G16_SFLOAT);
    m_thumbDepth  = sv::DepthImage::create(m_vkCtx, THUMB_W, THUMB_H);

    // RGBA8 SRGB target dedicated to texture-blit bakes.
    // Needs TRANSFER_SRC (for the final copy to staging) AND
    // TRANSFER_DST (as the blit destination).
    m_thumbBlitColor = sv::ColorImage::create(m_vkCtx, THUMB_W, THUMB_H,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    // freeze scene state for reproducible golden captures.
    // - m_paused=true keeps the animation system from advancing
    //   across the warm-up frames.
    // - Destroy the state machine so the blend path falls through to
    //   the rest-pose fallback (`m_animSys.blend(inst, nullptr, 0)`
    //   → ozz rest-pose T-pose). This sidesteps the issue where a
    //   state machine whose update() never ran emits empty blend
    //   layers and the skinning SSBO ends up with identity/zero
    //   matrices that collapse all vertices to the origin.
    // - Allocate the 512x512 golden render targets on demand so
    //   non-golden runs don't pay the VRAM.
    if (m_renderGoldensMode) {
        m_paused = true;
        m_stateMachine.reset();
        m_useBlendTree = false;
        m_goldenColor  = sv::ColorImage::create(m_vkCtx, GOLDEN_W, GOLDEN_H,
            m_swapchain.format(), VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        m_goldenMotion = sv::ColorImage::create(m_vkCtx, GOLDEN_W, GOLDEN_H,
            VK_FORMAT_R16G16_SFLOAT);
        m_goldenDepth  = sv::DepthImage::create(m_vkCtx, GOLDEN_W, GOLDEN_H);
        printf("[golden] render-golden mode enabled, out=%s (%ux%u)\n",
               m_goldenOutDir.c_str(), GOLDEN_W, GOLDEN_H);
    }

    // ── bring up the replication client, if requested ──
    // This runs last so a failed connect() doesn't bring down the
    // rest of the harness. A connect failure logs and leaves
    // m_netConnected = false; the ImGui panel reports it.
    if (!m_netConnectTarget.empty()) {
        initNetworking();
    }

    printf("[TestEngine] Init complete\n");
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Replication client bring-up.
//
// Parses host:port, starts a client-role sv::net::Transport (with
// cfg.clientInsecureNoVerify = true because the server ships a
// self-signed loopback cert), issues Transport::connect, installs
// the datagram handler, and waits briefly for the handshake. If
// the handshake doesn't finish in time the harness still runs —
// the ImGui panel reports "disconnected".
void TestEngine::initNetworking()
{
    printf("[TestEngine] Network target: %s\n", m_netConnectTarget.c_str());

    // Split "host:port".
    const size_t colon = m_netConnectTarget.rfind(':');
    if (colon == std::string::npos) {
        fprintf(stderr, "[TestEngine] --connect needs host:port (got '%s')\n",
                m_netConnectTarget.c_str());
        return;
    }
    const std::string host = m_netConnectTarget.substr(0, colon);
    uint16_t port = 0;
    {
        const std::string portStr = m_netConnectTarget.substr(colon + 1);
        char* end = nullptr;
        unsigned long v = std::strtoul(portStr.c_str(), &end, 10);
        if (!end || *end != '\0' || v == 0 || v > 65535) {
            fprintf(stderr, "[TestEngine] --connect port invalid: '%s'\n",
                    portStr.c_str());
            return;
        }
        port = static_cast<uint16_t>(v);
    }

    if (!sv::net::Transport::isMsquicAvailable()) {
        fprintf(stderr, "[TestEngine] MsQuic runtime unavailable, "
                        "skipping --connect\n");
        return;
    }

    m_netTransport = std::make_unique<sv::net::Transport>();
    sv::net::Transport::Config cfg;
    cfg.alpn                      = "stratumv/1";
    cfg.idleTimeoutMs             = 60000;
    cfg.useSelfSignedLoopbackCert = false; // client role
    cfg.clientInsecureNoVerify    = true;  // accept the server's self-signed cert
    cfg.appName                   = "skinned_test";

    const auto startStatus = m_netTransport->start(cfg);
    if (startStatus != sv::net::TransportStatus::Ok) {
        fprintf(stderr, "[TestEngine] Transport::start failed: %s\n",
                sv::net::transportStatusToString(startStatus));
        m_netTransport.reset();
        return;
    }

    const auto connectStatus =
        m_netTransport->connect(host, port, m_netConn);
    if (connectStatus != sv::net::TransportStatus::Ok) {
        fprintf(stderr, "[TestEngine] connect(%s:%u) failed: %s\n",
                host.c_str(), static_cast<unsigned>(port),
                sv::net::transportStatusToString(connectStatus));
        m_netTransport->stop();
        m_netTransport.reset();
        return;
    }

    // Install the datagram handler BEFORE the handshake completes so
    // we don't race an early snapshot. The lambda captures `this`;
    // the TestEngine outlives the Transport because m_netTransport
    // is destroyed in onShutdown before TestEngine itself goes away.
    m_netConn.setDatagramHandler([this](const uint8_t* data, size_t size) {
        if (!data || size == 0) return;
        std::vector<uint8_t> copy(data, data + size);
        std::lock_guard<std::mutex> lk(m_netInboxMu);
        m_netInbox.push_back(std::move(copy));
        m_netInboundTotal.fetch_add(1, std::memory_order_relaxed);
    });

    // ── + reliable-stream dispatch ────────
    // The server emits three kinds of reliable messages on one
    // unidirectional QUIC stream each:
    //
    // kFrameSchemaHandshake: one-shot preamble
    //       compared against the local registry.
    // kFrameWelcome: per-connection identity — the
    //       server-assigned clientId, permission scope, and
    //       avatarEntityId.
    // kFrameEditTransaction: Spawn/Despawn/SetField/
    //       Undo/Redo. We queue these for main-thread processing
    //       to avoid touching the entity map under a worker
    //       thread.
    //
    // The handler dispatches on the first byte (msgType) and
    // routes accordingly. Unknown messages are logged and dropped.
    m_netConn.setReliableMessageHandler(
        [this](const uint8_t* data, size_t size) {
            if (!data || size == 0) return;
            const uint8_t msgType = data[0];

            if (msgType == sv::net::kFrameSchemaHandshake) {
                sv::net::SchemaHandshake parsed;
                if (!sv::net::parseSchemaHandshake(data, size, parsed)) {
                    std::lock_guard<std::mutex> lk(m_netSchemaMu);
                    m_netSchemaState  = NetSchemaState::Mismatch;
                    m_netSchemaDetail = "preamble parse failed";
                    printf("[TestEngine] schema preamble parse failed\n");
                    m_netConn.shutdown(sv::net::kErrSchemaMismatch);
                    return;
                }

                sv::net::SchemaCompareResult cmp =
                    sv::net::compareSchemaHandshake(parsed);

                {
                    std::lock_guard<std::mutex> lk(m_netSchemaMu);
                    m_netSchemaSemver    = parsed.semver;
                    m_netSchemaTypeCount = static_cast<uint16_t>(parsed.types.size());

                    char buf[256];
                    switch (cmp.status) {
                        case sv::net::SchemaCompareStatus::Ok:
                            m_netSchemaState  = NetSchemaState::Ok;
                            m_netSchemaDetail.clear();
                            printf("[TestEngine] schema handshake OK "
                                   "(server %u.%u.%u, %zu types)\n",
                                   sv::net::semverMajor(parsed.semver),
                                   sv::net::semverMinor(parsed.semver),
                                   sv::net::semverPatch(parsed.semver),
                                   parsed.types.size());
                            break;
                        case sv::net::SchemaCompareStatus::Mismatch:
                            m_netSchemaState = NetSchemaState::Mismatch;
                            std::snprintf(buf, sizeof(buf),
                                "%s expected 0x%04X got 0x%04X",
                                cmp.mismatchName.empty()
                                    ? "<unknown type>"
                                    : cmp.mismatchName.c_str(),
                                static_cast<unsigned>(cmp.expectedVer),
                                static_cast<unsigned>(cmp.receivedVer));
                            m_netSchemaDetail = buf;
                            printf("[TestEngine] schema mismatch: %s - closing connection\n",
                                   m_netSchemaDetail.c_str());
                            break;
                        case sv::net::SchemaCompareStatus::ServerHasUnknown:
                            m_netSchemaState = NetSchemaState::Unknown;
                            std::snprintf(buf, sizeof(buf),
                                "server has unknown type hash 0x%08X",
                                static_cast<unsigned>(cmp.mismatchHash));
                            m_netSchemaDetail = buf;
                            printf("[TestEngine] schema preamble has unknown type 0x%08X "
                                   "- continuing (soft)\n",
                                   static_cast<unsigned>(cmp.mismatchHash));
                            break;
                    }
                }

                if (cmp.status == sv::net::SchemaCompareStatus::Mismatch) {
                    m_netConn.shutdown(sv::net::kErrSchemaMismatch);
                }
                return;
            }

            if (msgType == sv::net::kFrameWelcome) {
                sv::net::WelcomeMessage welcome;
                if (!sv::net::parseWelcomeMessage(data, size, welcome)) {
                    printf("[TestEngine] welcome parse failed\n");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(m_netStateMu);
                    m_netClientId       = welcome.clientId;
                    m_netScope          = sv::permissionScopeFromByte(welcome.scope);
                    m_netAvatarEntityId = welcome.avatarEntityId;
                    m_netWelcomed       = true;
                }
                printf("[TestEngine] welcomed as client %u (avatar=%u scope=%s)\n",
                       static_cast<unsigned>(welcome.clientId),
                       static_cast<unsigned>(welcome.avatarEntityId),
                       sv::permissionScopeToString(
                           sv::permissionScopeFromByte(welcome.scope)));
                return;
            }

            if (msgType == sv::net::kFrameEditTransaction) {
                auto txOpt = sv::parseEditTransaction(data, size);
                if (!txOpt) {
                    printf("[TestEngine] edit transaction parse failed\n");
                    return;
                }
                std::lock_guard<std::mutex> lk(m_netReliableInboxMu);
                m_netReliableInbox.push_back(std::move(*txOpt));
                return;
            }

            // asset sync — announce / chunk / ack. The
            // worker thread only copies the raw bytes into the
            // asset inbox; parsing + state-machine dispatch runs on
            // the main thread under drainAssetInbox().
            if (msgType == sv::net::kFrameAssetAnnounce ||
                msgType == sv::net::kFrameAssetChunk    ||
                msgType == sv::net::kFrameAssetAck) {
                IncomingAssetMsg msg;
                msg.msgType = msgType;
                msg.bytes.assign(data, data + size);
                std::lock_guard<std::mutex> lk(m_assetInboxMu);
                m_assetInbox.push_back(std::move(msg));
                return;
            }

            printf("[TestEngine] unknown reliable msgType=%u\n",
                   static_cast<unsigned>(msgType));
        });

    // Block the main thread briefly for the handshake. Two seconds
    // is generous on loopback — a longer wait would only mask a bug.
    m_netConnected = m_netConn.waitForConnected(2000);
    if (!m_netConnected) {
        fprintf(stderr, "[TestEngine] handshake timed out connecting to %s:%u\n",
                host.c_str(), static_cast<unsigned>(port));
        return;
    }

    const auto stats = m_netConn.stats();
    printf("[TestEngine] connected to %s (alpn=%s)\n",
           stats.peerAddress.c_str(),
           stats.negotiatedAlpn.c_str());
}

// ──────────────────────────────────────────────────────────────────
// + drain the datagram inbox each frame and
// route each snapshot into the m_netEntities map keyed by entityId.
// Older ticks (within a single entity) are dropped via the
// monotonic-tick filter; entries for unknown entities are silently
// created in case the datagram arrives before the Spawn transaction
// (in practice the reliable stream delivers Spawn first, but
// belt-and-braces).
void TestEngine::drainNetInbox()
{
    if (!m_netConnected) return;

    std::vector<std::vector<uint8_t>> drained;
    {
        std::lock_guard<std::mutex> lk(m_netInboxMu);
        drained.swap(m_netInbox);
    }
    if (drained.empty()) return;

    for (const auto& bytes : drained) {
        m_netDatagramsReceived += 1;
        m_netBytesReceived     += bytes.size();

        auto frame = sv::net::parseSnapshotFrame(bytes.data(), bytes.size());
        if (!frame) { m_netFramesDropped += 1; continue; }

        ClientEntity& ent = m_netEntities[frame->entityId];
        if (ent.entityId == 0) {
            ent.entityId = frame->entityId;
            ent.alive    = true;
        }

        // Monotonic tick filter per entity.
        if (ent.haveData && frame->tickIndex <= ent.lastTick) {
            m_netFramesDropped += 1;
            continue;
        }

        sv::NetTransform decoded = ent.currentState;
        sv::DirtyMask    dummyMask;
        if (!sv::net::applySnapshotFrame(*frame, &decoded, dummyMask)) {
            m_netFramesDropped += 1;
            continue;
        }

        ent.prevState          = ent.currentState;
        ent.currentState       = decoded;
        ent.lastTick           = frame->tickIndex;
        ent.haveData           = true;
        ent.lastDecodeWallSec  = glfwGetTime();
        m_netFramesDecoded     += 1;

        // Keep the legacy single-cube mirror so the existing text
        // readout in drawNetworkDemoPanel still works. Entity 1 is
        // the server's orbiting cube.
        if (frame->entityId == 1) {
            m_netPrev              = ent.prevState;
            m_netCurrent           = ent.currentState;
            m_netLastTick          = frame->tickIndex;
            m_netHaveData          = true;
            m_netLastDecodeWallSec = ent.lastDecodeWallSec;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// drain inbound EditTransactions. The MsQuic worker thread
// pushes them into m_netReliableInbox via setReliableMessageHandler;
// the main thread applies them here at the top of every frame.
//
// Spawn transactions create or refresh an entry in m_netEntities.
// Despawn transactions remove it. SetField/Undo/Redo are logged
// but not acted on — the next datagram snapshot carries the new
// state, so the client doesn't need to maintain its own undo log.
void TestEngine::drainNetReliableInbox()
{
    std::vector<sv::EditTransaction> batch;
    {
        std::lock_guard<std::mutex> lk(m_netReliableInboxMu);
        batch.swap(m_netReliableInbox);
    }
    if (batch.empty()) return;

    for (sv::EditTransaction& tx : batch) {
        m_netReliableTxApplied += 1;
        switch (tx.kind) {
            case sv::EditKind::Spawn: {
                // spawn payload is now the generic
                // encodeSnapshot-backed format — 4 bytes owner
                // followed by a full-mask snapshot for the
                // component matching tx.typeNameHash. The server
                // always replays at the CURRENT state so late
                // joiners see every entity at its live position.
                sv::NetTransform initial;
                uint32_t owner = 0;
                sv::DirtyMask mask(sv::kNetTransformFieldCount);
                if (!sv::readGenericSpawnPayload(tx.typeNameHash,
                                                  tx.payload.data(),
                                                  tx.payload.size(),
                                                  owner,
                                                  &initial,
                                                  mask)) {
                    printf("[TestEngine] malformed Spawn payload for ent %u\n",
                           static_cast<unsigned>(tx.entityId));
                    break;
                }
                ClientEntity& ent = m_netEntities[tx.entityId];
                ent.entityId       = tx.entityId;
                ent.ownerClientId  = owner;
                ent.alive          = true;
                ent.prevState      = initial;
                ent.currentState   = initial;
                ent.haveData       = true;
                ent.lastDecodeWallSec = glfwGetTime();
                break;
            }
            case sv::EditKind::Despawn: {
                // notify any connected editor before
                // the local map loses the entity.
                if (m_editorBridge && m_editorBridge->running()) {
                    m_editorBridge->pushEntityGone(tx.entityId);
                }
                m_netEntities.erase(tx.entityId);
                break;
            }
            case sv::EditKind::SetField: {
                // ParentLink SetField transactions
                // carry the full state in the reliable-stream echo
                // (there is no datagram path for ParentLink). We
                // dispatch on typeNameHash; NetTransform SetFields
                // stay a no-op because the datagram path carries that
                // state already. extends the switch
                // with LightComponent, which also has no datagram
                // path — state flows only via this rebroadcast.
                const sv::ReplicationMeta* parentMeta =
                    sv::ReplicationRegistry::get().find("ParentLink");
                const sv::ReplicationMeta* lightMeta =
                    sv::ReplicationRegistry::get().find("LightComponent");
                const sv::ReplicationMeta* cameraMeta =
                    sv::ReplicationRegistry::get().find("CameraComponent");
                const sv::ReplicationMeta* materialMeta =
                    sv::ReplicationRegistry::get().find("MaterialComponent");
                if (parentMeta && tx.typeNameHash == parentMeta->typeNameHash) {
                    auto it = m_netEntities.find(tx.entityId);
                    if (it != m_netEntities.end()) {
                        sv::ParentLink newParent = it->second.parent;
                        sv::DirtyMask mask(parentMeta->fields.size());
                        if (sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                            tx.payload.data(),
                                                            tx.payload.size(),
                                                            &newParent,
                                                            mask)) {
                            it->second.parent = newParent;
                            ++m_bridgeParentChanges;
                        } else {
                            printf("[TestEngine] malformed ParentLink SetField for ent %u\n",
                                   static_cast<unsigned>(tx.entityId));
                        }
                    }
                } else if (lightMeta && tx.typeNameHash == lightMeta->typeNameHash) {
                    // LightComponent SetField.
                    // Start from the current light state so a
                    // partial-mask SetField preserves the untouched
                    // fields. Bump m_bridgeLightsApplied so the UI
                    // panel shows the inbound count even on the
                    // observer client (which never touches the bridge).
                    auto it = m_netEntities.find(tx.entityId);
                    if (it != m_netEntities.end()) {
                        sv::LightComponent newLight = it->second.light;
                        sv::DirtyMask mask(lightMeta->fields.size());
                        if (sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                            tx.payload.data(),
                                                            tx.payload.size(),
                                                            &newLight,
                                                            mask)) {
                            it->second.light = newLight;
                            ++m_bridgeLightsApplied;
                        } else {
                            printf("[TestEngine] malformed LightComponent SetField for ent %u\n",
                                   static_cast<unsigned>(tx.entityId));
                        }
                    }
                } else if (cameraMeta && tx.typeNameHash == cameraMeta->typeNameHash) {
                    // CameraComponent SetField.
                    // Mirror LightComponent's path exactly — start
                    // from the current sidecar so partial masks
                    // preserve untouched fields.
                    auto it = m_netEntities.find(tx.entityId);
                    if (it != m_netEntities.end()) {
                        sv::CameraComponent newCam = it->second.camera;
                        sv::DirtyMask mask(cameraMeta->fields.size());
                        if (sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                            tx.payload.data(),
                                                            tx.payload.size(),
                                                            &newCam,
                                                            mask)) {
                            it->second.camera = newCam;
                            ++m_bridgeCamerasApplied;
                        } else {
                            printf("[TestEngine] malformed CameraComponent SetField for ent %u\n",
                                   static_cast<unsigned>(tx.entityId));
                        }
                    }
                } else if (materialMeta && tx.typeNameHash == materialMeta->typeNameHash) {
                    // MaterialComponent SetField.
                    auto it = m_netEntities.find(tx.entityId);
                    if (it != m_netEntities.end()) {
                        sv::MaterialComponent newMat = it->second.material;
                        sv::DirtyMask mask(materialMeta->fields.size());
                        if (sv::readGenericSetFieldPayload(tx.typeNameHash,
                                                            tx.payload.data(),
                                                            tx.payload.size(),
                                                            &newMat,
                                                            mask)) {
                            it->second.material = newMat;
                            ++m_bridgeMaterialsApplied;
                        } else {
                            printf("[TestEngine] malformed MaterialComponent SetField for ent %u\n",
                                   static_cast<unsigned>(tx.entityId));
                        }
                    }
                }
                break;
            }
            case sv::EditKind::Undo:
            case sv::EditKind::Redo:
                // State comes via the datagram path; the transaction
                // echo is informational for a future wire-log UI.
                break;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// issue a SetField transaction moving the local avatar by
// the given delta. The current state comes from the last decoded
// snapshot for the avatar entity, so clicks produce incremental
// moves rather than teleports.
void TestEngine::sendAvatarMove(float dx, float dy, float dz)
{
    if (!m_netConnected)         return;
    if (!m_netConn.valid())      return;
    uint32_t avatarId = 0;
    sv::PermissionScope scope;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
        avatarId = m_netAvatarEntityId;
        scope    = m_netScope;
    }
    if (avatarId == 0) return;

    auto it = m_netEntities.find(avatarId);
    if (it == m_netEntities.end()) {
        printf("[TestEngine] sendAvatarMove: no local state for ent %u\n",
               static_cast<unsigned>(avatarId));
        return;
    }
    sv::NetTransform after = it->second.currentState;
    after.posX += dx;
    after.posY += dy;
    after.posZ += dz;

    // encode via the generic SetField path so the wire
    // format matches whatever encodeSnapshot emits for the
    // NetTransform meta. Sends a full-mask snapshot so every field
    // lands on the server; future sessions may use a partial mask
    // to only push the three position deltas.
    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    if (!meta) return;

    sv::EditTransaction tx;
    tx.kind          = sv::EditKind::SetField;
    tx.requiredScope = sv::PermissionScope::Editor;
    tx.entityId      = avatarId;
    tx.typeNameHash  = meta->typeNameHash;
    sv::DirtyMask fullMask(meta->fields.size());
    fullMask.setAll();
    if (!sv::writeGenericSetFieldPayload(*meta, &after, fullMask, tx.payload)) {
        printf("[TestEngine] writeGenericSetFieldPayload failed for ent %u\n",
               static_cast<unsigned>(avatarId));
        return;
    }

    std::vector<uint8_t> bytes;
    if (!sv::encodeEditTransaction(tx, bytes)) return;
    if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
        m_netSetFieldSent += 1;
    }
    (void)scope;
}

void TestEngine::sendUndoRequest()
{
    if (!m_netConnected)    return;
    if (!m_netConn.valid()) return;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
    }
    sv::EditTransaction tx;
    tx.kind          = sv::EditKind::Undo;
    tx.requiredScope = sv::PermissionScope::Editor;
    // No payload for client-initiated Undo; the server walks its
    // own log to find the target.
    std::vector<uint8_t> bytes;
    if (!sv::encodeEditTransaction(tx, bytes)) return;
    if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
        m_netUndoSent += 1;
    }
}

void TestEngine::sendRedoRequest()
{
    if (!m_netConnected)    return;
    if (!m_netConn.valid()) return;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
    }
    sv::EditTransaction tx;
    tx.kind          = sv::EditKind::Redo;
    tx.requiredScope = sv::PermissionScope::Editor;
    std::vector<uint8_t> bytes;
    if (!sv::encodeEditTransaction(tx, bytes)) return;
    if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
        m_netRedoSent += 1;
    }
}

// ══════════════════════════════════════════════════════════════════
// editor-bridge helpers
// ══════════════════════════════════════════════════════════════════
//
// The bridge lives alongside the QUIC replication client. Once the
// harness has received its Welcome message we spin up the TCP
// listener with the Hello snapshot already populated, push every
// currently-known entity, and then feed further updates in through
// publishEntityToBridge() as snapshots or edit transactions arrive.
//
// Outbound: pushEntityState runs whenever an entity changes on the
// main thread (drainNetInbox / drainNetReliableInbox callers).
//
// Inbound: pumpBridgeMoves drains MoveSelf frames from any connected
// bridge client and turns them into the same full-mask SetField
// EditTransaction that sendAvatarMove builds for the in-engine UI.
// From the server's POV the edit looks like any other Editor-scope
// transaction from this skinned_test instance.
void TestEngine::maybeStartEditorBridge()
{
    if (m_editorBridgePort == 0)  return;
    if (m_editorBridgeStarted)    return;

    bool ready = false;
    uint32_t clientId = 0;
    uint32_t avatarId = 0;
    uint8_t  scopeRaw = 0;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (m_netWelcomed) {
            ready    = true;
            clientId = m_netClientId;
            avatarId = m_netAvatarEntityId;
            scopeRaw = static_cast<uint8_t>(m_netScope);
        }
    }
    if (!ready) return;

    // Look up the NetTransform schema version so Blender clients can
    // sanity-check their own wire codec against whatever the server
    // registry produced at startup.
    uint16_t schemaVer = 0;
    if (const sv::ReplicationMeta* meta =
            sv::ReplicationRegistry::get().find("NetTransform")) {
        schemaVer = meta->schemaVersion;
    }

    m_editorBridge = std::make_unique<sv::net::EditorBridge>();
    if (!m_editorBridge->start(m_editorBridgePort)) {
        printf("[TestEngine] EditorBridge::start failed on port %u\n",
               static_cast<unsigned>(m_editorBridgePort));
        m_editorBridge.reset();
        return;
    }

    const uint32_t semver = sv::net::packSemver(
        STRATUMV_VERSION_MAJOR,
        STRATUMV_VERSION_MINOR,
        STRATUMV_VERSION_PATCH);

    m_editorBridge->setHello(
        clientId,
        avatarId,
        schemaVer,
        scopeRaw,
        semver,
        sv::net::kBridgeServerWelcomed,
        "stratumv skinned_test");

    // Walk every currently-known entity and push it to the bridge
    // so a late-connecting Blender sees the same world the engine
    // sees, not a blank slate.
    for (const auto& kv : m_netEntities) {
        publishEntityToBridge(kv.first);
    }

    m_editorBridgeStarted = true;
    printf("[TestEngine] EditorBridge up on 127.0.0.1:%u (client=%u avatar=%u)\n",
           static_cast<unsigned>(m_editorBridgePort),
           static_cast<unsigned>(clientId),
           static_cast<unsigned>(avatarId));
}

void TestEngine::publishEntityToBridge(uint32_t entityId)
{
    if (!m_editorBridge || !m_editorBridge->running()) return;
    auto it = m_netEntities.find(entityId);
    if (it == m_netEntities.end()) return;
    const ClientEntity& ent = it->second;

    uint32_t localAvatarId = 0;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        localAvatarId = m_netAvatarEntityId;
    }

    sv::net::EditorBridgeEntityState st;
    st.entityId      = ent.entityId;
    st.ownerClientId = ent.ownerClientId;
    st.isSelf        = (ent.entityId == localAvatarId);
    // Authority: 0 (Server) for the cube / server-owned entities,
    // 1 (Owner) for per-client avatars. We derive the byte from
    // ownerClientId because the lab harness does not track authority
    // explicitly — server-owned entities always have owner 0.
    st.authority     = (ent.ownerClientId == 0)
                        ? static_cast<uint8_t>(sv::Authority::Server)
                        : static_cast<uint8_t>(sv::Authority::Owner);
    st.transform     = ent.currentState;

    char labelBuf[64] = {0};
    if (ent.entityId == 1) {
        std::snprintf(labelBuf, sizeof(labelBuf), "Cube");
    } else if (ent.ownerClientId == 0) {
        std::snprintf(labelBuf, sizeof(labelBuf),
                      "Entity %u", static_cast<unsigned>(ent.entityId));
    } else {
        std::snprintf(labelBuf, sizeof(labelBuf),
                      "Client %u avatar",
                      static_cast<unsigned>(ent.ownerClientId));
    }
    st.label = labelBuf;

    m_editorBridge->pushEntityState(st);
    ++m_bridgeStatePushed;
}

void TestEngine::pumpBridgeMoves()
{
    if (!m_editorBridge || !m_editorBridge->running()) return;
    auto moves = m_editorBridge->drainMoves();
    if (moves.empty()) return;

    // Gate once up-front: the bridge move path MUST go through a
    // real welcomed QUIC connection with Editor scope, same as the
    // in-engine "Move +X" button.
    if (!m_netConnected || !m_netConn.valid()) return;

    uint32_t avatarId = 0;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
        avatarId = m_netAvatarEntityId;
    }
    if (avatarId == 0) return;

    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("NetTransform");
    if (!meta) return;

    for (const auto& move : moves) {
        sv::EditTransaction tx;
        tx.kind          = sv::EditKind::SetField;
        tx.requiredScope = sv::PermissionScope::Editor;
        tx.entityId      = avatarId;
        tx.typeNameHash  = meta->typeNameHash;

        sv::DirtyMask fullMask(meta->fields.size());
        fullMask.setAll();
        if (!sv::writeGenericSetFieldPayload(
                *meta, &move.target, fullMask, tx.payload)) {
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!sv::encodeEditTransaction(tx, bytes)) continue;
        if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
            ++m_bridgeMoveApplied;
            ++m_netSetFieldSent;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// drain completed Blender-pushed assets and send
// each through the same upload path as the in-engine button.
// Runs on the main thread once per frame after maybeStartEditorBridge.
void TestEngine::pumpBridgeAssets()
{
    if (!m_editorBridge || !m_editorBridge->running()) return;
    auto assets = m_editorBridge->drainAssets();
    if (assets.empty()) return;

    for (auto& asset : assets) {
        ++m_bridgeAssetsReceived;
        const sv::AssetKind kind =
            static_cast<sv::AssetKind>(asset.assetKind);
        if (uploadAssetBytes(asset.name, kind,
                             asset.bytes.data(), asset.bytes.size())) {
            ++m_bridgeAssetsUploaded;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// drain parent-change requests and turn each into a
// ParentLink SetField transaction targeting the bridge's own avatar.
// Same scope gate as pumpBridgeMoves — connected + welcomed + Editor.
void TestEngine::pumpBridgeParents()
{
    if (!m_editorBridge || !m_editorBridge->running()) return;
    auto changes = m_editorBridge->drainParentChanges();
    if (changes.empty()) return;

    if (!m_netConnected || !m_netConn.valid()) return;

    uint32_t avatarId = 0;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
        avatarId = m_netAvatarEntityId;
    }
    if (avatarId == 0) return;

    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("ParentLink");
    if (!meta) return;

    for (const auto& pc : changes) {
        sv::ParentLink link;
        link.parentEntityId = pc.parentEntityId;

        sv::EditTransaction tx;
        tx.kind          = sv::EditKind::SetField;
        tx.requiredScope = sv::PermissionScope::Editor;
        tx.entityId      = avatarId;
        tx.typeNameHash  = meta->typeNameHash;

        sv::DirtyMask fullMask(meta->fields.size());
        fullMask.setAll();
        if (!sv::writeGenericSetFieldPayload(
                *meta, &link, fullMask, tx.payload)) {
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!sv::encodeEditTransaction(tx, bytes)) continue;
        if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
            ++m_netSetFieldSent;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// drain SetLight requests from the bridge and turn
// each into a full-mask LightComponent SetField EditTransaction
// targeting the bridge's own avatar. Mirrors pumpBridgeParents exactly
// — same scope gate (connected + welcomed + Editor), same "applies to
// the bridge's avatar, not an explicit entityId" convention.
void TestEngine::pumpBridgeLights()
{
    if (!m_editorBridge || !m_editorBridge->running()) return;
    auto lights = m_editorBridge->drainLights();
    if (lights.empty()) return;

    if (!m_netConnected || !m_netConn.valid()) return;

    uint32_t avatarId = 0;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
        avatarId = m_netAvatarEntityId;
    }
    if (avatarId == 0) return;

    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("LightComponent");
    if (!meta) return;

    for (const auto& ls : lights) {
        sv::EditTransaction tx;
        tx.kind          = sv::EditKind::SetField;
        tx.requiredScope = sv::PermissionScope::Editor;
        tx.entityId      = avatarId;
        tx.typeNameHash  = meta->typeNameHash;

        sv::DirtyMask fullMask(meta->fields.size());
        fullMask.setAll();
        if (!sv::writeGenericSetFieldPayload(
                *meta, &ls.light, fullMask, tx.payload)) {
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!sv::encodeEditTransaction(tx, bytes)) continue;
        if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
            ++m_netSetFieldSent;
            ++m_bridgeLightsSent;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// drain SetCamera requests from the bridge and turn
// each into a full-mask CameraComponent SetField EditTransaction
// targeting the bridge's own avatar. Mirrors pumpBridgeLights exactly.
void TestEngine::pumpBridgeCameras()
{
    if (!m_editorBridge || !m_editorBridge->running()) return;
    auto cameras = m_editorBridge->drainCameras();
    if (cameras.empty()) return;

    if (!m_netConnected || !m_netConn.valid()) return;

    uint32_t avatarId = 0;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
        avatarId = m_netAvatarEntityId;
    }
    if (avatarId == 0) return;

    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("CameraComponent");
    if (!meta) return;

    for (const auto& cs : cameras) {
        sv::EditTransaction tx;
        tx.kind          = sv::EditKind::SetField;
        tx.requiredScope = sv::PermissionScope::Editor;
        tx.entityId      = avatarId;
        tx.typeNameHash  = meta->typeNameHash;

        sv::DirtyMask fullMask(meta->fields.size());
        fullMask.setAll();
        if (!sv::writeGenericSetFieldPayload(
                *meta, &cs.camera, fullMask, tx.payload)) {
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!sv::encodeEditTransaction(tx, bytes)) continue;
        if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
            ++m_netSetFieldSent;
            ++m_bridgeCamerasSent;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// drain SetMaterial requests from the bridge.
void TestEngine::pumpBridgeMaterials()
{
    if (!m_editorBridge || !m_editorBridge->running()) return;
    auto materials = m_editorBridge->drainMaterials();
    if (materials.empty()) return;

    if (!m_netConnected || !m_netConn.valid()) return;

    uint32_t avatarId = 0;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) return;
        if (m_netScope < sv::PermissionScope::Editor) return;
        avatarId = m_netAvatarEntityId;
    }
    if (avatarId == 0) return;

    const sv::ReplicationMeta* meta =
        sv::ReplicationRegistry::get().find("MaterialComponent");
    if (!meta) return;

    for (const auto& ms : materials) {
        sv::EditTransaction tx;
        tx.kind          = sv::EditKind::SetField;
        tx.requiredScope = sv::PermissionScope::Editor;
        tx.entityId      = avatarId;
        tx.typeNameHash  = meta->typeNameHash;

        sv::DirtyMask fullMask(meta->fields.size());
        fullMask.setAll();
        if (!sv::writeGenericSetFieldPayload(
                *meta, &ms.material, fullMask, tx.payload)) {
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!sv::encodeEditTransaction(tx, bytes)) continue;
        if (m_netConn.sendReliableMessage(bytes.data(), bytes.size())) {
            ++m_netSetFieldSent;
            ++m_bridgeMaterialsSent;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// upload a local asset file to the server via announce +
// chunk stream. Hashes the file bytes, builds an AssetAnnounceMessage
// + AssetChunkMessage sequence via AssetUploadClient, and pumps each
// wire buffer through the reliable stream. The server replies with
// an Ack on the same stream; the main-thread drainAssetInbox handles
// the have/need branch.
bool TestEngine::uploadAssetFromDisk(const std::string& relPath,
                                     const std::string& absPath)
{
    // Read file bytes. Small synchronous read — fine for textures
    // at the demo scale. Future sessions can swap in a
    // streamed reader when assets grow.
    std::vector<uint8_t> bytes;
    {
        std::ifstream f(absPath, std::ios::binary | std::ios::ate);
        if (!f.good()) {
            m_lastUploadStatus = "upload: open failed: " + absPath;
            return false;
        }
        const auto sz = f.tellg();
        if (sz < 0) {
            m_lastUploadStatus = "upload: tellg failed";
            return false;
        }
        bytes.resize(static_cast<size_t>(sz));
        f.seekg(0, std::ios::beg);
        if (!f.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
            m_lastUploadStatus = "upload: read failed";
            return false;
        }
    }
    const sv::AssetKind kind = sv::kindFromFilename(relPath);
    return uploadAssetBytes(relPath, kind, bytes.data(), bytes.size());
}

// ──────────────────────────────────────────────────────────────────
// core asset upload path shared between the file-
// backed button and the bridge-forwarded Blender push. Takes
// already-in-memory bytes + a kind + a wire-visible relPath. Runs the
// same Announce + Chunks + local CAS pin sequence uploadAssetFromDisk
// used to do before the refactor.
bool TestEngine::uploadAssetBytes(const std::string& relPath,
                                  sv::AssetKind      kind,
                                  const uint8_t*     data,
                                  size_t             size)
{
    if (!m_netConnected || !m_netConn.valid()) {
        m_lastUploadStatus = "upload: not connected";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        if (!m_netWelcomed) {
            m_lastUploadStatus = "upload: not welcomed";
            return false;
        }
        if (m_netScope < sv::PermissionScope::Editor) {
            m_lastUploadStatus = "upload: scope below Editor";
            return false;
        }
    }
    if (size > sv::net::kAssetByteLimit) {
        m_lastUploadStatus = "upload: size above 64 MiB limit";
        return false;
    }
    if (data == nullptr && size != 0) {
        m_lastUploadStatus = "upload: null bytes";
        return false;
    }

    const sv::AssetHash hash = sv::sha256(data, size);
    const std::string   hex  = sv::digestToHex(hash);

    sv::AssetUploadRequest req;
    req.hash      = hash;
    req.byteSize  = static_cast<uint32_t>(size);
    req.assetKind = static_cast<uint8_t>(kind);
    req.name      = relPath;
    req.bytes     = data;
    req.chunkSize = sv::net::kAssetChunkSize;

    std::vector<uint8_t> announceBytes;
    if (!sv::buildAssetAnnounce(req, announceBytes)) {
        m_lastUploadStatus = "upload: buildAssetAnnounce failed";
        return false;
    }
    if (!m_netConn.sendReliableMessage(announceBytes.data(), announceBytes.size())) {
        m_lastUploadStatus = "upload: Announce send failed";
        return false;
    }

    // Start tracking on the client side so we can resolve the Ack
    // response when it arrives. The receiver is used as the
    // "pending upload" marker — its `assembled` bytes stay empty
    // since we never deposit chunks locally; we keep it purely so
    // drainAssetInbox can look up metadata when the server acks
    // HaveIt (for the "Uploaded (dedup)" status line).
    auto& pending = m_assetReceivers[hex];
    pending.beginFromAnnounce(
        hash, req.byteSize, req.assetKind, relPath,
        sv::assetChunkCount(req.byteSize, req.chunkSize), req.chunkSize);

    // Eagerly ship chunks. The server will send Ack(HaveIt) as a
    // dedup hint after it sees the Announce; in that case the
    // chunks we just sent get dropped server-side. Streaming
    // optimistically keeps the code path simple — a future
    // session can add the "wait for Ack" mode for large assets.
    std::vector<std::vector<uint8_t>> chunks;
    if (!sv::buildAssetChunks(req, chunks)) {
        m_lastUploadStatus = "upload: buildAssetChunks failed";
        m_assetReceivers.erase(hex);
        return false;
    }
    for (const auto& c : chunks) {
        if (!m_netConn.sendReliableMessage(c.data(), c.size())) {
            m_lastUploadStatus = "upload: Chunk send failed mid-stream";
            m_assetReceivers.erase(hex);
            return false;
        }
        ++m_assetChunksSent;
    }

    // Pin the uploaded bytes in the local cache immediately — a
    // client that uploads an asset conceptually already has it.
    const auto saveStatus = m_assetStore.save(
        hash, req.assetKind, relPath, data, size);
    if (saveStatus == sv::AssetPersistenceStatus::Ok) {
        ReceivedAssetRow row;
        row.name            = relPath;
        row.hashPrefix      = hex.substr(0, 12);
        row.byteSize        = req.byteSize;
        row.assetKind       = req.assetKind;
        row.uploadedLocally = true;
        m_receivedAssetRows.push_back(row);
    }
    ++m_assetUploadsSent;

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "uploaded %s (%u bytes, hash %s...)",
                  relPath.c_str(),
                  static_cast<unsigned>(req.byteSize),
                  hex.substr(0, 12).c_str());
    m_lastUploadStatus = buf;
    return true;
}

// ──────────────────────────────────────────────────────────────────
// drain the asset inbox and drive the receive state machine.
// Announce: kick off a new AssetReceiver keyed by hex hash. Chunk:
// deposit into the matching receiver; on complete, verify + save.
// Ack: client side only sees this as metadata about our own uploads.
void TestEngine::drainAssetInbox()
{
    std::vector<IncomingAssetMsg> batch;
    {
        std::lock_guard<std::mutex> lk(m_assetInboxMu);
        batch.swap(m_assetInbox);
    }
    for (const auto& msg : batch) {
        if (msg.msgType == sv::net::kFrameAssetAnnounce) {
            sv::net::AssetAnnounceMessage ann;
            if (!sv::net::parseAssetAnnounce(msg.bytes.data(), msg.bytes.size(), ann)) {
                printf("[TestEngine] AssetAnnounce parse failed\n");
                continue;
            }
            const std::string hex = sv::digestToHex(ann.hash);
            // Dedup: drop the broadcast on the floor if we already
            // have it cached locally. Still bump the "received"
            // counter so the UI reflects the traffic.
            if (m_assetStore.find(ann.hash)) {
                ++m_assetsReceived;
                continue;
            }
            auto& rx = m_assetReceivers[hex];
            rx.beginFromAnnounce(
                ann.hash, ann.byteSize, ann.assetKind, ann.name,
                sv::assetChunkCount(ann.byteSize, sv::net::kAssetChunkSize),
                sv::net::kAssetChunkSize);
            continue;
        }
        if (msg.msgType == sv::net::kFrameAssetChunk) {
            sv::net::AssetChunkMessage c;
            if (!sv::net::parseAssetChunk(msg.bytes.data(), msg.bytes.size(), c)) {
                printf("[TestEngine] AssetChunk parse failed\n");
                continue;
            }
            const std::string hex = sv::digestToHex(c.hash);
            auto it = m_assetReceivers.find(hex);
            if (it == m_assetReceivers.end()) {
                // Silent drop — either the Announce was suppressed
                // (we already had the bytes cached) or we missed the
                // Announce for some reason. Either way the chunk is
                // harmless to ignore.
                continue;
            }
            sv::AssetReceiver& rx = it->second;
            // An uploading client's own receiver has empty assembled
            // buffer — skip the deposit in that case (this client is
            // the sender).
            if (rx.assembled.empty() && rx.byteSize > 0) {
                // This is the sender's own pending upload. Drop the
                // inbound chunk (we never receive our own chunks).
                continue;
            }
            if (!rx.depositChunk(c.chunkIndex, c.chunk, c.chunkLen)) {
                printf("[TestEngine] depositChunk %u/%u refused for %s...\n",
                       static_cast<unsigned>(c.chunkIndex),
                       static_cast<unsigned>(c.chunkCount),
                       hex.substr(0, 12).c_str());
                m_assetReceivers.erase(it);
                continue;
            }
            if (rx.complete) {
                if (!rx.verifyHash()) {
                    printf("[TestEngine] inbound asset hash mismatch for %s...\n",
                           hex.substr(0, 12).c_str());
                    m_assetReceivers.erase(it);
                    continue;
                }
                const auto saveStatus = m_assetStore.save(
                    rx.hash, rx.assetKind, rx.name,
                    rx.assembled.data(), rx.assembled.size());
                if (saveStatus != sv::AssetPersistenceStatus::Ok) {
                    printf("[TestEngine] asset store save failed: %s\n",
                           sv::assetPersistenceStatusToString(saveStatus));
                    m_assetReceivers.erase(it);
                    continue;
                }
                ReceivedAssetRow row;
                row.name       = rx.name;
                row.hashPrefix = hex.substr(0, 12);
                row.byteSize   = rx.byteSize;
                row.assetKind  = rx.assetKind;
                row.uploadedLocally = false;
                m_receivedAssetRows.push_back(row);
                ++m_assetsReceived;
                printf("[TestEngine] received asset '%s' (%u bytes, hash %s...)\n",
                       rx.name.c_str(),
                       static_cast<unsigned>(rx.byteSize),
                       hex.substr(0, 12).c_str());
                m_assetReceivers.erase(it);
            }
            continue;
        }
        if (msg.msgType == sv::net::kFrameAssetAck) {
            sv::net::AssetAckMessage ack;
            if (!sv::net::parseAssetAck(msg.bytes.data(), msg.bytes.size(), ack)) {
                continue;
            }
            if (ack.status == sv::net::AssetAckStatus::HaveIt) {
                ++m_assetDedupHits;
                const std::string hex = sv::digestToHex(ack.hash);
                // Pop the matching pending upload — the server
                // already has the bytes, so we don't have to
                // re-track the upload on the client side.
                m_assetReceivers.erase(hex);
            }
            continue;
        }
    }
}

// ──────────────────────────────────────────────────────────────────
// Replicated Assets panel — lists assets the local store
// knows about, flagging which were uploaded locally vs received
// from the server. Shows per-row hash prefix + byte size + kind.
void TestEngine::drawReplicatedAssetsPanel()
{
    ImGui::SetNextWindowPos(ImVec2(870, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("Replicated Assets");

    ImGui::Text("Local CAS: %zu assets", m_assetStore.size());
    ImGui::Text("Uploaded: %llu   Dedup hits: %llu",
                static_cast<unsigned long long>(m_assetUploadsSent),
                static_cast<unsigned long long>(m_assetDedupHits));
    ImGui::Text("Received: %llu   Chunks sent: %llu",
                static_cast<unsigned long long>(m_assetsReceived),
                static_cast<unsigned long long>(m_assetChunksSent));
    ImGui::Separator();

    if (m_receivedAssetRows.empty()) {
        ImGui::TextDisabled("no replicated assets yet — click Upload");
    } else {
        if (ImGui::BeginChild("ReplicatedAssetList", ImVec2(0, 210),
                              ImGuiChildFlags_Borders)) {
            for (const auto& row : m_receivedAssetRows) {
                ImVec4 col = row.uploadedLocally
                    ? ImVec4(0.7f, 0.9f, 0.4f, 1.0f)   // lime-ish for local
                    : ImVec4(0.4f, 0.8f, 1.0f, 1.0f);  // cyan for received
                ImGui::TextColored(col, "[%s] %s",
                                   sv::assetKindToString(
                                       static_cast<sv::AssetKind>(row.assetKind)),
                                   row.name.c_str());
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                   "(%u B, %s...)",
                                   row.byteSize,
                                   row.hashPrefix.c_str());
                if (row.uploadedLocally) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
                                       "[uploaded]");
                } else {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                                       "[from server]");
                }
            }
        }
        ImGui::EndChild();
    }

    if (!m_lastUploadStatus.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_lastUploadStatus.c_str());
    }

    ImGui::End();
}

// ──────────────────────────────────────────────────────────────────
void TestEngine::initFrameSync()
{
    VkDevice device = m_vkCtx.device();

    VkCommandBufferAllocateInfo cmdAI{};
    cmdAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAI.commandPool        = m_vkCtx.commandPool();
    cmdAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAI.commandBufferCount = 1;

    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        vkAllocateCommandBuffers(device, &cmdAI, &m_frames[i].cmd);
        vkCreateSemaphore(device, &semCI, nullptr, &m_frames[i].imageAvailable);
        vkCreateFence(device, &fenceCI, nullptr, &m_frames[i].inFlight);
    }

    // One renderFinished semaphore per swapchain image: present
    // consumes the semaphore keyed by imageIndex, so signaling must
    // be keyed the same way or an in-flight signal can collide
    // (VUID-vkQueueSubmit-pSignalSemaphores-00067).
    m_renderFinishedPerImage.resize(m_swapchain.imageCount(), VK_NULL_HANDLE);
    for (auto& sem : m_renderFinishedPerImage)
        vkCreateSemaphore(device, &semCI, nullptr, &sem);
}

// ──────────────────────────────────────────────────────────────────
void TestEngine::initDescriptors()
{
    VkDevice     device = m_vkCtx.device();
    VmaAllocator alloc  = m_vkCtx.allocator();

    // UBO buffers
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        m_uboBuffers[i] = sv::VkBuf::create(alloc, sizeof(SceneUBO),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    // Descriptor set layout: binding 0 = SceneUBO
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_sceneDescLayout);

    // Pool + allocate 2 sets
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = MAX_FRAMES;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = MAX_FRAMES;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    vkCreateDescriptorPool(device, &poolCI, nullptr, &m_descPool);

    VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_sceneDescLayout, m_sceneDescLayout };
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = MAX_FRAMES;
    allocInfo.pSetLayouts        = layouts;
    vkAllocateDescriptorSets(device, &allocInfo, m_descSets);

    // Write UBO descriptors
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_uboBuffers[i].buffer;
        bufInfo.offset = 0;
        bufInfo.range  = sizeof(SceneUBO);

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_descSets[i];
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

// ──────────────────────────────────────────────────────────────────
void TestEngine::updateSceneUBO(float /*dt*/)
{
    float aspect = m_window.aspect();
    glm::mat4 view = m_camera.viewMatrix();
    glm::mat4 proj = m_camera.projMatrix(aspect);

    // ── per-entity CameraComponent override ─────
    // Walk m_netEntities once and pick the FIRST entity with a
    // non-default CameraComponent (sorted by entityId for determinism
    // across clients). When found, replace the local proj matrix with
    // a custom perspective so the local viewport reflects the editor
    // camera the bridge pushed. View matrix stays the local
    // FreeFly/Follow camera — only the lens changes — so the visual
    // checkpoint shows a clear FOV difference between "no override"
    // and "override active" without needing a real camera handover.
    {
        const sv::CameraComponent* activeCam = nullptr;
        uint32_t activeCamEntity = 0;
        for (const auto& [eid, ent] : m_netEntities) {
            if (!ent.alive) continue;
            const auto& c = ent.camera;
            if (c.fovDeg <= 0.0f) continue;
            if (c.farPlane <= c.nearPlane) continue;
            if (activeCam == nullptr || eid < activeCamEntity) {
                activeCam = &c;
                activeCamEntity = eid;
            }
        }
        if (activeCam != nullptr) {
            const float effectiveAspect =
                (activeCam->aspect > 0.0f) ? activeCam->aspect : aspect;
            // Plain glm::perspective with no proj[1][1] flip — the
            // main render pass already uses a negative-height
            // viewport for the Vulkan Y-flip, so a flip here would
            // double-flip and render the world upside-down. Match
            // the engine's Camera::projMatrix() behaviour exactly.
            proj = glm::perspective(
                glm::radians(activeCam->fovDeg),
                effectiveAspect,
                activeCam->nearPlane,
                activeCam->farPlane);
        }
    }

    glm::mat4 vp   = proj * view;

    m_sceneUBO.viewProj              = vp;
    m_sceneUBO.invViewProj           = glm::inverse(vp);
    m_sceneUBO.invViewProjUnjittered = m_sceneUBO.invViewProj;
    m_sceneUBO.cameraPos             = glm::vec4(m_camera.position(), 0.0f);

    // Sun: from upper-right, warm white — moderate intensity for SRGB output
    glm::vec3 sunDir = glm::normalize(glm::vec3(0.4f, 0.6f, -0.3f));
    m_sceneUBO.sunDirection = glm::vec4(sunDir, sunDir.y);
    m_sceneUBO.sunColor     = glm::vec4(1.0f, 0.95f, 0.85f, 1.5f);
    m_sceneUBO.ambientColor = glm::vec4(0.3f, 0.32f, 0.38f, 0.0f);

    // ── pack replicated lights into the UBO tail ─
    // Walk m_netEntities and collect every entity whose LightComponent
    // sidecar is actually active (type != 0 AND intensity > 0). Cap at
    // kMaxLights — further entries silently drop; the Network Demo
    // panel shows the total count so truncation is visible.
    uint32_t activeLights = 0;
    for (const auto& [eid, ent] : m_netEntities) {
        if (!ent.alive) continue;
        if (ent.light.type == 0) continue;
        if (ent.light.intensity <= 0.0f) continue;
        if (activeLights >= sv_ubo::kMaxLights) break;

        SceneLight& slot = m_sceneUBO.lights[activeLights];
        slot.positionType = glm::vec4(ent.currentState.posX,
                                       ent.currentState.posY,
                                       ent.currentState.posZ,
                                       static_cast<float>(ent.light.type));

        // Forward vector from the entity's quaternion. For a unit
        // quaternion (x,y,z,w), the local forward (-Z) rotated into
        // world space is:
        //   (2(xz + wy),  2(yz - wx), 1 - 2(x^2 + y^2))
        // The lab harness stores rotations as full (x,y,z,w) floats.
        const float qx = ent.currentState.rotX;
        const float qy = ent.currentState.rotY;
        const float qz = ent.currentState.rotZ;
        const float qw = ent.currentState.rotW;
        glm::vec3 forward(
            2.0f * (qx * qz + qw * qy),
            2.0f * (qy * qz - qw * qx),
            1.0f - 2.0f * (qx * qx + qy * qy));
        // Guard against a malformed zero-length quaternion.
        const float fwdLen = glm::length(forward);
        if (fwdLen > 1e-5f) {
            forward /= fwdLen;
        } else {
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
        }
        slot.directionRange = glm::vec4(forward, ent.light.range);

        slot.colorIntensity = glm::vec4(ent.light.colorR,
                                         ent.light.colorG,
                                         ent.light.colorB,
                                         ent.light.intensity);

        // Cone angles are stored in degrees on the wire; the shader
        // wants cosines to avoid trig in the inner loop. Convert
        // here so every frame's shader workload is identical
        // regardless of light count.
        const float deg2rad = 3.14159265358979323846f / 180.0f;
        const float cosInner = std::cos(ent.light.coneInnerDeg * deg2rad);
        const float cosOuter = std::cos(ent.light.coneOuterDeg * deg2rad);
        slot.coneParams = glm::vec4(cosInner, cosOuter, 1.0f, 0.0f);

        ++activeLights;
    }
    // Zero out unused slots so a shrink from N lights to N-1 does not
    // leave stale data bleeding into the shader loop (the shader
    // reads all 8 entries but early-exits on coneParams.z < 0.5).
    for (uint32_t i = activeLights; i < sv_ubo::kMaxLights; ++i) {
        m_sceneUBO.lights[i] = SceneLight{};
    }
    m_sceneUBO.lightCount = glm::uvec4(activeLights, 0, 0, 0);

    // ── pack the demo material override ──────────
    // Walk m_netEntities and pick the FIRST entity (sorted by entityId
    // for cross-client determinism) whose MaterialComponent has a
    // non-zero override strength. Pack its (rgb, strength) into the
    // single SceneUBO::materialOverride slot. The fragment shaders
    // multiply this into the per-mesh basecolor at the very end of
    // the lighting pipeline; strength = 0 leaves rendering identical
    // to 1.3.9. The single-slot scope is intentional — full per-entity
    // material overrides require a SkinnedMeshPass descriptor set 2
    // rewrite, which is explicitly out of scope for .
    {
        const sv::MaterialComponent* activeMat = nullptr;
        uint32_t activeMatEntity = 0;
        for (const auto& [eid, ent] : m_netEntities) {
            if (!ent.alive) continue;
            if (ent.material.overrideStrength <= 0.0f) continue;
            if (activeMat == nullptr || eid < activeMatEntity) {
                activeMat = &ent.material;
                activeMatEntity = eid;
            }
        }
        if (activeMat != nullptr) {
            m_sceneUBO.materialOverride =
                glm::vec4(activeMat->baseColorR,
                          activeMat->baseColorG,
                          activeMat->baseColorB,
                          activeMat->overrideStrength);
        } else {
            m_sceneUBO.materialOverride =
                glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        }
    }

    // Upload to GPU
    void* mapped = m_uboBuffers[m_currentFrame].info.pMappedData;
    if (mapped)
        std::memcpy(mapped, &m_sceneUBO, sizeof(SceneUBO));
}

// ──────────────────────────────────────────────────────────────────
void TestEngine::recordFrame(uint32_t imageIndex)
{
    VkCommandBuffer cmd = m_frames[m_currentFrame].cmd;
    auto ext = m_swapchain.extent();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // ── Transition images to attachment ────────────────────────────
    sv::transitionImage(cmd, m_swapchain.image(imageIndex),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    sv::transitionImage(cmd, m_motionVec.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    sv::transitionImage(cmd, m_depth.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // ── Main pass: skinned mesh ───────────────────────────────────
    VkRenderingAttachmentInfo colorAttach{};
    colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach.imageView   = m_swapchain.imageView(imageIndex);
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.clearValue.color = {{0.12f, 0.12f, 0.15f, 1.0f}};

    VkRenderingAttachmentInfo motionAttach{};
    motionAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    motionAttach.imageView   = m_motionVec.view;
    motionAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    motionAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    motionAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    motionAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingAttachmentInfo colorAttachments[] = { colorAttach, motionAttach };

    VkRenderingAttachmentInfo depthAttach{};
    depthAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttach.imageView   = m_depth.view;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo{};
    renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea           = {{0, 0}, ext};
    renderInfo.layerCount           = 1;
    renderInfo.colorAttachmentCount = 2;
    renderInfo.pColorAttachments    = colorAttachments;
    renderInfo.pDepthAttachment     = &depthAttach;

    vkCmdBeginRendering(cmd, &renderInfo);

    // Negative height flips Y to compensate for Vulkan/OpenGL NDC difference.
    // This preserves CCW winding order so back-face culling works correctly.
    VkViewport viewport{0, (float)ext.height, (float)ext.width, -(float)ext.height, 0.0f, 1.0f};
    VkRect2D   scissor{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // ── Opaque pass ─────────────────────────────────────────────────
    m_skinnedPass.bind(cmd, m_descSets[m_currentFrame],
                       m_animSys.bonePaletteDescSet());

    // reset per-frame draw accumulators
    m_drawsThisFrame = 0;
    m_trisThisFrame  = 0;

    const auto& submeshes = m_mesh.submeshes();
    const auto& materials = m_mesh.materials();
    for (size_t i = 0; i < submeshes.size(); i++) {
        const auto& sub = submeshes[i];
        bool isBlend = (sub.materialIndex >= 0 && sub.materialIndex < (int)materials.size()
                        && materials[sub.materialIndex].blendMode == sv::BlendMode::AlphaBlend);
        if (isBlend) continue; // draw transparent submeshes in second pass

        sv::SkinnedDrawCmd dc{};
        dc.vertexBuffer = m_mesh.vertexBuffer();
        dc.indexBuffer  = m_mesh.indexBuffer();
        dc.indexCount   = sub.indexCount;
        dc.firstIndex   = sub.indexOffset;
        dc.model        = glm::mat4(1.0f);
        dc.boneOffset   = m_boneOffset;
        dc.materialSet  = (i < m_materialSets.size()) ? m_materialSets[i].set : VK_NULL_HANDLE;
        m_skinnedPass.draw(cmd, dc);
        m_drawsThisFrame++;
        m_trisThisFrame += sub.indexCount / 3u;
    }

    // ── Alpha-blend pass (eyelash, hair) ──────────────────────────
    m_skinnedPass.bindBlend(cmd, m_descSets[m_currentFrame],
                            m_animSys.bonePaletteDescSet());

    for (size_t i = 0; i < submeshes.size(); i++) {
        const auto& sub = submeshes[i];
        bool isBlend = (sub.materialIndex >= 0 && sub.materialIndex < (int)materials.size()
                        && materials[sub.materialIndex].blendMode == sv::BlendMode::AlphaBlend);
        if (!isBlend) continue;

        sv::SkinnedDrawCmd dc{};
        dc.vertexBuffer = m_mesh.vertexBuffer();
        dc.indexBuffer  = m_mesh.indexBuffer();
        dc.indexCount   = sub.indexCount;
        dc.firstIndex   = sub.indexOffset;
        dc.model        = glm::mat4(1.0f);
        dc.boneOffset   = m_boneOffset;
        dc.materialSet  = (i < m_materialSets.size()) ? m_materialSets[i].set : VK_NULL_HANDLE;
        m_skinnedPass.draw(cmd, dc, true);
        m_drawsThisFrame++;
        m_trisThisFrame += sub.indexCount / 3u;
    }

    vkCmdEndRendering(cmd);

    // ── ImGui pass ────────────────────────────────────────────────
    m_imguiLayer.render(cmd, m_swapchain.imageView(imageIndex), ext.width, ext.height);

    // ── Transition swapchain to present ───────────────────────────
    sv::transitionImage(cmd, m_swapchain.image(imageIndex),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(cmd);
}

// ──────────────────────────────────────────────────────────────────
void TestEngine::drawDebugUI()
{
    ImGui::SetNextWindowPos(ImVec2(880, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("Animation Debug");

    // ── Performance HUD ──────────────────────────────────
    // Live frame / draw / VRAM counters + network
    // observability placeholders. Backed by m_perfContext which is
    // populated at the end of onFrame(dt).
    ImGui::SeparatorText("Performance");
    const auto& pc = m_perfContext;
    {
        // Frame time vs budget
        const float budget = (pc.budget.maxFrameMs > 0.f) ? pc.budget.maxFrameMs : 16.67f;
        ImVec4 col = (pc.frameTimeMs < budget * 0.75f) ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                   : (pc.frameTimeMs < budget)         ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                                         : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
        char label[64];
        snprintf(label, sizeof(label), "%.2f / %.2f ms (%.0f fps)",
                 pc.frameTimeMs, budget, pc.avgFps);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(budget > 0.f ? pc.frameTimeMs / budget : 0.f, ImVec2(-1, 0), label);
        ImGui::PopStyleColor();
    }
    ImGui::Text("CPU: %.2f ms   GPU: %.2f ms   (GPU profiler: off in lab)",
                pc.cpuFrameTimeMs, pc.gpuFrameTimeMs);
    ImGui::Text("Draws: %u / %u", pc.drawCallCount, pc.budget.maxDrawCalls);
    ImGui::Text("Tris:  %u / %u", pc.triangleCount, pc.budget.maxTriangles);
    if (pc.vramBudgetMB > 0.0f) {
        const float pct = pc.vramUsedMB / pc.vramBudgetMB;
        ImVec4 col = (pct < 0.7f) ? ImVec4(0.2f, 0.8f, 0.3f, 1.f)
                   : (pct < 0.9f) ? ImVec4(0.9f, 0.8f, 0.2f, 1.f)
                                   : ImVec4(0.9f, 0.3f, 0.2f, 1.f);
        char label[64];
        snprintf(label, sizeof(label), "VRAM %.0f / %.0f MB", pc.vramUsedMB, pc.vramBudgetMB);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(pct, ImVec2(-1, 0), label);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("VRAM: (budget query failed)");
    }
    // Network observability placeholders — zero until wires them.
    if (ImGui::TreeNode("Network Stats")) {
        const auto& net = pc.network;
        const bool idle = (net.tickMs == 0.0f && net.bytesPerSec == 0 &&
                           net.packetsPerSec == 0 && net.replicatedEntityCount == 0 &&
                           net.ackLatencyMs == 0.0f && net.droppedDatagramPct == 0.0f);
        if (idle) {
            ImGui::TextDisabled("(no network session — will populate)");
        }
        ImGui::BulletText("tickMs:               %.2f", net.tickMs);
        ImGui::BulletText("bytesPerSec:          %llu",
                          static_cast<unsigned long long>(net.bytesPerSec));
        ImGui::BulletText("packetsPerSec:        %u", net.packetsPerSec);
        ImGui::BulletText("replicatedEntityCount: %u", net.replicatedEntityCount);
        ImGui::BulletText("ackLatencyMs:         %.2f", net.ackLatencyMs);
        ImGui::BulletText("droppedDatagramPct:   %.2f %%", net.droppedDatagramPct * 100.0f);
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Mesh");
    ImGui::Text("Total indices: %u", m_mesh.totalIndexCount());
    ImGui::Text("Submeshes:     %zu", m_mesh.submeshes().size());
    ImGui::Text("Materials:     %zu", m_mesh.materials().size());
    ImGui::Text("Textures:      %zu", m_mesh.textures().size());
    ImGui::Text("Material sets: %zu", m_materialSets.size());

    // Material blend mode breakdown
    {
        int opaque = 0, blend = 0;
        for (const auto& mat : m_mesh.materials()) {
            if (mat.blendMode == sv::BlendMode::AlphaBlend) blend++;
            else opaque++;
        }
        ImGui::Text("Opaque/Blend:  %d / %d", opaque, blend);
        if (ImGui::TreeNode("Material Names")) {
            for (size_t i = 0; i < m_mesh.materials().size(); i++) {
                const auto& mat = m_mesh.materials()[i];
                ImGui::Text("[%zu] %s %s", i, mat.name.c_str(),
                    mat.blendMode == sv::BlendMode::AlphaBlend ? "[BLEND]" : "");
            }
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Skeleton");
    const auto& skelData = m_mesh.skeleton();
    ImGui::Text("Joints (glTF):  %d", skelData.jointCount());
    ImGui::Text("Joints (ozz):   %d", m_skeleton.jointCount());
    ImGui::Text("SoA groups:     %d", m_skeleton.soaCount());

    ImGui::SeparatorText("SSBO Bone Palette");
    ImGui::Text("Bone offset:    %u", m_boneOffset);
    ImGui::Text("Bones used:     %u / %u", m_animSys.currentBoneOffset(), m_animSys.maxBones());
    ImGui::ProgressBar((float)m_animSys.currentBoneOffset() / (float)m_animSys.maxBones());

    ImGui::SeparatorText("Playback Mode");
    ImGui::Checkbox("Use Blend Tree", &m_useBlendTree);
    ImGui::Checkbox("Paused", &m_paused);

    if (m_useBlendTree) {
        // ── Blend tree controls ────────────────────────────
        ImGui::SeparatorText("Blend Space 1D");
        ImGui::SliderFloat("Parameter", &m_blendParam, 0.0f, 1.0f, "%.2f");
        ImGui::Text("0.0 = rest pose, 1.0 = clip");

        ImGui::SeparatorText("Partial Body Blending");
        bool canPartial = m_splitJointIdx >= 0 && !m_upperBodyMask.empty();
        if (!canPartial) ImGui::BeginDisabled();
        ImGui::Checkbox("Enable Partial Blend", &m_enablePartialBlend);
        if (m_enablePartialBlend) {
            ImGui::SliderFloat("Upper Layer Weight", &m_upperLayerWeight, 0.0f, 1.0f, "%.2f");
            ImGui::Text("Split joint: %s",
                         m_splitJointIdx >= 0 ? m_jointNames[m_splitJointIdx].c_str() : "none");

            // Joint selector combo
            if (!m_jointNames.empty()) {
                const char* preview = m_splitJointIdx >= 0
                    ? m_jointNames[m_splitJointIdx].c_str() : "none";
                if (ImGui::BeginCombo("Split Joint", preview)) {
                    for (int i = 0; i < (int)m_jointNames.size(); i++) {
                        bool selected = (i == m_splitJointIdx);
                        if (ImGui::Selectable(m_jointNames[i].c_str(), selected)) {
                            m_splitJointIdx = i;
                            m_upperBodyMask = sv::buildJointMask(
                                m_skeleton, m_jointNames[i].c_str(), 1.0f, 0.0f);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
        }
        if (!canPartial) ImGui::EndDisabled();
    } else {
        // ── State machine controls ─────────────────────────
        ImGui::SeparatorText("State Machine");
        if (m_stateMachine) {
            ImGui::Text("Current state: %s", m_stateMachine->getCurrentStateName());
            ImGui::Text("Time:          %.3f / %.3f",
                         m_stateMachine->getCurrentTime(),
                         m_stateMachine->getCurrentDuration());
            ImGui::Text("Transitioning: %s", m_stateMachine->isTransitioning() ? "YES" : "no");
            if (m_stateMachine->isTransitioning())
                ImGui::ProgressBar(m_stateMachine->getTransitionProgress(),
                                   ImVec2(-1, 0), "crossfade");

            ImGui::SeparatorText("Clips (click to transition)");
            for (int i = 0; i < m_stateMachine->getStateCount(); i++) {
                const auto* state = m_stateMachine->getState(i);
                if (!state) continue;
                bool isCurrent = (strcmp(state->name, m_stateMachine->getCurrentStateName()) == 0);
                if (isCurrent)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                if (ImGui::Button(state->name))
                    m_stateMachine->triggerTransition(state->name);
                ImGui::SameLine();
                ImGui::Text("%.2fs %s", state->clip ? state->clip->duration : 0.0f,
                             state->loop ? "[loop]" : "[once]");
                if (isCurrent)
                    ImGui::PopStyleColor();
            }
        } else {
            ImGui::Text("No animations found — T-pose");
            ImGui::Text("Clips loaded: %zu", m_clips.size());
        }
    }

    // ── IK controls ────────────────────────────────────
    ImGui::SeparatorText("Two-Bone IK");
    {
        bool hasLegs = m_leftLegIK.startJoint >= 0 || m_rightLegIK.startJoint >= 0;
        if (!hasLegs) ImGui::BeginDisabled();
        ImGui::Checkbox("Enable Foot IK", &m_enableTwoBoneIK);
        if (m_enableTwoBoneIK) {
            ImGui::SliderFloat("Foot Target Y", &m_ikTargetY, -20.0f, 120.0f, "%.1f");
            ImGui::SliderFloat("Foot Spread X", &m_ikTargetSpreadX, 0.0f, 80.0f, "%.1f");
            ImGui::Text("Left leg:  %d -> %d -> %d",
                         m_leftLegIK.startJoint, m_leftLegIK.midJoint, m_leftLegIK.endJoint);
            ImGui::Text("Right leg: %d -> %d -> %d",
                         m_rightLegIK.startJoint, m_rightLegIK.midJoint, m_rightLegIK.endJoint);
        }
        if (!hasLegs) ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Aim IK");
    {
        bool hasHead = m_headAimIK.joint >= 0;
        if (!hasHead) ImGui::BeginDisabled();
        ImGui::Checkbox("Enable Head Aim", &m_enableAimIK);
        if (m_enableAimIK) {
            ImGui::SliderFloat3("Aim Target", &m_aimTarget.x, -300.0f, 300.0f, "%.1f");
            ImGui::Text("Head joint: %d", m_headAimIK.joint);
        }
        if (!hasHead) ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Root Motion");
    ImGui::Checkbox("Enable Root Motion", &m_rootMotionEnabled);
    if (m_rootMotionEnabled) {
        ImGui::Text("Delta pos: (%.3f, %.3f, %.3f)",
                     m_lastRootDelta.deltaPosition.x,
                     m_lastRootDelta.deltaPosition.y,
                     m_lastRootDelta.deltaPosition.z);
        ImGui::Text("Accum:     (%.1f, %.1f, %.1f)",
                     m_rootMotionAccum.x, m_rootMotionAccum.y, m_rootMotionAccum.z);
        if (ImGui::Button("Reset Accum"))
            m_rootMotionAccum = glm::vec3(0.0f);
    }

    ImGui::SeparatorText("Joint Names");
    if (ImGui::BeginChild("JointList", ImVec2(0, 120), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < skelData.jointCount(); i++) {
            ImGui::Text("[%3d] %s (parent=%d)",
                        i, skelData.joints[i].name.c_str(), skelData.joints[i].parent);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// ══════════════════════════════════════════════════════════════════
// replication demo panel.
//
// Two-part ImGui window. Top half shows connection state, the
// decoded NetTransform, interpolation alpha, and wire counters.
// Bottom half is a 2D canvas (top-down view) with a grid and a
// filled dot at the cube's current XZ position — this is the
// "cube moving in sync" visual checkpoint.
// ══════════════════════════════════════════════════════════════════
void TestEngine::drawNetworkDemoPanel()
{
    // Fixed position + size so the visual checkpoint PNG is
    // deterministic regardless of window resizing.
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Network Demo");

    // Compute interpolation alpha: how far along we are between the
    // previous and current snapshot, based on wall clock vs expected
    // tick period. Clamped to [0, 1] so a long gap doesn't overshoot.
    const double nowSec       = glfwGetTime();
    const double tickPeriodSec =
        (m_netTickHz > 0) ? (1.0 / static_cast<double>(m_netTickHz)) : 0.033;
    double rawAlpha = 0.0;
    if (m_netHaveData) {
        rawAlpha = (nowSec - m_netLastDecodeWallSec) / tickPeriodSec;
        if (rawAlpha < 0.0) rawAlpha = 0.0;
        if (rawAlpha > 1.0) rawAlpha = 1.0;
    }
    const float alpha = static_cast<float>(rawAlpha);
    const sv::NetTransform display =
        m_netHaveData
            ? sv::lerpNetTransform(m_netPrev, m_netCurrent, alpha)
            : sv::NetTransform{};

    // ── Top half: text readout ────────────────────────────────────
    if (m_netConnectTarget.empty()) {
        ImGui::TextDisabled("Offline (no --connect target)");
    } else {
        ImGui::Text("Target:    %s", m_netConnectTarget.c_str());
        if (m_netConnected) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f),
                               "Connected");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f),
                               "Disconnected");
        }
    }

    // schema handshake state. Snapshot under the mutex
    // because the MsQuic worker thread can mutate these fields any
    // time the preamble arrives.
    NetSchemaState   schemaState;
    std::string      schemaDetail;
    uint32_t         schemaSemver;
    uint16_t         schemaTypeCount;
    {
        std::lock_guard<std::mutex> lk(m_netSchemaMu);
        schemaState     = m_netSchemaState;
        schemaDetail    = m_netSchemaDetail;
        schemaSemver    = m_netSchemaSemver;
        schemaTypeCount = m_netSchemaTypeCount;
    }
    switch (schemaState) {
        case NetSchemaState::Pending:
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f),
                "Schema:    pending");
            break;
        case NetSchemaState::Ok:
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f),
                "Schema:    OK (server %u.%u.%u, %u types)",
                sv::net::semverMajor(schemaSemver),
                sv::net::semverMinor(schemaSemver),
                sv::net::semverPatch(schemaSemver),
                static_cast<unsigned>(schemaTypeCount));
            break;
        case NetSchemaState::Mismatch:
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
                "Schema:    MISMATCH");
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.5f, 1.0f),
                "  %s", schemaDetail.c_str());
            break;
        case NetSchemaState::Unknown:
            ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f),
                "Schema:    server-unknown");
            ImGui::TextColored(ImVec4(0.85f, 0.8f, 0.55f, 1.0f),
                "  %s", schemaDetail.c_str());
            break;
    }

    ImGui::Separator();
    ImGui::Text("Last tick: %u",  static_cast<unsigned>(m_netLastTick));
    ImGui::Text("Datagrams: %llu",
                static_cast<unsigned long long>(m_netDatagramsReceived));
    ImGui::Text("Bytes:     %llu",
                static_cast<unsigned long long>(m_netBytesReceived));
    ImGui::Text("Decoded:   %llu  Dropped: %llu",
                static_cast<unsigned long long>(m_netFramesDecoded),
                static_cast<unsigned long long>(m_netFramesDropped));
    if (m_netConnected) {
        // reliable-message counters from the transport
        // stats. On a healthy session this should read "1 / 0" forever
        // (one preamble received, zero sent from the client side).
        const auto cs = m_netConn.stats();
        ImGui::Text("Reliable:  rx %llu  tx %llu",
                    static_cast<unsigned long long>(cs.reliableMessagesReceived),
                    static_cast<unsigned long long>(cs.reliableMessagesSent));
    }
    ImGui::Text("Alpha:     %.2f", alpha);

    // ── client identity + edit buttons ───────────────────
    // Snapshot state under the mutex so the UI thread can branch
    // without a race against the worker thread's welcome arrival.
    uint32_t            localClientId = 0;
    sv::PermissionScope localScope    = sv::PermissionScope::Spectator;
    uint32_t            localAvatarId = 0;
    bool                welcomed      = false;
    {
        std::lock_guard<std::mutex> lk(m_netStateMu);
        localClientId = m_netClientId;
        localScope    = m_netScope;
        localAvatarId = m_netAvatarEntityId;
        welcomed      = m_netWelcomed;
    }
    ImGui::Separator();
    if (welcomed) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.95f, 1.0f),
            "Identity:  client %u  avatar %u",
            static_cast<unsigned>(localClientId),
            static_cast<unsigned>(localAvatarId));
        ImGui::Text("Scope:     %s",
            sv::permissionScopeToString(localScope));
    } else {
        ImGui::TextDisabled("Identity:  pending welcome");
    }

    const bool canEdit = welcomed && (localScope >= sv::PermissionScope::Editor);

    // Keybinds: Ctrl+Z undo, Ctrl+Y redo, arrow keys move the avatar
    // in the XZ plane. The keybinds are checked via ImGui so they
    // don't fight the existing GLFW free-fly camera controls.
    if (canEdit) {
        ImGuiIO& io = ImGui::GetIO();
        const float step = 5.0f;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) sendUndoRequest();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) sendRedoRequest();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  sendAvatarMove(-step, 0.0f, 0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) sendAvatarMove( step, 0.0f, 0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    sendAvatarMove(0.0f, 0.0f, -step);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  sendAvatarMove(0.0f, 0.0f,  step);
    }

    ImGui::BeginDisabled(!canEdit);
    if (ImGui::Button("X-")) sendAvatarMove(-5.0f, 0.0f, 0.0f);
    ImGui::SameLine();
    if (ImGui::Button("X+")) sendAvatarMove( 5.0f, 0.0f, 0.0f);
    ImGui::SameLine();
    if (ImGui::Button("Z-")) sendAvatarMove(0.0f, 0.0f, -5.0f);
    ImGui::SameLine();
    if (ImGui::Button("Z+")) sendAvatarMove(0.0f, 0.0f,  5.0f);
    ImGui::SameLine();
    if (ImGui::Button("Undo")) sendUndoRequest();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) sendRedoRequest();
    ImGui::EndDisabled();

    ImGui::Text("Edits:     set=%llu undo=%llu redo=%llu",
                static_cast<unsigned long long>(m_netSetFieldSent),
                static_cast<unsigned long long>(m_netUndoSent),
                static_cast<unsigned long long>(m_netRedoSent));
    ImGui::Text("Entities:  %zu  RxTx:  %llu",
                m_netEntities.size(),
                static_cast<unsigned long long>(m_netReliableTxApplied));

    // ── editor bridge status ─────────────────────
    if (m_editorBridgePort != 0) {
        ImGui::Separator();
        if (m_editorBridge && m_editorBridge->running()) {
            const size_t clients = m_editorBridge->clientCount();
            const ImVec4 col = clients > 0
                ? ImVec4(0.35f, 0.9f, 0.45f, 1.0f)    // green = linked
                : ImVec4(0.85f, 0.85f, 0.4f, 1.0f);   // yellow = idle
            ImGui::TextColored(col,
                "Bridge:    :%u  clients=%zu",
                static_cast<unsigned>(m_editorBridgePort),
                clients);
            ImGui::Text("Bridge rx/tx: %llu / %llu",
                static_cast<unsigned long long>(m_editorBridge->messagesInbound()),
                static_cast<unsigned long long>(m_editorBridge->messagesOutbound()));
            ImGui::Text("Blender edits applied: %llu  (states pushed %llu)",
                static_cast<unsigned long long>(m_bridgeMoveApplied),
                static_cast<unsigned long long>(m_bridgeStatePushed));
            // asset + parent counters.
            ImGui::Text("Bridge assets: %llu recv / %llu uploaded",
                static_cast<unsigned long long>(m_bridgeAssetsReceived),
                static_cast<unsigned long long>(m_bridgeAssetsUploaded));
            ImGui::Text("Bridge parents: %llu applied",
                static_cast<unsigned long long>(m_bridgeParentChanges));
            // light counter — sent via the bridge
            // (pumpBridgeLights) and applied via SetField dispatch
            // (drainNetReliableInbox). Observer clients with no bridge
            // show sent=0 but still bump applied.
            ImGui::Text("Bridge lights: %llu sent / %llu applied",
                static_cast<unsigned long long>(m_bridgeLightsSent),
                static_cast<unsigned long long>(m_bridgeLightsApplied));
            // camera + material counters. Same shape
            // as lights — observer clients show sent=0 but bump applied
            // when an inbound SetField broadcast lands.
            ImGui::Text("Bridge cameras: %llu sent / %llu applied",
                static_cast<unsigned long long>(m_bridgeCamerasSent),
                static_cast<unsigned long long>(m_bridgeCamerasApplied));
            ImGui::Text("Bridge materials: %llu sent / %llu applied",
                static_cast<unsigned long long>(m_bridgeMaterialsSent),
                static_cast<unsigned long long>(m_bridgeMaterialsApplied));
        } else {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.4f, 1.0f),
                "Bridge:    :%u  (not yet started)",
                static_cast<unsigned>(m_editorBridgePort));
        }
    }

    ImGui::Separator();
    ImGui::Text("Entity 1 (cube): (%7.2f, %7.2f, %7.2f)",
                display.posX, display.posY, display.posZ);

    // count entities with a non-zero parent and
    // surface a dedicated line so the visual checkpoint can show
    // "Parented: Client1->Cube" etc. without sifting through the
    // per-entity canvas labels.
    {
        size_t parentedCount = 0;
        for (const auto& kv : m_netEntities) {
            if (kv.second.parent.parentEntityId != 0) ++parentedCount;
        }
        if (parentedCount > 0) {
            ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f),
                "Parented entities: %zu", parentedCount);
            for (const auto& kv : m_netEntities) {
                if (kv.second.parent.parentEntityId == 0) continue;
                ImGui::BulletText("#%u -> #%u",
                    static_cast<unsigned>(kv.first),
                    static_cast<unsigned>(kv.second.parent.parentEntityId));
            }
        }
    }

    // surface the "active editor camera" + "active
    // material override" lines so the visual checkpoint shows which
    // entity drives each override. The override-pick logic mirrors
    // updateSceneUBO exactly (first entity by sorted entityId).
    {
        const sv::CameraComponent* activeCam = nullptr;
        uint32_t activeCamEntity = 0;
        for (const auto& kv : m_netEntities) {
            const auto& c = kv.second.camera;
            if (c.fovDeg <= 0.0f) continue;
            if (c.farPlane <= c.nearPlane) continue;
            if (activeCam == nullptr || kv.first < activeCamEntity) {
                activeCam = &c;
                activeCamEntity = kv.first;
            }
        }
        if (activeCam != nullptr) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                "Editor camera: #%u  fov=%.1f  near=%.2f  far=%.1f",
                static_cast<unsigned>(activeCamEntity),
                activeCam->fovDeg,
                activeCam->nearPlane,
                activeCam->farPlane);
        }

        const sv::MaterialComponent* activeMat = nullptr;
        uint32_t activeMatEntity = 0;
        for (const auto& kv : m_netEntities) {
            const auto& m = kv.second.material;
            if (m.overrideStrength <= 0.0f) continue;
            if (activeMat == nullptr || kv.first < activeMatEntity) {
                activeMat = &m;
                activeMatEntity = kv.first;
            }
        }
        if (activeMat != nullptr) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.85f, 1.0f),
                "Material override: #%u  rgb=(%.2f, %.2f, %.2f)  strength=%.2f",
                static_cast<unsigned>(activeMatEntity),
                activeMat->baseColorR,
                activeMat->baseColorG,
                activeMat->baseColorB,
                activeMat->overrideStrength);
        }
    }

    // count entities with an active LightComponent
    // (type != 0 && intensity > 0) and surface a "Lit entities" block
    // analogous to the parented-entities block. The visual checkpoint
    // reads this line to prove both clients see the same light state
    // after a SetField broadcast.
    {
        size_t litCount = 0;
        for (const auto& kv : m_netEntities) {
            if (kv.second.light.type == 0) continue;
            if (kv.second.light.intensity <= 0.0f) continue;
            ++litCount;
        }
        if (litCount > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                "Lit entities: %zu", litCount);
            for (const auto& kv : m_netEntities) {
                const ClientEntity& e = kv.second;
                if (e.light.type == 0 || e.light.intensity <= 0.0f) continue;
                const char* typeStr = "?";
                switch (e.light.type) {
                    case 1: typeStr = "dir";   break;
                    case 2: typeStr = "point"; break;
                    case 3: typeStr = "spot";  break;
                    default: break;
                }
                ImGui::BulletText("#%u -> %s (%.2f, %.2f, %.2f) I:%.2f r:%.1f",
                    static_cast<unsigned>(kv.first),
                    typeStr,
                    e.light.colorR, e.light.colorG, e.light.colorB,
                    e.light.intensity, e.light.range);
            }
        }
    }

    // ── Bottom half: 2D canvas ────────────────────────────────────
    // Draw a top-down view of the XZ plane. X axis = world X, Y
    // axis on the canvas = -Z (so +Z is up on screen). Grid spacing
    // matches the default orbit radius (100 units) for a clean
    // checkpoint image.
    ImGui::Separator();
    ImGui::Text("Top-down view (XZ plane, 1 px = 1 unit)");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 canvasTL = ImGui::GetCursorScreenPos();
    const float  canvasW  = 300.0f;
    const float  canvasH  = 180.0f;
    const ImVec2 canvasBR(canvasTL.x + canvasW, canvasTL.y + canvasH);
    const ImVec2 canvasCT(canvasTL.x + canvasW * 0.5f,
                          canvasTL.y + canvasH * 0.5f);

    // Background + border
    draw->AddRectFilled(canvasTL, canvasBR, IM_COL32(16, 16, 24, 255));
    draw->AddRect      (canvasTL, canvasBR, IM_COL32(80, 80, 100, 255));

    // Grid lines every 50 world units
    const ImU32 gridCol = IM_COL32(50, 50, 70, 255);
    for (int gx = -150; gx <= 150; gx += 50) {
        const float x = canvasCT.x + static_cast<float>(gx);
        draw->AddLine(ImVec2(x, canvasTL.y),
                      ImVec2(x, canvasBR.y), gridCol);
    }
    for (int gz = -90; gz <= 90; gz += 50) {
        const float y = canvasCT.y - static_cast<float>(gz);
        draw->AddLine(ImVec2(canvasTL.x, y),
                      ImVec2(canvasBR.x, y), gridCol);
    }
    // Centre crosshair
    draw->AddLine(ImVec2(canvasCT.x - 5, canvasCT.y),
                  ImVec2(canvasCT.x + 5, canvasCT.y),
                  IM_COL32(150, 150, 200, 255));
    draw->AddLine(ImVec2(canvasCT.x, canvasCT.y - 5),
                  ImVec2(canvasCT.x, canvasCT.y + 5),
                  IM_COL32(150, 150, 200, 255));

    // ── per-entity dots + nameplates ─────────────────────
    // Iterate the entity map. Cube (entityId 1) renders orange;
    // the local avatar renders bright cyan; other avatars render
    // magenta. Each dot carries a text label above it so the
    // visual checkpoint can show "Client1" / "Client2" labels.
    auto colorFor = [&](uint32_t entityId, uint32_t ownerId) -> ImU32 {
        if (entityId == 1) return IM_COL32(255, 160, 40, 255); // orange cube
        if (ownerId == localClientId && localClientId != 0) {
            return IM_COL32(80, 220, 255, 255);                // local = cyan
        }
        return IM_COL32(230, 100, 220, 255);                    // other = magenta
    };
    auto labelFor = [&](uint32_t entityId, uint32_t ownerId) -> std::string {
        if (entityId == 1) return "Cube";
        if (ownerId == 0)  return "Entity " + std::to_string(entityId);
        std::string name = "Client" + std::to_string(ownerId);
        if (ownerId == localClientId && localClientId != 0) name += " (you)";
        return name;
    };

    for (const auto& kv : m_netEntities) {
        const ClientEntity& ent = kv.second;
        if (!ent.alive || !ent.haveData) continue;
        // Per-entity interpolation alpha using the same clock as
        // the single-cube path.
        double entAlpha = (nowSec - ent.lastDecodeWallSec) / tickPeriodSec;
        if (entAlpha < 0.0) entAlpha = 0.0;
        if (entAlpha > 1.0) entAlpha = 1.0;
        const sv::NetTransform lerped = sv::lerpNetTransform(
            ent.prevState, ent.currentState, static_cast<float>(entAlpha));

        float dx = canvasCT.x + lerped.posX;
        float dy = canvasCT.y - lerped.posZ;
        if (dx < canvasTL.x + 6) dx = canvasTL.x + 6;
        if (dy < canvasTL.y + 6) dy = canvasTL.y + 6;
        if (dx > canvasBR.x - 6) dx = canvasBR.x - 6;
        if (dy > canvasBR.y - 6) dy = canvasBR.y - 6;

        const ImU32 dotCol = colorFor(ent.entityId, ent.ownerClientId);
        draw->AddCircleFilled(ImVec2(dx, dy), 6.5f, dotCol);
        draw->AddCircle      (ImVec2(dx, dy), 8.0f,
                              IM_COL32(255, 255, 255, 180), 12, 1.5f);

        // overlay a yellow open ring on every
        // entity that is emitting light, with a single-char type tag
        // to the right. Renders above the dot so the visual
        // checkpoint PNG has unambiguous per-entity light state.
        if (ent.light.type != 0 && ent.light.intensity > 0.0f) {
            const ImU32 lightCol = IM_COL32(255, 220, 60, 255);
            draw->AddCircle(ImVec2(dx, dy), 12.0f, lightCol, 18, 2.0f);
            const char* tag = "?";
            switch (ent.light.type) {
                case 1: tag = "D"; break;  // directional
                case 2: tag = "P"; break;  // point
                case 3: tag = "S"; break;  // spot
                default: break;
            }
            draw->AddText(ImVec2(dx + 14.0f, dy + 2.0f), lightCol, tag);
        }

        const std::string name = labelFor(ent.entityId, ent.ownerClientId);
        draw->AddText(ImVec2(dx + 9.0f, dy - 7.0f),
                      IM_COL32(240, 240, 240, 230), name.c_str());
    }

    // Reserve the canvas space so subsequent ImGui widgets don't
    // overlap it.
    ImGui::Dummy(ImVec2(canvasW, canvasH));

    ImGui::End();
}

// ══════════════════════════════════════════════════════════════════
// ImGui asset browser panel (drag source) +
// thumbnail bake panel (drop target + cache status).
// ══════════════════════════════════════════════════════════════════
void TestEngine::drawAssetPanels()
{
    // ── Asset Browser panel ───────────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("Asset Browser");

    if (!m_browserScanned) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Browser scan failed at startup");
    } else {
        ImGui::Text("Root: %s", m_assetBrowser.rootDir().c_str());
        ImGui::Text("Entries: %zu", m_assetBrowser.size());
        if (ImGui::Button("Rescan")) {
            m_assetBrowser.rescan();
            // After rescan, recheck thumbnail cache validity against
            // the (possibly updated) entry mtimes.
            m_thumbnailCache.invalidateStale(m_assetBrowser.entries());
        }
        ImGui::Separator();

        if (ImGui::BeginChild("AssetList", ImVec2(0, 200), ImGuiChildFlags_Borders)) {
            const auto& entries = m_assetBrowser.entries();
            for (int i = 0; i < (int)entries.size(); i++) {
                const auto& e = entries[i];
                char label[256];
                snprintf(label, sizeof(label), "[%s] %s",
                         sv::assetKindToString(e.kind), e.relativePath.c_str());

                bool selected = (i == m_browserSelected);
                if (ImGui::Selectable(label, selected))
                    m_browserSelected = i;

                // Drag source — payload contract:
                //   payload type:  "STRATUMV_ASSET_PATH"
                //   payload data:  null-terminated relativePath
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    ImGui::SetDragDropPayload(
                        "STRATUMV_ASSET_PATH",
                        e.relativePath.c_str(),
                        e.relativePath.size() + 1);
                    ImGui::Text("Drag: %s", e.name.c_str());
                    ImGui::EndDragDropSource();
                }
            }
        }
        ImGui::EndChild();

        // Selected entry detail
        if (m_browserSelected >= 0 &&
            m_browserSelected < (int)m_assetBrowser.size()) {
            const auto& sel = m_assetBrowser.entries()[m_browserSelected];
            ImGui::Text("Sel: %s", sel.relativePath.c_str());
            ImGui::Text("mtime: %lld", (long long)sel.lastModified);
            const auto* cached = m_thumbnailCache.find(sel.relativePath);
            ImGui::Text("Thumb: %s",
                        cached ? "cached" : "not cached");

            // upload-to-server button, gated on the
            // reliable-stream being up and the client scope being
            // at least Editor.
            bool canUpload = false;
            {
                std::lock_guard<std::mutex> lk(m_netStateMu);
                canUpload = m_netConnected && m_netWelcomed &&
                            m_netScope >= sv::PermissionScope::Editor;
            }
            if (!canUpload) ImGui::BeginDisabled();
            if (ImGui::Button("Upload to server")) {
                uploadAssetFromDisk(sel.relativePath, sel.absolutePath);
            }
            if (!canUpload) ImGui::EndDisabled();
        }
    }
    ImGui::End();

    // ── Thumbnail Bake panel ──────────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(380, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("Thumbnail Bake");

    ImGui::TextWrapped(
        "Drag an asset from the browser onto the drop zone below. "
        "Click 'Bake' to render the currently-loaded mesh as a 256x256 "
        "PNG sibling at <asset>.thumb.png and register it in the cache.");
    ImGui::Separator();

    // Drop zone — invisible button + drop target. Highlights when an
    // active drag is over it.
    ImGui::Text("Drop Zone:");
    ImVec2 dropSize(-1.0f, 60.0f);
    ImVec4 col = m_dropAssetRelPath.empty()
        ? ImVec4(0.20f, 0.20f, 0.30f, 0.6f)
        : ImVec4(0.15f, 0.40f, 0.20f, 0.7f);
    ImGui::PushStyleColor(ImGuiCol_Button,        col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col);
    ImGui::Button(m_dropAssetRelPath.empty()
        ? "(drop a STRATUMV_ASSET_PATH payload here)"
        : m_dropAssetRelPath.c_str(),
        dropSize);
    ImGui::PopStyleColor(3);

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("STRATUMV_ASSET_PATH"))
        {
            const char* relPath = static_cast<const char*>(payload->Data);
            m_dropAssetRelPath = relPath;
            // Resolve absolute path via the browser.
            if (const auto* e = m_assetBrowser.findByRelativePath(m_dropAssetRelPath))
                m_dropAssetAbsPath = e->absolutePath;
            else
                m_dropAssetAbsPath.clear();
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    bool canBake = !m_dropAssetRelPath.empty() && !m_dropAssetAbsPath.empty();
    if (!canBake) ImGui::BeginDisabled();
    if (ImGui::Button("Bake current mesh -> thumbnail PNG")) {
        m_bakeRequested = true;
        m_bakeRelPath   = m_dropAssetRelPath;
        m_bakeAbsPath   = m_dropAssetAbsPath;
    }
    if (!canBake) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear drop")) {
        m_dropAssetRelPath.clear();
        m_dropAssetAbsPath.clear();
    }

    if (!m_lastBakeStatus.empty()) {
        ImGui::TextWrapped("%s", m_lastBakeStatus.c_str());
    }

    ImGui::Separator();
    ImGui::Text("Cache: %zu entries", m_thumbnailCache.size());
    if (ImGui::Button("Invalidate stale")) {
        size_t dropped = m_thumbnailCache.invalidateStale(m_assetBrowser.entries());
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "invalidateStale dropped %zu entries", dropped);
        m_lastBakeStatus = buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear cache")) {
        size_t before = m_thumbnailCache.size();
        m_thumbnailCache.clear();
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Cleared %zu cache entries (sibling files deleted)",
                 before);
        m_lastBakeStatus = buf;
    }

    if (ImGui::BeginChild("CacheList", ImVec2(0, 110), ImGuiChildFlags_Borders)) {
        for (const auto& kv : m_thumbnailCache.entries()) {
            const auto& e = kv.second;
            // Pull current mtime from the browser to show validity status.
            int64_t curMtime = 0;
            if (const auto* be = m_assetBrowser.findByRelativePath(e.relativePath))
                curMtime = be->lastModified;
            bool valid = m_thumbnailCache.isValid(e.relativePath, curMtime);
            ImGui::TextColored(
                valid ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                      : ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "%s  %llu B  %s",
                e.relativePath.c_str(),
                (unsigned long long)e.byteSize,
                valid ? "[VALID]" : "[STALE]");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// ══════════════════════════════════════════════════════════════════
// GPU thumbnail bake.
// Records a single-time command buffer that:
//   1. Transitions offscreen targets to attachment layouts
//   2. Renders the current mesh into the 256x256 offscreen targets
//      via the existing SkinnedMeshPass pipeline (opaque + blend)
//   3. Transitions the color target to TRANSFER_SRC
//   4. Copies it to a staging buffer
// Then maps the staging buffer, swizzles BGRA -> RGBA, encodes a PNG
// via stb_image_write, and calls m_thumbnailCache.markBaked().
// ══════════════════════════════════════════════════════════════════
bool TestEngine::bakeThumbnail(const std::string& relPath,
                               const std::string& absPath)
{
    if (absPath.empty()) {
        m_lastBakeStatus = "bake: empty absolute path";
        return false;
    }

    VkDevice     device = m_vkCtx.device();
    VmaAllocator alloc  = m_vkCtx.allocator();

    // Drain in-flight work so the bone palette SSBO and image layouts
    // are settled before we record the offscreen pass.
    vkDeviceWaitIdle(device);

    // Save the current scene UBO so we can override the camera with a
    // fit-to-mesh framing for the thumbnail (the user's interactive
    // camera might not have the mesh in view) and restore it before
    // returning. The mapped UBO is read by the GPU at execution time,
    // so the override must persist until vkDeviceWaitIdle below
    // confirms the bake has completed.
    SceneUBO* uboMapped = static_cast<SceneUBO*>(
        m_uboBuffers[m_currentFrame].info.pMappedData);
    SceneUBO savedUbo{};
    if (uboMapped) {
        savedUbo = *uboMapped;
        SceneUBO thumbUbo = savedUbo;

        // fit camera driven by VkMesh::aabb instead of
        // hardcoded CC5 numbers. AABB is local-space (the skinning
        // matrices are identity at rest), so framing it directly
        // gives a reasonable bounding box regardless of unit system
        // or axis convention.
        const sv::AABB& bounds = m_mesh.aabb();
        glm::vec3 target(0.0f, 0.95f, 0.0f);
        glm::vec3 eye   (0.0f, 1.10f, 3.20f);
        float fovRad = glm::radians(35.0f);
        float nearZ  = 0.05f;
        float farZ   = 50.0f;

        if (bounds.valid) {
            const glm::vec3 center    = bounds.center();
            const float     longest   = bounds.longestAxis();
            // Place the camera along +Z looking at the AABB center.
            // distance is computed so the bounding sphere of the box
            // fits inside the vertical FOV with a 20% padding factor.
            const float radius   = glm::length(bounds.extents());
            const float padding  = 1.20f;
            const float fitDist  = (radius * padding) / std::tan(fovRad * 0.5f);
            // Clamp to sensible bounds so a zero-sized mesh doesn't
            // produce eye == target (which would break lookAt).
            const float distance = (fitDist > 0.0001f) ? fitDist : 3.2f;

            target = center;
            eye    = center + glm::vec3(0.0f, 0.0f, distance);

            // Adjust near/far so huge or tiny meshes both render OK.
            nearZ  = (distance > 1.0f) ? distance * 0.01f : 0.001f;
            farZ   = distance + radius * 4.0f + 10.0f;

            printf("[TestEngine] bakeThumbnail fit: center=(%.2f,%.2f,%.2f) "
                   "longest=%.3f radius=%.3f distance=%.3f\n",
                   center.x, center.y, center.z,
                   longest, radius, distance);
        } else {
            printf("[TestEngine] bakeThumbnail: AABB invalid, using CC5 fallback camera\n");
        }

        // NB: the main pass uses a negative-height viewport for the
        // Vulkan Y-flip, so the proj here does NOT include the
        // proj[1][1] *= -1 flip — that would double-flip.
        glm::mat4 view  = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj  = glm::perspective(fovRad, 1.0f, nearZ, farZ);
        glm::mat4 vp    = proj * view;
        thumbUbo.viewProj    = vp;
        thumbUbo.invViewProj = glm::inverse(vp);
        thumbUbo.invViewProjUnjittered = thumbUbo.invViewProj;
        thumbUbo.cameraPos   = glm::vec4(eye, 0.0f);
        std::memcpy(uboMapped, &thumbUbo, sizeof(SceneUBO));
    }

    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();

    // ── Transition offscreen targets to attachment layouts ───────
    sv::transitionImage(cmd, m_thumbColor.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    sv::transitionImage(cmd, m_thumbMotion.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    sv::transitionImage(cmd, m_thumbDepth.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // ── Begin offscreen rendering (matches main render layout) ──
    VkRenderingAttachmentInfo colorAttach{};
    colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach.imageView   = m_thumbColor.view;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.clearValue.color = {{0.10f, 0.12f, 0.18f, 1.0f}};

    VkRenderingAttachmentInfo motionAttach{};
    motionAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    motionAttach.imageView   = m_thumbMotion.view;
    motionAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    motionAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    motionAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    motionAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingAttachmentInfo colorAttachments[] = { colorAttach, motionAttach };

    VkRenderingAttachmentInfo depthAttach{};
    depthAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttach.imageView   = m_thumbDepth.view;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo{};
    renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea           = {{0, 0}, {THUMB_W, THUMB_H}};
    renderInfo.layerCount           = 1;
    renderInfo.colorAttachmentCount = 2;
    renderInfo.pColorAttachments    = colorAttachments;
    renderInfo.pDepthAttachment     = &depthAttach;

    vkCmdBeginRendering(cmd, &renderInfo);

    // Match the negative-height viewport flip used by the main pass.
    VkViewport viewport{0, (float)THUMB_H, (float)THUMB_W, -(float)THUMB_H, 0.0f, 1.0f};
    VkRect2D   scissor{{0, 0}, {THUMB_W, THUMB_H}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Reuse the current frame's scene + bone palette descriptor sets.
    // The bake captures whatever pose the user is currently looking at.
    m_skinnedPass.bind(cmd, m_descSets[m_currentFrame],
                       m_animSys.bonePaletteDescSet());

    const auto& submeshes = m_mesh.submeshes();
    const auto& materials = m_mesh.materials();
    for (size_t i = 0; i < submeshes.size(); i++) {
        const auto& sub = submeshes[i];
        bool isBlend = (sub.materialIndex >= 0
                        && sub.materialIndex < (int)materials.size()
                        && materials[sub.materialIndex].blendMode == sv::BlendMode::AlphaBlend);
        if (isBlend) continue;

        sv::SkinnedDrawCmd dc{};
        dc.vertexBuffer = m_mesh.vertexBuffer();
        dc.indexBuffer  = m_mesh.indexBuffer();
        dc.indexCount   = sub.indexCount;
        dc.firstIndex   = sub.indexOffset;
        dc.model        = glm::mat4(1.0f);
        dc.boneOffset   = m_boneOffset;
        dc.materialSet  = (i < m_materialSets.size()) ? m_materialSets[i].set : VK_NULL_HANDLE;
        m_skinnedPass.draw(cmd, dc);
    }

    // Alpha-blend submeshes (eyelash, hair) — second pass.
    m_skinnedPass.bindBlend(cmd, m_descSets[m_currentFrame],
                            m_animSys.bonePaletteDescSet());
    for (size_t i = 0; i < submeshes.size(); i++) {
        const auto& sub = submeshes[i];
        bool isBlend = (sub.materialIndex >= 0
                        && sub.materialIndex < (int)materials.size()
                        && materials[sub.materialIndex].blendMode == sv::BlendMode::AlphaBlend);
        if (!isBlend) continue;

        sv::SkinnedDrawCmd dc{};
        dc.vertexBuffer = m_mesh.vertexBuffer();
        dc.indexBuffer  = m_mesh.indexBuffer();
        dc.indexCount   = sub.indexCount;
        dc.firstIndex   = sub.indexOffset;
        dc.model        = glm::mat4(1.0f);
        dc.boneOffset   = m_boneOffset;
        dc.materialSet  = (i < m_materialSets.size()) ? m_materialSets[i].set : VK_NULL_HANDLE;
        m_skinnedPass.draw(cmd, dc, /*alphaBlend=*/true);
    }

    vkCmdEndRendering(cmd);

    // ── Transition color → transfer src and copy to staging ──────
    sv::transitionImage(cmd, m_thumbColor.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    const VkDeviceSize bufSize = (VkDeviceSize)THUMB_W * THUMB_H * 4;
    sv::VkBuf staging = sv::VkBuf::create(alloc, bufSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {THUMB_W, THUMB_H, 1};
    vkCmdCopyImageToBuffer(cmd, m_thumbColor.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        staging.buffer, 1, &region);

    m_vkCtx.endSingleTimeCommands(cmd);

    // Restore the user-camera UBO now that the GPU has finished
    // reading the override (endSingleTimeCommands is fence-synchronous).
    if (uboMapped) {
        std::memcpy(uboMapped, &savedUbo, sizeof(SceneUBO));
    }

    // ── Map staging, BGRA → RGBA swizzle, encode PNG ─────────────
    void* mapped = nullptr;
    vmaMapMemory(alloc, staging.allocation, &mapped);
    if (!mapped) {
        staging.destroy(alloc);
        m_lastBakeStatus = "bake: vmaMapMemory failed";
        return false;
    }

    std::vector<uint8_t> rgba(THUMB_W * THUMB_H * 4);
    const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
    for (uint32_t i = 0; i < THUMB_W * THUMB_H; i++) {
        rgba[i * 4 + 0] = pixels[i * 4 + 2]; // R ← B
        rgba[i * 4 + 1] = pixels[i * 4 + 1]; // G
        rgba[i * 4 + 2] = pixels[i * 4 + 0]; // B ← R
        rgba[i * 4 + 3] = pixels[i * 4 + 3]; // A
    }
    vmaUnmapMemory(alloc, staging.allocation);
    staging.destroy(alloc);

    std::string thumbPath = absPath + ".thumb.png";
    int rc = stbi_write_png(thumbPath.c_str(),
                            (int)THUMB_W, (int)THUMB_H, 4,
                            rgba.data(), (int)(THUMB_W * 4));
    if (rc == 0) {
        m_lastBakeStatus = std::string("bake: stbi_write_png failed for ") + thumbPath;
        return false;
    }

    // Look up source mtime so the cache can detect later edits.
    int64_t mtime = 0;
    {
        std::error_code ec;
        auto tt = std::filesystem::last_write_time(absPath, ec);
        if (!ec) mtime = tt.time_since_epoch().count();
    }

    m_thumbnailCache.markBaked(relPath, absPath, mtime, rgba.size());

    char buf[256];
    snprintf(buf, sizeof(buf),
             "bake OK: %s (%ux%u, %zu B)",
             thumbPath.c_str(), THUMB_W, THUMB_H, rgba.size());
    m_lastBakeStatus = buf;
    printf("[TestEngine] Thumbnail baked: %s\n", thumbPath.c_str());
    return true;
}

// ══════════════════════════════════════════════════════════════════
// texture-blit bake path.
// Loads a texture asset via sv::VkTex (which uploads it as an
// RGBA8_SRGB GPU image with mipmaps), then issues vkCmdBlitImage to
// scale mip 0 into the RGBA8_SRGB offscreen target m_thumbBlitColor.
// No render pass is needed — a blit can do arbitrary-to-arbitrary
// resampling with a linear filter. The result is copied to a staging
// buffer, written directly as a PNG (no component swizzle), and
// registered with m_thumbnailCache.
// ══════════════════════════════════════════════════════════════════
bool TestEngine::bakeTextureThumbnail(const std::string& relPath,
                                      const std::string& absPath)
{
    if (absPath.empty()) {
        m_lastBakeStatus = "texture bake: empty absolute path";
        return false;
    }

    VkDevice     device = m_vkCtx.device();
    VmaAllocator alloc  = m_vkCtx.allocator();

    vkDeviceWaitIdle(device);

    // Load the source texture. VkTex::loadFromFile ends with the
    // image in SHADER_READ_ONLY_OPTIMAL across all mips.
    sv::VkTex src;
    if (!src.loadFromFile(m_vkCtx, absPath, /*srgb=*/true)) {
        m_lastBakeStatus = std::string("texture bake: VkTex::loadFromFile failed for ") + absPath;
        return false;
    }
    if (src.image() == VK_NULL_HANDLE || src.width() == 0 || src.height() == 0) {
        src.destroy(device, alloc);
        m_lastBakeStatus = "texture bake: source image is empty";
        return false;
    }

    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();

    // ── Transition source mip 0 SHADER_READ_ONLY → TRANSFER_SRC ─
    // generateMipmaps left every mip level in SHADER_READ_ONLY, so
    // we only need to flip the layout; no cross-layout work.
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.image               = src.image();
        b.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel   = 0;
        b.subresourceRange.levelCount     = 1;
        b.subresourceRange.layerCount     = 1;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── Transition dst UNDEFINED → TRANSFER_DST ─────────────────
    sv::transitionImage(cmd, m_thumbBlitColor.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // ── vkCmdBlitImage ──────────────────────────────────────────
    // Source region = mip 0 of the loaded texture at its native
    // resolution. Destination region = full 256x256 offscreen
    // target. LINEAR filter gives a decent downsample.
    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel   = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {(int32_t)src.width(), (int32_t)src.height(), 1};

    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel   = 0;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {(int32_t)THUMB_W, (int32_t)THUMB_H, 1};

    vkCmdBlitImage(cmd,
        src.image(),              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        m_thumbBlitColor.image,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);

    // ── Transition dst → TRANSFER_SRC and copy to staging ───────
    sv::transitionImage(cmd, m_thumbBlitColor.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    const VkDeviceSize bufSize = (VkDeviceSize)THUMB_W * THUMB_H * 4;
    sv::VkBuf staging = sv::VkBuf::create(alloc, bufSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {THUMB_W, THUMB_H, 1};
    vkCmdCopyImageToBuffer(cmd, m_thumbBlitColor.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        staging.buffer, 1, &region);

    m_vkCtx.endSingleTimeCommands(cmd);

    // ── Source texture is no longer needed ──────────────────────
    src.destroy(device, alloc);

    // ── Map staging and write PNG ───────────────────────────────
    // No swizzle: m_thumbBlitColor is RGBA8_SRGB and the PNG we
    // produce is also RGBA, so the bytes go straight through.
    void* mapped = nullptr;
    vmaMapMemory(alloc, staging.allocation, &mapped);
    if (!mapped) {
        staging.destroy(alloc);
        m_lastBakeStatus = "texture bake: vmaMapMemory failed";
        return false;
    }

    std::vector<uint8_t> rgba(THUMB_W * THUMB_H * 4);
    std::memcpy(rgba.data(), mapped, rgba.size());
    vmaUnmapMemory(alloc, staging.allocation);
    staging.destroy(alloc);

    std::string thumbPath = absPath + ".thumb.png";
    int rc = stbi_write_png(thumbPath.c_str(),
                            (int)THUMB_W, (int)THUMB_H, 4,
                            rgba.data(), (int)(THUMB_W * 4));
    if (rc == 0) {
        m_lastBakeStatus = std::string("texture bake: stbi_write_png failed for ") + thumbPath;
        return false;
    }

    int64_t mtime = 0;
    {
        std::error_code ec;
        auto tt = std::filesystem::last_write_time(absPath, ec);
        if (!ec) mtime = tt.time_since_epoch().count();
    }
    m_thumbnailCache.markBaked(relPath, absPath, mtime, rgba.size());

    char buf[256];
    snprintf(buf, sizeof(buf),
             "texture bake OK: %s (%ux%u, %zu B)",
             thumbPath.c_str(), THUMB_W, THUMB_H, rgba.size());
    m_lastBakeStatus = buf;
    printf("[TestEngine] Texture thumbnail baked: %s\n", thumbPath.c_str());
    return true;
}

// ══════════════════════════════════════════════════════════════════
// golden capture path.
//
// Pipeline overview:
//   renderSkinnedMeshOffscreen()         → fills m_goldenColor + m_goldenDepth (512x512)
//     extractColorToPng(goldenColor)     → skinned_mesh.png  (SkinnedMeshPass)
//     extractDepthToPng(goldenDepth)     → shadow_pass.png   (depth-only path)
//     writePostProcessReferencePng()     → post_process.png  (CPU tonemap ref)
//   renderImGuiOverlayOffscreen()        → fills m_goldenColor with ImGui
//     extractColorToPng(goldenColor)     → imgui_layer.png   (ImGuiLayer)
//
// Determinism notes:
//  - Animation is paused and the scene UBO's time field stays at 0.0f
//    (updateSceneUBO ignores the dt argument for the fields it writes),
//    so the bake sees rest-pose bones every run.
//  - The bake's camera override is driven by VkMesh::aabb() which is a
//    pure CPU walk over the loaded vertex list — identical every run.
//  - The four PNG targets are written in a fixed order (mesh → depth →
//    post → imgui) so any failure mode is reproducible.
//
// Scope honesty:
//  - shadow_pass.png captures the depth buffer from the skinned-mesh
//    bake. This exercises the depth write path but does NOT invoke the
//    engine's ShadowPass class (which needs cascaded light matrices +
//    a shadow.vert/shadow.frag shader pair that stratumv does not yet
//    ship). A follow-up session can tighten the coverage once those
//    shaders land.
//  - post_process.png is a pure-CPU Reinhard tonemap reference applied
//    to a synthetic HDR gradient. It locks the reference tonemap math
//    so a future GPU postprocess chain can be cross-checked against it.
// ══════════════════════════════════════════════════════════════════
bool TestEngine::captureGoldens(const std::string& outDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        fprintf(stderr, "[golden] failed to create %s: %s\n",
                outDir.c_str(), ec.message().c_str());
        return false;
    }

    printf("[golden] capturing into %s\n", outDir.c_str());

    // Golden 1 + 2 + 3: render the skinned mesh once, then extract
    // three views from the same GPU state (color, depth, CPU post-process
    // reference using the same synthetic gradient every run).
    if (!renderSkinnedMeshOffscreen()) {
        fprintf(stderr, "[golden] renderSkinnedMeshOffscreen failed\n");
        return false;
    }

    const std::string meshPath  = outDir + "/skinned_mesh.png";
    const std::string depthPath = outDir + "/shadow_pass.png";
    const std::string postPath  = outDir + "/post_process.png";
    const std::string imguiPath = outDir + "/imgui_layer.png";

    if (!extractColorToPng(m_goldenColor, meshPath)) {
        fprintf(stderr, "[golden] extractColorToPng(mesh) failed\n");
        return false;
    }
    if (!extractDepthToPng(m_goldenDepth, depthPath)) {
        fprintf(stderr, "[golden] extractDepthToPng failed\n");
        return false;
    }
    if (!writePostProcessReferencePng(postPath)) {
        fprintf(stderr, "[golden] writePostProcessReferencePng failed\n");
        return false;
    }

    // Golden 4: ImGui overlay. This reuses m_goldenColor, so it MUST
    // run after the mesh color has been extracted above — otherwise
    // the ImGui draw would clobber it before the extract runs.
    if (!renderImGuiOverlayOffscreen()) {
        fprintf(stderr, "[golden] renderImGuiOverlayOffscreen failed\n");
        return false;
    }
    if (!extractColorToPng(m_goldenColor, imguiPath)) {
        fprintf(stderr, "[golden] extractColorToPng(imgui) failed\n");
        return false;
    }

    printf("[golden] wrote 4 PNGs:\n");
    printf("  %s\n  %s\n  %s\n  %s\n",
           meshPath.c_str(), depthPath.c_str(),
           postPath.c_str(), imguiPath.c_str());
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Skinned mesh bake variant for golden captures. Mirrors the first
// half of bakeThumbnail (UBO override, render-pass recording, two
// opaque/blend loops) but stops after transitioning the color image
// to TRANSFER_SRC_OPTIMAL so downstream extractors can reuse the
// GPU state for multiple PNG outputs. No PNG write, no cache mark.
// Renders into the dedicated GOLDEN_W x GOLDEN_H (512x512) targets
// so the output is legible at a human review glance — the thumbnail
// bake path keeps its 256x256 behavior unchanged.
// ──────────────────────────────────────────────────────────────────
bool TestEngine::renderSkinnedMeshOffscreen()
{
    VkDevice     device = m_vkCtx.device();
    VmaAllocator alloc  = m_vkCtx.allocator();
    (void)alloc;
    vkDeviceWaitIdle(device);

    // Force a deterministic camera using the mesh AABB (identical to
    // bakeThumbnail). In golden mode animation is paused so the bones
    // are at rest pose and the AABB is stable across runs.
    SceneUBO* uboMapped = static_cast<SceneUBO*>(
        m_uboBuffers[m_currentFrame].info.pMappedData);
    SceneUBO savedUbo{};
    if (uboMapped) {
        savedUbo = *uboMapped;
        SceneUBO thumbUbo = savedUbo;

        const sv::AABB& bounds = m_mesh.aabb();
        glm::vec3 target(0.0f, 0.95f, 0.0f);
        glm::vec3 eye   (0.0f, 1.10f, 3.20f);
        const float fovRad = glm::radians(35.0f);
        float nearZ = 0.05f;
        float farZ  = 50.0f;

        if (bounds.valid) {
            const glm::vec3 center  = bounds.center();
            const float     radius  = glm::length(bounds.extents());
            const float     padding = 1.20f;
            const float     fitDist = (radius * padding) / std::tan(fovRad * 0.5f);
            const float     distance = (fitDist > 0.0001f) ? fitDist : 3.2f;

            target = center;
            eye    = center + glm::vec3(0.0f, 0.0f, distance);
            nearZ  = (distance > 1.0f) ? distance * 0.01f : 0.001f;
            farZ   = distance + radius * 4.0f + 10.0f;
        }

        glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj = glm::perspective(fovRad, 1.0f, nearZ, farZ);
        glm::mat4 vp   = proj * view;
        thumbUbo.viewProj              = vp;
        thumbUbo.invViewProj           = glm::inverse(vp);
        thumbUbo.invViewProjUnjittered = thumbUbo.invViewProj;
        thumbUbo.cameraPos             = glm::vec4(eye, 0.0f);
        std::memcpy(uboMapped, &thumbUbo, sizeof(SceneUBO));
    }

    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();

    // Attachment transitions — golden render targets (512x512).
    sv::transitionImage(cmd, m_goldenColor.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    sv::transitionImage(cmd, m_goldenMotion.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    sv::transitionImage(cmd, m_goldenDepth.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // Dynamic rendering info — 2 color + 1 depth, matching the main
    // pipeline's attachment layout. Identical to bakeThumbnail except
    // the clear color is pinned to a stable value.
    VkRenderingAttachmentInfo colorAttach{};
    colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttach.imageView   = m_goldenColor.view;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.clearValue.color = {{0.10f, 0.12f, 0.18f, 1.0f}};

    VkRenderingAttachmentInfo motionAttach{};
    motionAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    motionAttach.imageView   = m_goldenMotion.view;
    motionAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    motionAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    motionAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    motionAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingAttachmentInfo colorAttachments[] = { colorAttach, motionAttach };

    VkRenderingAttachmentInfo depthAttach{};
    depthAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttach.imageView   = m_goldenDepth.view;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo{};
    renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderInfo.renderArea           = {{0, 0}, {GOLDEN_W, GOLDEN_H}};
    renderInfo.layerCount           = 1;
    renderInfo.colorAttachmentCount = 2;
    renderInfo.pColorAttachments    = colorAttachments;
    renderInfo.pDepthAttachment     = &depthAttach;

    vkCmdBeginRendering(cmd, &renderInfo);

    // Negative-height viewport (Y flip) matches the main pipeline.
    VkViewport viewport{0, (float)GOLDEN_H, (float)GOLDEN_W, -(float)GOLDEN_H, 0.0f, 1.0f};
    VkRect2D   scissor{{0, 0}, {GOLDEN_W, GOLDEN_H}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Opaque submeshes.
    m_skinnedPass.bind(cmd, m_descSets[m_currentFrame],
                       m_animSys.bonePaletteDescSet());
    const auto& submeshes = m_mesh.submeshes();
    const auto& materials = m_mesh.materials();
    for (size_t i = 0; i < submeshes.size(); i++) {
        const auto& sub = submeshes[i];
        bool isBlend = (sub.materialIndex >= 0
                        && sub.materialIndex < (int)materials.size()
                        && materials[sub.materialIndex].blendMode == sv::BlendMode::AlphaBlend);
        if (isBlend) continue;

        sv::SkinnedDrawCmd dc{};
        dc.vertexBuffer = m_mesh.vertexBuffer();
        dc.indexBuffer  = m_mesh.indexBuffer();
        dc.indexCount   = sub.indexCount;
        dc.firstIndex   = sub.indexOffset;
        dc.model        = glm::mat4(1.0f);
        dc.boneOffset   = m_boneOffset;
        dc.materialSet  = (i < m_materialSets.size()) ? m_materialSets[i].set : VK_NULL_HANDLE;
        m_skinnedPass.draw(cmd, dc);
    }

    // Blend submeshes.
    m_skinnedPass.bindBlend(cmd, m_descSets[m_currentFrame],
                            m_animSys.bonePaletteDescSet());
    for (size_t i = 0; i < submeshes.size(); i++) {
        const auto& sub = submeshes[i];
        bool isBlend = (sub.materialIndex >= 0
                        && sub.materialIndex < (int)materials.size()
                        && materials[sub.materialIndex].blendMode == sv::BlendMode::AlphaBlend);
        if (!isBlend) continue;

        sv::SkinnedDrawCmd dc{};
        dc.vertexBuffer = m_mesh.vertexBuffer();
        dc.indexBuffer  = m_mesh.indexBuffer();
        dc.indexCount   = sub.indexCount;
        dc.firstIndex   = sub.indexOffset;
        dc.model        = glm::mat4(1.0f);
        dc.boneOffset   = m_boneOffset;
        dc.materialSet  = (i < m_materialSets.size()) ? m_materialSets[i].set : VK_NULL_HANDLE;
        m_skinnedPass.draw(cmd, dc, /*alphaBlend=*/true);
    }

    vkCmdEndRendering(cmd);

    // Transition both golden attachments to TRANSFER_SRC so
    // extractColorToPng / extractDepthToPng can copy without
    // re-recording.
    sv::transitionImage(cmd, m_goldenColor.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    sv::transitionImage(cmd, m_goldenDepth.image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    m_vkCtx.endSingleTimeCommands(cmd);

    if (uboMapped) {
        std::memcpy(uboMapped, &savedUbo, sizeof(SceneUBO));
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Render a fixed ImGui window layout into m_thumbColor, leaving the
// image in TRANSFER_SRC_OPTIMAL ready for extractColorToPng. Uses the
// same ImGuiLayer that the interactive harness uses, just with a
// locked-down widget tree so captures are reproducible.
// ──────────────────────────────────────────────────────────────────
bool TestEngine::renderImGuiOverlayOffscreen()
{
    VkDevice device = m_vkCtx.device();
    vkDeviceWaitIdle(device);

    // Build a fresh ImGui frame with ONLY the golden widgets.
    m_imguiLayer.newFrame();
    buildGoldenImGuiFrame();
    ImGui::Render();

    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();

    // Previous renderSkinnedMeshOffscreen left m_goldenColor in
    // TRANSFER_SRC_OPTIMAL. Transition back to COLOR_ATTACHMENT_OPTIMAL
    // so ImGuiLayer::render can treat it as a color target.
    sv::transitionImage(cmd, m_goldenColor.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // ImGuiLayer::render uses LOAD_OP_LOAD internally (it paints
    // over whatever is in the swapchain), so we have to clear the
    // target ourselves first — otherwise the previous mesh bake
    // bleeds through behind the ImGui panel and the golden is
    // flaky. A one-shot empty render pass with LOAD_OP_CLEAR is
    // the cheapest way to wipe the attachment.
    {
        VkRenderingAttachmentInfo clearAttach{};
        clearAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        clearAttach.imageView   = m_goldenColor.view;
        clearAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        clearAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        clearAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        clearAttach.clearValue.color = {{0.08f, 0.09f, 0.13f, 1.0f}};

        VkRenderingInfo clearInfo{};
        clearInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        clearInfo.renderArea           = {{0, 0}, {GOLDEN_W, GOLDEN_H}};
        clearInfo.layerCount           = 1;
        clearInfo.colorAttachmentCount = 1;
        clearInfo.pColorAttachments    = &clearAttach;

        vkCmdBeginRendering(cmd, &clearInfo);
        vkCmdEndRendering(cmd);
    }

    // Now the target is a known solid color — ImGuiLayer::render
    // will LOAD_OP_LOAD it and composite the panel on top.
    m_imguiLayer.render(cmd, m_goldenColor.view, GOLDEN_W, GOLDEN_H);

    // Transition to TRANSFER_SRC for the extract step.
    sv::transitionImage(cmd, m_goldenColor.image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    m_vkCtx.endSingleTimeCommands(cmd);
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Fixed ImGui widget tree for the imgui_layer.png golden. Everything
// here must stay deterministic — no live clocks, no frame counters,
// no window-position randomness from ImGui::Begin "first use" state.
// ──────────────────────────────────────────────────────────────────
void TestEngine::buildGoldenImGuiFrame()
{
    // Lock window position + size so layout doesn't drift. Sized
    // for the 512x512 golden target — leaves a 16-pixel margin on
    // all sides so window rounding is visible.
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(480.0f, 480.0f), ImGuiCond_Always);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoMove      |
        ImGuiWindowFlags_NoResize    |
        ImGuiWindowFlags_NoCollapse  |
        ImGuiWindowFlags_NoTitleBar  |
        ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##GoldenFrame", nullptr, kFlags)) {
        ImGui::Text("StratumV golden");
        ImGui::Separator();
        ImGui::Text("SkinnedMeshPass");
        ImGui::Text("ShadowPass");
        ImGui::Text("PostProcess");
        ImGui::Text("ImGuiLayer");
        ImGui::Separator();
        ImGui::ProgressBar(0.5f, ImVec2(-1.0f, 0.0f), "50%");
        ImGui::ProgressBar(0.75f, ImVec2(-1.0f, 0.0f), "75%");
    }
    ImGui::End();
}

// ──────────────────────────────────────────────────────────────────
// Copy a color image (swapchain format, assumed BGRA8-like) to a
// staging buffer, swizzle to RGBA, and write a PNG at outPath.
// Source MUST be in TRANSFER_SRC_OPTIMAL.
// ──────────────────────────────────────────────────────────────────
bool TestEngine::extractColorToPng(sv::ColorImage& src, const std::string& outPath)
{
    VmaAllocator alloc = m_vkCtx.allocator();

    // Size is always the golden dimensions — these extract helpers
    // are only called from the golden capture path (the asset
    // thumbnail bake uses its own inline code).
    const uint32_t     W        = GOLDEN_W;
    const uint32_t     H        = GOLDEN_H;
    const VkDeviceSize bufSize  = (VkDeviceSize)W * H * 4;
    sv::VkBuf staging = sv::VkBuf::create(alloc, bufSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, src.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        staging.buffer, 1, &region);
    m_vkCtx.endSingleTimeCommands(cmd);

    void* mapped = nullptr;
    vmaMapMemory(alloc, staging.allocation, &mapped);
    if (!mapped) {
        staging.destroy(alloc);
        return false;
    }

    std::vector<uint8_t> rgba((size_t)W * H * 4);
    const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        rgba[i * 4 + 0] = pixels[i * 4 + 2]; // R ← B
        rgba[i * 4 + 1] = pixels[i * 4 + 1]; // G
        rgba[i * 4 + 2] = pixels[i * 4 + 0]; // B ← R
        rgba[i * 4 + 3] = pixels[i * 4 + 3]; // A
    }
    vmaUnmapMemory(alloc, staging.allocation);
    staging.destroy(alloc);

    int rc = stbi_write_png(outPath.c_str(),
                            (int)W, (int)H, 4,
                            rgba.data(), (int)(W * 4));
    if (rc == 0) {
        fprintf(stderr, "[golden] stbi_write_png failed: %s\n", outPath.c_str());
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Copy D32_SFLOAT depth → staging → linearized grayscale PNG.
// ──────────────────────────────────────────────────────────────────
bool TestEngine::extractDepthToPng(sv::DepthImage& src, const std::string& outPath)
{
    VmaAllocator alloc = m_vkCtx.allocator();

    const uint32_t     W       = GOLDEN_W;
    const uint32_t     H       = GOLDEN_H;
    const VkDeviceSize bufSize = (VkDeviceSize)W * H * sizeof(float);
    sv::VkBuf staging = sv::VkBuf::create(alloc, bufSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, src.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        staging.buffer, 1, &region);
    m_vkCtx.endSingleTimeCommands(cmd);

    void* mapped = nullptr;
    vmaMapMemory(alloc, staging.allocation, &mapped);
    if (!mapped) {
        staging.destroy(alloc);
        return false;
    }

    std::vector<uint8_t> rgba((size_t)W * H * 4);
    const float* depthPixels = static_cast<const float*>(mapped);
    // Non-linear perspective depth clusters mesh values in ~[0.988,
    // 0.995] for the AABB-fit camera, so a plain `1 - d` mapping
    // gives nearly-pure-black output (values 1-12 / 255) and the
    // silhouette is invisible to the naked eye. We remap via a
    // `pow(1 - d, 0.15)` tone curve: keeps background at 0 (1-d==0
    // still evaluates to 0 under a positive exponent) and stretches
    // the narrow mesh band into the [100, 200] visible grey range.
    // Deterministic — no per-image min/max, no scene-dependent
    // normalization — so the golden stays stable across runs.
    for (size_t i = 0; i < (size_t)W * H; i++) {
        float d = depthPixels[i];
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        const float inv    = 1.0f - d;
        const float curved = (inv > 0.0f) ? std::pow(inv, 0.15f) : 0.0f;
        const uint8_t g    = (uint8_t)(curved * 255.0f + 0.5f);
        rgba[i * 4 + 0] = g;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = g;
        rgba[i * 4 + 3] = 255;
    }
    vmaUnmapMemory(alloc, staging.allocation);
    staging.destroy(alloc);

    int rc = stbi_write_png(outPath.c_str(),
                            (int)W, (int)H, 4,
                            rgba.data(), (int)(W * 4));
    if (rc == 0) {
        fprintf(stderr, "[golden] stbi_write_png failed: %s\n", outPath.c_str());
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────
// CPU reference for the PostProcess chain. Synthesize a deterministic
// HDR gradient, apply Reinhard + gamma 2.2, encode as PNG. This is the
// locked reference that a future GPU postprocess chain can be diffed
// against. Using a procedural input means every machine produces the
// same PNG, so the golden doesn't depend on GPU driver quirks.
// ──────────────────────────────────────────────────────────────────
bool TestEngine::writePostProcessReferencePng(const std::string& outPath)
{
    const uint32_t W = GOLDEN_W;
    const uint32_t H = GOLDEN_H;
    std::vector<uint8_t> rgba((size_t)W * H * 4);

    // Reinhard tonemap: L / (1 + L). Then gamma-correct to sRGB-ish.
    // Input is a 2D gradient with deliberate HDR peaks so the
    // tonemap compresses them into the [0,1] range visibly.
    const float invW = 1.0f / (float)(W - 1);
    const float invH = 1.0f / (float)(H - 1);
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            const float u = x * invW;
            const float v = y * invH;
            // HDR input: bright gradient with a hotspot near (0.75, 0.25).
            const float dx = u - 0.75f;
            const float dy = v - 0.25f;
            const float r2 = dx * dx + dy * dy;
            const float hot = 4.5f * std::exp(-r2 * 20.0f);
            const float hdrR = u * 2.0f + hot;
            const float hdrG = v * 1.5f + hot * 0.8f;
            const float hdrB = (1.0f - u) * 1.0f + hot * 0.4f;

            // Reinhard.
            const float tmR = hdrR / (1.0f + hdrR);
            const float tmG = hdrG / (1.0f + hdrG);
            const float tmB = hdrB / (1.0f + hdrB);

            // Gamma 2.2.
            const float gR = std::pow(tmR, 1.0f / 2.2f);
            const float gG = std::pow(tmG, 1.0f / 2.2f);
            const float gB = std::pow(tmB, 1.0f / 2.2f);

            auto to8 = [](float f) {
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                return (uint8_t)(f * 255.0f + 0.5f);
            };
            const size_t i = ((size_t)y * W + x) * 4;
            rgba[i + 0] = to8(gR);
            rgba[i + 1] = to8(gG);
            rgba[i + 2] = to8(gB);
            rgba[i + 3] = 255;
        }
    }

    int rc = stbi_write_png(outPath.c_str(),
                            (int)W, (int)H, 4,
                            rgba.data(), (int)(W * 4));
    if (rc == 0) {
        fprintf(stderr, "[golden] stbi_write_png failed: %s\n", outPath.c_str());
        return false;
    }
    return true;
}

// ══════════════════════════════════════════════════════════════════
// route a bake request to the right path based on AssetKind.
// Mesh-like assets run through the full skinned-mesh offscreen render
// pass (bakeThumbnail). Texture assets use the cheaper vkCmdBlit path
// (bakeTextureThumbnail). Unknown kinds currently fall back to the
// mesh path because that's what was there before — future kinds
// (materials, etc.) can opt in individually.
// ══════════════════════════════════════════════════════════════════
bool TestEngine::dispatchBake(const std::string& relPath,
                              const std::string& absPath)
{
    const sv::AssetBrowserEntry* entry = m_assetBrowser.findByRelativePath(relPath);
    if (entry && entry->kind == sv::AssetKind::Texture) {
        return bakeTextureThumbnail(relPath, absPath);
    }
    return bakeThumbnail(relPath, absPath);
}

// ══════════════════════════════════════════════════════════════════
bool TestEngine::onFrame(float dt)
{
    // stamp CPU frame start (wall-clock) so we can measure
    // CPU-side work in ms, independent of GPU/present wait time.
    const double cpuStartSec = glfwGetTime();

    m_totalTime += dt;
    m_window.pollEvents();

    if (m_window.shouldClose() ||
        glfwGetKey(m_window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS)
        return false;

    m_camera.update(m_window.handle(), dt);

    VkDevice device = m_vkCtx.device();
    auto& frame = m_frames[m_currentFrame];

    // Wait for previous frame
    vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

    // Acquire swapchain image
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(device, m_swapchain.swapchain(),
                                            UINT64_MAX, frame.imageAvailable,
                                            VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        return true;

    vkResetFences(device, 1, &frame.inFlight);
    vkResetCommandBuffer(frame.cmd, 0);

    // ── Animation update (/ ) ─────────────────────────
    float animDt = m_paused ? 0.0f : dt;
    m_animSys.resetBonePalette();

    if (m_useBlendTree && m_blendSpace) {
        // blend tree path
        m_blendSpace->setParameter(m_blendParam);

        if (m_enablePartialBlend && !m_upperBodyMask.empty() && m_upperClipNode) {
            // Two body layers: full body blend space + upper body clip overlay
            sv::AnimBodyLayer layers[2];
            layers[0].node   = m_blendSpace.get();
            layers[0].weight = 1.0f;

            layers[1].node         = m_upperClipNode.get();
            layers[1].weight       = m_upperLayerWeight;
            layers[1].jointWeights = m_upperBodyMask.data();

            m_animSys.blendBodyLayers(m_animInst, layers, 2, animDt);
        } else {
            // Single body layer: blend space only
            sv::AnimBodyLayer layer;
            layer.node   = m_blendSpace.get();
            layer.weight = 1.0f;
            m_animSys.blendBodyLayers(m_animInst, &layer, 1, animDt);
        }
    } else if (m_stateMachine) {
        // state machine path
        if (!m_paused)
            m_stateMachine->update(dt);
        sv::BlendLayer layers[2];
        int layerCount = m_stateMachine->getBlendLayers(layers, 2);
        m_animSys.blend(m_animInst, layers, layerCount);
    } else {
        m_animSys.blend(m_animInst, nullptr, 0); // rest pose (T-pose fallback)
    }
    // Root motion extraction — before L2M
    if (m_rootMotionEnabled) {
        m_lastRootDelta = m_animSys.extractRootMotion(m_animInst, m_prevRootPos, m_prevRootRot);
        m_rootMotionAccum += m_lastRootDelta.deltaPosition;
    }

    m_animSys.computeSkinningMatrices(m_animInst, m_mesh.skeleton());

    // IK post-processing — after L2M, before upload
    {
        sv::TwoBoneIKSlot twoBone[2];
        int tbCount = 0;
        if (m_enableTwoBoneIK) {
            if (m_leftLegIK.startJoint >= 0) {
                m_leftLegIK.target = glm::vec3(-m_ikTargetSpreadX, m_ikTargetY, 0.0f);
                m_leftLegIK.weight = 1.0f;
                twoBone[tbCount++] = m_leftLegIK;
            }
            if (m_rightLegIK.startJoint >= 0) {
                m_rightLegIK.target = glm::vec3(m_ikTargetSpreadX, m_ikTargetY, 0.0f);
                m_rightLegIK.weight = 1.0f;
                twoBone[tbCount++] = m_rightLegIK;
            }
        }

        sv::AimIKSlot aim[1];
        int aimCount = 0;
        if (m_enableAimIK && m_headAimIK.joint >= 0) {
            m_headAimIK.target = m_aimTarget;
            m_headAimIK.weight = 1.0f;
            aim[aimCount++] = m_headAimIK;
        }

        if (tbCount > 0 || aimCount > 0) {
            m_animSys.applyIK(m_animInst, m_mesh.skeleton(),
                              twoBone, tbCount, aim, aimCount);
        }
    }

    m_boneOffset = m_animSys.uploadBones(m_animInst);
    m_animSys.flushBonePalette(device, m_vkCtx.allocator());

    // ── Scene UBO ─────────────────────────────────────────────────
    updateSceneUBO(dt);

    // ── drain replication datagrams ───────────────────
    // Runs before drawDebugUI so the ImGui panel shows the very
    // latest decoded snapshot this frame.
    drainNetReliableInbox(); // Spawn/Despawn first
    drainAssetInbox();       // Announce/Chunk/Ack
    drainNetInbox();         // snapshot datagrams

    // ── editor bridge pump ───────────────────────
    // Start the listener the first time we're welcomed, flush every
    // entity state out to connected bridge clients, and translate any
    // inbound Blender edits into SetField transactions for the server.
    maybeStartEditorBridge();
    if (m_editorBridge && m_editorBridge->running()) {
        for (const auto& kv : m_netEntities) {
            publishEntityToBridge(kv.first);
        }
        pumpBridgeMoves();
        pumpBridgeAssets();
        pumpBridgeParents();
        pumpBridgeLights();
        pumpBridgeCameras();
        pumpBridgeMaterials();
    }

    // ── ImGui frame ───────────────────────────────────────────────
    m_imguiLayer.newFrame();
    drawDebugUI();
    drawAssetPanels();
    drawReplicatedAssetsPanel();
    drawNetworkDemoPanel();

    // Help overlay: F1 toggles it; --show-help opens it at startup.
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
        m_helpOverlay.toggle();
    m_helpOverlay.draw();

    // handle pending bake before recording the main frame.
    // bakeThumbnail() drains the queue itself, so it is safe to call
    // here — the main render below resubmits cleanly afterwards.
    // dispatchBake routes Textures through the cheaper
    // vkCmdBlit path.
    if (m_bakeRequested) {
        dispatchBake(m_bakeRelPath, m_bakeAbsPath);
        m_bakeRequested = false;
    }

    // ── Record + submit ───────────────────────────────────────────
    recordFrame(imageIndex);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &frame.imageAvailable;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &frame.cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &m_renderFinishedPerImage[imageIndex];
    vkQueueSubmit(m_vkCtx.graphicsQueue(), 1, &submitInfo, frame.inFlight);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &m_renderFinishedPerImage[imageIndex];
    presentInfo.swapchainCount     = 1;
    VkSwapchainKHR sc = m_swapchain.swapchain();
    presentInfo.pSwapchains        = &sc;
    presentInfo.pImageIndices      = &imageIndex;
    vkQueuePresentKHR(m_vkCtx.presentQueue(), &presentInfo);

    // auto-bake the first browser mesh at frame 5 so the
    // visual checkpoint screenshot shows a populated cache. Only fires
    // when invoked with `--auto-bake`.
    if (m_autoBake && m_frameCount == 5 && m_browserScanned &&
        m_dropAssetRelPath.empty()) {
        for (const auto& e : m_assetBrowser.entries()) {
            if (e.kind == sv::AssetKind::Mesh) {
                m_dropAssetRelPath = e.relativePath;
                m_dropAssetAbsPath = e.absolutePath;
                m_bakeRequested    = true;
                m_bakeRelPath      = e.relativePath;
                m_bakeAbsPath      = e.absolutePath;
                break;
            }
        }
    }

    // additionally auto-bake the first browser texture at
    // frame 8 so the visual checkpoint exercises the vkCmdBlit path.
    if (m_autoBake && m_frameCount == 8 && m_browserScanned) {
        for (const auto& e : m_assetBrowser.entries()) {
            if (e.kind == sv::AssetKind::Texture) {
                m_bakeRequested = true;
                m_bakeRelPath   = e.relativePath;
                m_bakeAbsPath   = e.absolutePath;
                break;
            }
        }
    }

    // One-shot framebuffer capture after a few frames.
    // bumped from 10 → 15 so the screenshot is taken
    // AFTER both auto-bakes (mesh at frame 6, texture at frame 9).
    // drive the auto-upload flow for the
    // s_edit2b_checkpoint.png capture rig. Runs once at
    // m_autoUploadFrame, which defaults to 60 (~1 s at 60 fps) —
    // well before the networked capture window at frame 300 so
    // both clients have time to observe the receive-side state.
    if (!m_autoUploadRelPath.empty() && !m_autoUploadDone &&
        m_frameCount >= m_autoUploadFrame) {
        bool ready = false;
        {
            std::lock_guard<std::mutex> lk(m_netStateMu);
            ready = m_netConnected && m_netWelcomed &&
                    m_netScope >= sv::PermissionScope::Editor;
        }
        if (ready) {
            const auto* entry =
                m_assetBrowser.findByRelativePath(m_autoUploadRelPath);
            if (entry) {
                uploadAssetFromDisk(entry->relativePath, entry->absolutePath);
                m_autoUploadDone = true;
            } else {
                printf("[TestEngine] --auto-upload: '%s' not in browser\n",
                       m_autoUploadRelPath.c_str());
                m_autoUploadDone = true;
            }
        }
    }

    // networked runs need time for the reliable-stream
    // welcome + spawn transactions + first few datagram snapshots
    // to arrive before the capture triggers. When --connect is set
    // we delay the capture to frame 300 (~5 s at 60fps) so a second
    // late-joining client has time to complete its handshake and
    // for its avatar Spawn transaction to reach this instance.
    // Offline runs still snap at frame 15 so /
    // checks keep their fast timings.
    const uint32_t captureFrame =
        m_captureFrameOverride > 0
            ? m_captureFrameOverride
            : (m_netConnectTarget.empty() ? 15u : 300u);
    if (m_frameCount == captureFrame && !m_captured) {
        vkQueueWaitIdle(m_vkCtx.graphicsQueue());
        captureFramebuffer(imageIndex);
        m_captured = true;
    }

    // render-golden mode. A few warm-up frames are
    // enough for the animation system + bone palette SSBO to have
    // been populated at least once via the paused resetBonePalette/
    // blend/uploadBones path above. We deliberately capture AFTER
    // submitting the main (buggy) render so the rest of the frame
    // matches interactive runs — the capture path itself uses the
    // known-good offscreen bake targets, not the swapchain.
    if (m_renderGoldensMode && !m_goldensCaptured && m_frameCount == 3) {
        vkQueueWaitIdle(m_vkCtx.graphicsQueue());
        bool ok = captureGoldens(m_goldenOutDir);
        m_goldensCaptured = true;
        fprintf(stdout, "[golden] captureGoldens: %s\n", ok ? "OK" : "FAILED");
        return false; // exit immediately; skip the per-frame wrap-up
    }

    // Auto-exit a few frames after the capture (visual checkpoint mode).
    // dump collected perf stats to stdout before exit so the
    // PERF_BASELINE.md doc gets a reproducible frametime number from
    // a --auto-exit run.
    // Exit window extended for networked runs to match the
    // bumped capture frame — offline runs still exit at 26.
    const uint32_t exitFrame =
        m_captureFrameOverride > 0
            ? m_captureFrameOverride + 11u
            : (m_netConnectTarget.empty() ? 26u : 311u);
    if (m_autoExit && m_captured && m_frameCount > exitFrame) {
        fprintf(stdout,
                " frames=%u frameTime=%.3f ms avgFps=%.1f "
                "cpu=%.3f ms  draws=%u  tris=%u  vram=%.1f/%.1f MB\n",
                m_frameCount, m_perfContext.frameTimeMs,
                m_perfContext.avgFps, m_perfContext.cpuFrameTimeMs,
                m_perfContext.drawCallCount, m_perfContext.triangleCount,
                m_perfContext.vramUsedMB, m_perfContext.vramBudgetMB);
        return false;
    }

    // ── update live PerformanceContext ────────────────────
    // Frame time + EMA-smoothed fps; draw / triangle counts from
    // recordFrame; VRAM from VkCtx heap budget. gpuFrameTimeMs is
    // left zero in the lab harness until GpuProfiler is wired in a
    // follow-up session (the lab harness never enabled the profiler
    // because the main-pass render has a pre-existing bug).
    const float cpuEndSec = static_cast<float>(glfwGetTime());
    const float cpuMs     = (cpuEndSec - static_cast<float>(cpuStartSec)) * 1000.0f;
    const float frameMs   = dt * 1000.0f;

    m_perfContext.frameTimeMs    = frameMs;
    m_perfContext.cpuFrameTimeMs = cpuMs;
    m_perfContext.gpuFrameTimeMs = 0.0f; // profiler not enabled in lab harness

    // True running average — frames / total time. More stable than an
    // EMA across the lab harness's short --auto-exit run, where the
    // first few frames are dominated by init costs and skew an EMA
    // for many frames before settling. m_totalTime is updated at the
    // top of onFrame above.
    if (m_totalTime > 0.0f)
        m_perfContext.avgFps = static_cast<float>(m_frameCount + 1) / m_totalTime;

    m_perfContext.drawCallCount = m_drawsThisFrame;
    m_perfContext.triangleCount = m_trisThisFrame;

    // VRAM: device-local heap usage vs budget from VMA
    const auto heap           = m_vkCtx.getDeviceLocalBudget();
    m_perfContext.vramUsedMB   = heap.usageBytes  / (1024.0f * 1024.0f);
    m_perfContext.vramBudgetMB = heap.budgetBytes / (1024.0f * 1024.0f);

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
    m_frameCount++;
    return true;
}

// ══════════════════════════════════════════════════════════════════
void TestEngine::shutdownNetworking()
{
    // tear down the editor bridge before the QUIC
    // connection so the bridge cannot see a half-destroyed net state
    // on the main thread. stop() is blocking and joins all internal
    // threads, so control is fully back before we drop the pointer.
    if (m_editorBridge) {
        m_editorBridge->stop();
        m_editorBridge.reset();
        m_editorBridgeStarted = false;
    }

    // Destruction order matters (RegistrationClose blocks on live child handles):
    //   Connection  ->  Transport.stop()  ->  Transport destroyed
    // If Transport is destroyed before Connection, RegistrationClose
    // blocks forever on the live child handle.
    if (m_netConn.valid()) {
        m_netConn.shutdown(0);
        m_netConn.waitForShutdownComplete(500);
    }
    // Clearing setDatagramHandler with nullptr drops the lambda's
    // `this` capture, making the MsQuic worker thread a no-op even
    // if one more event is in flight.
    m_netConn.setDatagramHandler(nullptr);

    // Explicitly drop the Connection BEFORE stopping the transport.
    // Move-assigning a default Connection over m_netConn frees the
    // inner Impl and runs ConnectionClose.
    m_netConn = sv::net::Connection{};

    if (m_netTransport) {
        m_netTransport->stop();
        m_netTransport.reset();
    }
    m_netConnected = false;
}

void TestEngine::onShutdown()
{
    // Tear down networking first — the MsQuic registration must be
    // closed before anything else, and the Connection/Transport do
    // not depend on the Vulkan device.
    shutdownNetworking();

    VkDevice     device = m_vkCtx.device();
    VmaAllocator alloc  = m_vkCtx.allocator();
    vkDeviceWaitIdle(device);

    m_imguiLayer.shutdown(device);
    m_skinnedPass.destroy();
    for (auto& ms : m_materialSets)
        m_materialPipeline.destroyMaterialSet(alloc, ms);
    m_materialSets.clear();
    m_materialPipeline.destroy(m_vkCtx);

    // persist VkPipelineCache and release the handle before
    // the logical device is destroyed. The save failure path (e.g.
    // read-only volume) is already logged inside PipelineCache::save.
    m_pipelineCache.save(device, m_pipelineCachePath);
    m_pipelineCache.destroy(device);
    m_animSys.destroy();
    m_mesh.destroy(alloc);

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        m_uboBuffers[i].destroy(alloc);
        vkDestroySemaphore(device, m_frames[i].imageAvailable, nullptr);
        vkDestroyFence(device, m_frames[i].inFlight, nullptr);
    }
    for (auto sem : m_renderFinishedPerImage)
        vkDestroySemaphore(device, sem, nullptr);
    m_renderFinishedPerImage.clear();

    vkDestroyDescriptorPool(device, m_descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_sceneDescLayout, nullptr);

    // offscreen thumbnail bake targets
    m_thumbColor.destroy(device, alloc);
    m_thumbMotion.destroy(device, alloc);
    m_thumbDepth.destroy(device, alloc);
    m_thumbBlitColor.destroy(device, alloc);

    // golden capture targets (no-ops if never allocated,
    // because sv::ColorImage::destroy / sv::DepthImage::destroy are
    // safe on VK_NULL_HANDLE members).
    m_goldenColor.destroy(device, alloc);
    m_goldenMotion.destroy(device, alloc);
    m_goldenDepth.destroy(device, alloc);

    m_motionVec.destroy(device, alloc);
    m_depth.destroy(device, alloc);

    sv::VkShader::shutdownCompiler();
    m_swapchain.shutdown(device);
    m_vkCtx.shutdown();
    m_window.shutdown();

    printf("[TestEngine] Shutdown complete\n");
}

// ──────────────────────────────────────────────────────────────────
void TestEngine::captureFramebuffer(uint32_t imageIndex)
{
    VmaAllocator alloc  = m_vkCtx.allocator();
    auto ext = m_swapchain.extent();
    uint32_t w = ext.width, h = ext.height;

    // Create staging buffer
    VkDeviceSize bufSize = (VkDeviceSize)w * h * 4;
    sv::VkBuf staging = sv::VkBuf::create(alloc, bufSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

    // Copy swapchain image → staging buffer
    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();

    sv::transitionImage(cmd, m_swapchain.image(imageIndex),
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        0, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, m_swapchain.image(imageIndex),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);

    sv::transitionImage(cmd, m_swapchain.image(imageIndex),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_TRANSFER_READ_BIT, 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    m_vkCtx.endSingleTimeCommands(cmd);

    // Read pixels and write PPM
    void* mapped = nullptr;
    vmaMapMemory(alloc, staging.allocation, &mapped);
    if (mapped) {
        const std::string path =
            m_captureName.empty() ? std::string("capture.ppm") : m_captureName;
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << w << " " << h << "\n255\n";
        const uint8_t* pixels = (const uint8_t*)mapped;
        // Swapchain is B8G8R8A8 — swap B and R for PPM (RGB)
        for (uint32_t i = 0; i < w * h; i++) {
            uint8_t rgb[3] = { pixels[i*4+2], pixels[i*4+1], pixels[i*4+0] };
            out.write((const char*)rgb, 3);
        }
        out.close();
        printf("[TestEngine] Captured framebuffer to %s (%ux%u)\n",
               path.c_str(), w, h);
    }
    vmaUnmapMemory(alloc, staging.allocation);
    staging.destroy(alloc);
}

// ══════════════════════════════════════════════════════════════════
int main(int argc, char** argv)
{
    // force-link ParentLink.obj into the exe.
    // Same static-archive dead-strip concern as NetTransform below.
    (void)sv::ensureParentLinkRegistered();
    // force-link LightComponent.obj into the exe.
    // Same reason — and the anchor ALSO re-applies setAuthority so
    // the Editor tag survives repeated re-registration.
    (void)sv::ensureLightComponentRegistered();
    // force-link CameraComponent + MaterialComponent.
    (void)sv::ensureCameraComponentRegistered();
    (void)sv::ensureMaterialComponentRegistered();
    // force-link NetTransform.obj into the exe.
    // stratumv.lib is a static archive; without this explicit call
    // the SV_REPLICATE file-scope initializer in NetTransform.cpp
    // gets dead-stripped and ReplicationRegistry::find("NetTransform")
    // returns null. The return value is intentionally ignored — the
    // call itself is the observable side effect. See NetTransform.h.
    (void)sv::ensureNetTransformRegistered();

    TestEngine engine;
    // visual-checkpoint flags consumed BEFORE EngineBase::run.
    // --render-golden <dir> puts the harness into a
    // deterministic capture mode and exits after writing 4 PNGs.
    // --connect host:port turns the harness into a
    // replication client of a running stratumv_server. The server's
    // default port is 9001, so a typical invocation is
    //   skinned_test --connect 127.0.0.1:9001
    // Public setters live on TestEngine via the new helper accessors.
    bool wantGolden = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--auto-bake") == 0) engine.setAutoBake(true);
        if (strcmp(argv[i], "--auto-exit") == 0) engine.setAutoExit(true);
        if (strcmp(argv[i], "--show-help") == 0) engine.setShowHelp(true);
        if (strcmp(argv[i], "--render-golden") == 0 && i + 1 < argc) {
            engine.setRenderGoldensMode(argv[++i]);
            wantGolden = true;
        }
        if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            engine.setNetworkTarget(argv[++i]);
        }
        if (strcmp(argv[i], "--net-tick-hz") == 0 && i + 1 < argc) {
            engine.setNetworkTickHz(static_cast<uint32_t>(std::atoi(argv[++i])));
        }
        // auto-upload a specific asset at a specific frame.
        //   --auto-upload-path textures/grass_albedo.png
        //   --auto-upload-frame 80
        if (strcmp(argv[i], "--auto-upload-path") == 0 && i + 1 < argc) {
            engine.setAutoUploadPath(argv[++i]);
        }
        if (strcmp(argv[i], "--auto-upload-frame") == 0 && i + 1 < argc) {
            engine.setAutoUploadFrame(static_cast<uint32_t>(std::atoi(argv[++i])));
        }
        if (strcmp(argv[i], "--capture-name") == 0 && i + 1 < argc) {
            engine.setCaptureName(argv[++i]);
        }
        if (strcmp(argv[i], "--capture-frame") == 0 && i + 1 < argc) {
            engine.setCaptureFrame(static_cast<uint32_t>(std::atoi(argv[++i])));
        }
        // optional plain-TCP editor bridge port. 0 = off.
        if (strcmp(argv[i], "--editor-bridge-port") == 0 && i + 1 < argc) {
            engine.setEditorBridgePort(
                static_cast<uint16_t>(std::atoi(argv[++i])));
        }
    }
    // Golden-capture mode renders a known skinned mesh. If that input
    // (ASSET_PATH) is absent — e.g. a public checkout that doesn't ship
    // the binary asset — exit with CTest's skip code instead of failing
    // hard in init, so the render-regression suite reports "Skipped"
    // rather than a spurious failure. See SKIP_RETURN_CODE in
    // tests/CMakeLists.txt.
    if (wantGolden) {
        if (FILE* f = sv::FOpen(ASSET_PATH, "rb")) {
            fclose(f);
        } else {
            fprintf(stderr,
                    "[golden] SKIP: input mesh '%s' not found — "
                    "golden capture skipped (exit 125)\n", ASSET_PATH);
            return 125;
        }
    }
    return engine.run(argc, argv);
}
