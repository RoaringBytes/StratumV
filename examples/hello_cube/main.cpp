// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
//
// hello_cube — the minimal StratumV example.
//
// Opens a window and renders a spinning, per-face-colored cube through the
// engine's public API. No asset files, no skinning, no ImGui in the example
// itself. This is the canonical "how do I drive the engine" reference and the
// model for a game's EngineBase::onInit().
//
// Run interactively:   hello_cube
// Render one frame to a PNG and exit (no window needed for the image):
//                      hello_cube --headless out.png
//
// Build (from the repo root):
//   cmake -B build -G "Visual Studio 17 2022" -A x64 -DSTRATUMV_BUILD_EXAMPLES=ON
//   cmake --build build --config Release --target hello_cube
//   ./build/examples/hello_cube/Release/hello_cube   (run from its own dir)

#include <engine/EngineBase.h>
#include <engine/Window.h>
#include <engine/vk/VkContext.h>
#include <engine/vk/VkSwapchain.h>
#include <engine/vk/VkBuffer.h>
#include <engine/vk/VkTexture.h>
#include <engine/vk/VkPipeline.h>
#include <engine/vk/VkShader.h>
#include <engine/Types.h>            // sv::transitionImage, sv::FrameSync

#include <volk.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "stb_image_write.h"         // declaration only; impl lives in stratumv.lib

#include <array>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

namespace {

struct CubeVertex { glm::vec3 pos; glm::vec3 color; };
struct CubeUBO    { glm::mat4 viewProj; glm::mat4 model; };   // distinct from sv::SceneUBO

constexpr uint32_t MAX_FRAMES   = 2;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

// 8 corners, 6 faces (4 verts each) with a flat per-face color.
std::vector<CubeVertex> buildCubeVerts() {
    const glm::vec3 c[8] = {
        {-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f}, // back  (z-)
        {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, // front (z+)
    };
    struct Face { int i[4]; glm::vec3 col; };
    const Face faces[6] = {
        {{4,5,6,7}, {0.90f,0.30f,0.30f}}, // +Z  red
        {{1,0,3,2}, {0.30f,0.70f,0.90f}}, // -Z  blue
        {{5,1,2,6}, {0.35f,0.85f,0.45f}}, // +X  green
        {{0,4,7,3}, {0.95f,0.80f,0.25f}}, // -X  yellow
        {{7,6,2,3}, {0.75f,0.45f,0.90f}}, // +Y  purple
        {{0,1,5,4}, {0.95f,0.55f,0.25f}}, // -Y  orange
    };
    std::vector<CubeVertex> v;
    for (const auto& f : faces)
        for (int k = 0; k < 4; ++k)
            v.push_back({ c[f.i[k]], f.col });
    return v;
}

std::vector<uint32_t> buildCubeIndices() {
    std::vector<uint32_t> idx;
    for (uint32_t f = 0; f < 6; ++f) {
        uint32_t b = f * 4;
        idx.insert(idx.end(), { b+0, b+1, b+2, b+0, b+2, b+3 });
    }
    return idx;
}

class HelloCube : public sv::EngineBase {
public:
    HelloCube(bool headless, std::string outPng)
        : m_headless(headless), m_outPng(std::move(outPng)) {
        setDevServerPort(0);   // the debug socket is opt-in; the sample doesn't need it
    }

protected:
    bool onInit() override;
    void onShutdown() override;
    bool onFrame(float dt) override;

private:
    bool createPipeline();
    bool createDescriptors();
    void initFrameSync();
    void drawScene(VkCommandBuffer cmd, VkImageView colorView, VkExtent2D ext, VkDescriptorSet set);
    bool renderHeadless();

    sv::Window     m_window;
    sv::VkCtx      m_vkCtx;
    sv::VkSwap     m_swapchain;
    sv::DepthImage m_depth;

    sv::VkBuf  m_vbo, m_ibo;
    uint32_t   m_indexCount = 0;

    sv::VkShader m_vert, m_frag;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;

    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    sv::VkBuf        m_ubo[MAX_FRAMES];
    VkDescriptorSet  m_descSets[MAX_FRAMES]{};

    sv::FrameSync m_frames[MAX_FRAMES]{};
    uint32_t      m_frameIdx = 0;
    float         m_time     = 0.0f;

    bool        m_headless = false;
    std::string m_outPng;
};

bool HelloCube::onInit() {
    sv::WindowConfig cfg;
    cfg.title = "StratumV - hello_cube";
    cfg.width = 1280; cfg.height = 720; cfg.vsync = true;
    if (!m_window.init(cfg))             { std::printf("[hello_cube] window init failed\n");  return false; }
    if (!m_vkCtx.init(m_window.handle())){ std::printf("[hello_cube] VkCtx init failed\n");    return false; }
    if (!m_swapchain.init(m_vkCtx, cfg.width, cfg.height, cfg.vsync)) {
        std::printf("[hello_cube] swapchain init failed\n"); return false;
    }
    m_depth = sv::DepthImage::create(m_vkCtx, cfg.width, cfg.height, DEPTH_FORMAT);

    sv::VkShader::initCompiler();
    if (!m_vert.loadFromFile(m_vkCtx.device(), "shaders/hello_cube.vert", VK_SHADER_STAGE_VERTEX_BIT) ||
        !m_frag.loadFromFile(m_vkCtx.device(), "shaders/hello_cube.frag", VK_SHADER_STAGE_FRAGMENT_BIT)) {
        std::printf("[hello_cube] shader compile failed (run from the exe's own dir so shaders/ resolves)\n");
        return false;
    }

    auto verts = buildCubeVerts();
    auto idx   = buildCubeIndices();
    m_indexCount = (uint32_t)idx.size();
    m_vbo = sv::VkBuf::createWithData(m_vkCtx, verts.data(), verts.size()*sizeof(CubeVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_ibo = sv::VkBuf::createWithData(m_vkCtx, idx.data(),   idx.size()*sizeof(uint32_t),     VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    if (!createDescriptors()) return false;
    if (!createPipeline())    return false;
    initFrameSync();

    std::printf("[hello_cube] initialized (%u verts, %u indices)%s\n",
                (uint32_t)verts.size(), m_indexCount, m_headless ? " [headless]" : "");
    return true;
}

bool HelloCube::createDescriptors() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1; li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(m_vkCtx.device(), &li, nullptr, &m_descLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.poolSizeCount = 1; pi.pPoolSizes = &ps; pi.maxSets = MAX_FRAMES;
    if (vkCreateDescriptorPool(m_vkCtx.device(), &pi, nullptr, &m_descPool) != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        m_ubo[i] = sv::VkBuf::create(m_vkCtx.allocator(), sizeof(CubeUBO),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = m_descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_descLayout;
        if (vkAllocateDescriptorSets(m_vkCtx.device(), &ai, &m_descSets[i]) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo bi{ m_ubo[i].buffer, 0, sizeof(CubeUBO) };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_descSets[i]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(m_vkCtx.device(), 1, &w, 0, nullptr);
    }
    return true;
}

bool HelloCube::createPipeline() {
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1; pl.pSetLayouts = &m_descLayout;
    if (vkCreatePipelineLayout(m_vkCtx.device(), &pl, nullptr, &m_pipeLayout) != VK_SUCCESS) return false;

    const VkVertexInputAttributeDescription attrs[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, pos)   },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, color) },
    };
    m_pipeline = sv::VkPipeBuilder()
        .setShaders(m_vert.module(), m_frag.module())
        .setVertexBinding(sizeof(CubeVertex), attrs, 2)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setCullMode(VK_CULL_MODE_NONE)          // no winding worries for a hello sample
        .setDepthTest(true, true)
        .setColorFormat(m_swapchain.format())
        .setDepthFormat(DEPTH_FORMAT)
        .setLayout(m_pipeLayout)
        .build(m_vkCtx.device(), VK_NULL_HANDLE);
    return m_pipeline != VK_NULL_HANDLE;
}

void HelloCube::initFrameSync() {
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_vkCtx.commandPool(); ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_vkCtx.device(), &ai, &m_frames[i].commandBuffer);
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(m_vkCtx.device(), &si, nullptr, &m_frames[i].imageAvailable);
        vkCreateSemaphore(m_vkCtx.device(), &si, nullptr, &m_frames[i].renderFinished);
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(m_vkCtx.device(), &fi, nullptr, &m_frames[i].inFlight);
    }
}

// Records a clear + cube draw into `cmd`. The color image must already be in
// COLOR_ATTACHMENT_OPTIMAL; the depth image (m_depth) is transitioned here.
void HelloCube::drawScene(VkCommandBuffer cmd, VkImageView colorView, VkExtent2D ext, VkDescriptorSet set) {
    sv::transitionImage(cmd, m_depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = colorView; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{ 0.12f, 0.12f, 0.15f, 1.0f }};
    VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    depth.imageView = m_depth.view; depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = { 1.0f, 0 };

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = {{0,0}, ext}; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &color; ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp{ 0.0f, (float)ext.height, (float)ext.width, -(float)ext.height, 0.0f, 1.0f }; // Y-flip
    VkRect2D   sc{ {0,0}, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout, 0, 1, &set, 0, nullptr);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vbo.buffer, &off);
    vkCmdBindIndexBuffer(cmd, m_ibo.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);

    vkCmdEndRendering(cmd);
}

bool HelloCube::onFrame(float dt) {
    m_window.pollEvents();
    if (m_window.shouldClose()) return false;
    m_time += dt;

    CubeUBO ubo{};
    glm::mat4 view = glm::lookAt(glm::vec3(2.6f, 1.9f, 3.2f), glm::vec3(0.0f), glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), m_window.aspect(), 0.1f, 100.0f);
    ubo.viewProj = proj * view;
    ubo.model    = glm::rotate(glm::mat4(1.0f), m_time * 0.8f, glm::normalize(glm::vec3(0.3f, 1.0f, 0.15f)));

    if (m_headless) {
        std::memcpy(m_ubo[0].info.pMappedData, &ubo, sizeof(ubo));
        bool ok = renderHeadless();
        return false;   // one frame, then exit
    }

    auto& fr = m_frames[m_frameIdx];
    vkWaitForFences(m_vkCtx.device(), 1, &fr.inFlight, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(m_vkCtx.device(), m_swapchain.swapchain(), UINT64_MAX,
                                         fr.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) return true;   // (no resize handling in this minimal sample)

    std::memcpy(m_ubo[m_frameIdx].info.pMappedData, &ubo, sizeof(ubo));
    vkResetFences(m_vkCtx.device(), 1, &fr.inFlight);
    vkResetCommandBuffer(fr.commandBuffer, 0);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fr.commandBuffer, &bi);

    sv::transitionImage(fr.commandBuffer, m_swapchain.image(imageIndex),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    drawScene(fr.commandBuffer, m_swapchain.imageView(imageIndex), m_swapchain.extent(), m_descSets[m_frameIdx]);

    sv::transitionImage(fr.commandBuffer, m_swapchain.image(imageIndex),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(fr.commandBuffer);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &fr.imageAvailable; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &fr.commandBuffer;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &fr.renderFinished;
    vkQueueSubmit(m_vkCtx.graphicsQueue(), 1, &si, fr.inFlight);

    VkSwapchainKHR sc = m_swapchain.swapchain();
    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &fr.renderFinished;
    pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &imageIndex;
    vkQueuePresentKHR(m_vkCtx.presentQueue(), &pi);

    m_frameIdx = (m_frameIdx + 1) % MAX_FRAMES;
    return true;
}

// Renders one frame to an offscreen image, reads it back, prints a pixel
// self-check, and writes a PNG. Avoids the swapchain entirely.
bool HelloCube::renderHeadless() {
    const uint32_t W = 1280, H = 720;
    VkExtent2D ext{ W, H };
    sv::ColorImage off = sv::ColorImage::create(m_vkCtx, W, H, m_swapchain.format(), VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    sv::VkBuf staging = sv::VkBuf::create(m_vkCtx.allocator(), (VkDeviceSize)W*H*4,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

    VkCommandBuffer cmd = m_vkCtx.beginSingleTimeCommands();
    sv::transitionImage(cmd, off.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    drawScene(cmd, off.view, ext, m_descSets[0]);
    sv::transitionImage(cmd, off.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { W, H, 1 };
    vkCmdCopyImageToBuffer(cmd, off.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
    m_vkCtx.endSingleTimeCommands(cmd);

    void* mapped = nullptr;
    vmaMapMemory(m_vkCtx.allocator(), staging.allocation, &mapped);
    const uint8_t* src = (const uint8_t*)mapped;
    std::vector<uint8_t> rgba((size_t)W*H*4);
    for (size_t i = 0; i < (size_t)W*H; ++i) {        // swapchain is BGRA -> RGBA
        rgba[i*4+0] = src[i*4+2];
        rgba[i*4+1] = src[i*4+1];
        rgba[i*4+2] = src[i*4+0];
        rgba[i*4+3] = 255;
    }
    auto px = [&](uint32_t x, uint32_t y) {
        size_t o = ((size_t)y*W + x)*4; return std::array<int,3>{ rgba[o], rgba[o+1], rgba[o+2] };
    };
    auto ctr = px(W/2, H/2); auto cor = px(4, 4);
    vmaUnmapMemory(m_vkCtx.allocator(), staging.allocation);

    bool cubeVisible = (std::abs(ctr[0]-cor[0]) + std::abs(ctr[1]-cor[1]) + std::abs(ctr[2]-cor[2])) > 30;
    int ok = stbi_write_png(m_outPng.c_str(), (int)W, (int)H, 4, rgba.data(), (int)W*4);
    std::printf("[hello_cube] headless: center rgb=(%d,%d,%d) corner rgb=(%d,%d,%d) -> %s | png '%s' %s\n",
                ctr[0],ctr[1],ctr[2], cor[0],cor[1],cor[2],
                cubeVisible ? "CUBE VISIBLE" : "WARNING: center==clear",
                m_outPng.c_str(), ok ? "written" : "FAILED");

    staging.destroy(m_vkCtx.allocator());
    off.destroy(m_vkCtx.device(), m_vkCtx.allocator());
    return cubeVisible && ok;
}

void HelloCube::onShutdown() {
    vkDeviceWaitIdle(m_vkCtx.device());
    VkDevice d = m_vkCtx.device(); VmaAllocator a = m_vkCtx.allocator();
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        if (m_frames[i].inFlight)       vkDestroyFence(d, m_frames[i].inFlight, nullptr);
        if (m_frames[i].imageAvailable) vkDestroySemaphore(d, m_frames[i].imageAvailable, nullptr);
        if (m_frames[i].renderFinished) vkDestroySemaphore(d, m_frames[i].renderFinished, nullptr);
        m_ubo[i].destroy(a);
    }
    if (m_pipeline)   vkDestroyPipeline(d, m_pipeline, nullptr);
    if (m_pipeLayout) vkDestroyPipelineLayout(d, m_pipeLayout, nullptr);
    if (m_descPool)   vkDestroyDescriptorPool(d, m_descPool, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(d, m_descLayout, nullptr);
    m_vert.destroy(d); m_frag.destroy(d);
    sv::VkShader::shutdownCompiler();
    m_vbo.destroy(a); m_ibo.destroy(a);
    m_depth.destroy(d, a);
    m_swapchain.shutdown(d);
    m_vkCtx.shutdown();
    m_window.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    bool headless = false;
    std::string outPng = "hello_cube.png";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--headless") { headless = true; if (i+1 < argc && argv[i+1][0] != '-') outPng = argv[++i]; }
    }
    HelloCube app(headless, outPng);
    return app.run(argc, argv);
}
