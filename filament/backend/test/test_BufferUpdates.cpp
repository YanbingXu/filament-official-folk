/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "BackendTest.h"

#include "Lifetimes.h"
#include "Shader.h"
#include "ShaderGenerator.h"
#include "TrianglePrimitive.h"

namespace {

////////////////////////////////////////////////////////////////////////////////////////////////////
// Shaders
////////////////////////////////////////////////////////////////////////////////////////////////////

std::string vertex (R"(#version 450 core

layout(location = 0) in vec4 mesh_position;

layout(location = 0) out uvec4 indices;

layout(binding = 0, set = 1) uniform Params {
    highp vec4 padding[4];  // offset of 64 bytes

    highp vec4 color;
    highp vec4 offset;
} params;

void main() {
    gl_Position = vec4(mesh_position.xy + params.offset.xy, 0.0, 1.0);
#if defined(TARGET_VULKAN_ENVIRONMENT)
    // In Vulkan, clip space is Y-down. In OpenGL and Metal, clip space is Y-up.
    gl_Position.y = -gl_Position.y;
#endif
}
)");

std::string fragment (R"(#version 450 core

layout(location = 0) out vec4 fragColor;

layout(binding = 0, set = 1) uniform Params {
    highp vec4 padding[4];  // offset of 64 bytes

    highp vec4 color;
    highp vec4 offset;
} params;

void main() {
    fragColor = vec4(params.color.rgb, 1.0f);
}

)");

}

namespace test {

using namespace filament;
using namespace filament::backend;

// In the shader, these MaterialParams are offset by 64 bytes into the uniform buffer to test buffer
// updates with offset.
struct MaterialParams {
    math::float4 color;
    math::float4 offset;
};
static_assert(sizeof(MaterialParams) == 8 * sizeof(float));
// Uniform config for writing MaterialParams to the shader uniform with 64 bytes of padding.
const UniformBindingConfig kBindingConfig = {
        .dataSize = sizeof(MaterialParams),
        .bufferSize = sizeof(MaterialParams) + 64,
        .byteOffset = 64
};

class BufferUpdatesTest : public BackendTest {
public:
    BufferUpdatesTest() : mCleanup(getDriverApi()) {}

protected:
    Shader createShader() {
        return Shader(getDriverApi(), mCleanup, ShaderConfig{
           vertex, fragment, {"Params"}
        });
    }

    Cleanup mCleanup;
};

TEST_F(BufferUpdatesTest, VertexBufferUpdate) {
    const bool largeBuffers = false;

    // If updateIndices is true, then even-numbered triangles will have their indices set to
    // {0, 0, 0}, effectively "hiding" every other triangle.
    const bool updateIndices = true;

    // The test is executed within this block scope to force destructors to run before
    // executeCommands().
    {
        auto& api = getDriverApi();
        Cleanup cleanup(api);

        // Create a platform-specific SwapChain and make it current.
        auto swapChain = cleanup.add(createSwapChain());
        api.makeCurrent(swapChain, swapChain);

        Shader shader = createShader();

        auto defaultRenderTarget = cleanup.add(api.createDefaultRenderTarget(0));

        // To test large buffers (which exercise a different code path) create an extra large
        // buffer. Only the first 3 vertices will be used.
        TrianglePrimitive triangle(api, largeBuffers);

        RenderPassParams params = {};
        fullViewport(params);
        params.flags.clear = TargetBufferFlags::COLOR;
        params.clearColor = {0.f, 1.f, 0.f, 1.f};
        params.flags.discardStart = TargetBufferFlags::ALL;
        params.flags.discardEnd = TargetBufferFlags::NONE;

        PipelineState state;
        state.program = shader.getProgram();
        state.pipelineLayout.setLayout[1] = { shader.getDescriptorSetLayout() };
        state.rasterState.colorWrite = true;
        state.rasterState.depthWrite = false;
        state.rasterState.depthFunc = RasterState::DepthFunc::A;
        state.rasterState.culling = CullingMode::NONE;

        // Create a uniform buffer.
        // We use STATIC here, even though the buffer is updated, to force the Metal backend to use
        // a GPU buffer, which is more interesting to test.
        auto ubuffer = cleanup.add(api.createBufferObject(sizeof(MaterialParams) + 64,
                BufferObjectBinding::UNIFORM, BufferUsage::STATIC));

        shader.bindUniform<MaterialParams>(api, ubuffer, kBindingConfig);

        api.startCapture(0);

        // Upload the uniform, but with an offset to accommodate the padding in the shader's
        // uniform definition.
        shader.uploadUniform(api, ubuffer, kBindingConfig, MaterialParams{
                .color = { 1.0f, 1.0f, 1.0f, 1.0f },
                .offset = { 0.0f, 0.0f, 0.0f, 0.0f }
        });

        api.makeCurrent(swapChain, swapChain);
        api.beginFrame(0, 0, 0);

        // Draw 10 triangles, updating the vertex buffer / index buffer each time.
        size_t triangleIndex = 0;
        for (float i = -1.0f; i < 1.0f; i += 0.2f) {
            const float low = i, high = i + 0.2;
            const filament::math::float2 v[3] {{low, low}, {high, low}, {low, high}};
            triangle.updateVertices(v);

            if (updateIndices) {
                if (triangleIndex % 2 == 0) {
                    // Upload each index separately, to test offsets.
                    const TrianglePrimitive::index_type i[3] {0, 1, 2};
                    triangle.updateIndices(i + 0, 1, 0);
                    triangle.updateIndices(i + 1, 1, 1);
                    triangle.updateIndices(i + 2, 1, 2);
                } else {
                    // This effectively hides this triangle.
                    const TrianglePrimitive::index_type i[3] {0, 0, 0};
                    triangle.updateIndices(i);
                }
            }

            if (triangleIndex > 0) {
                params.flags.clear = TargetBufferFlags::NONE;
                params.flags.discardStart = TargetBufferFlags::NONE;
            }

            api.beginRenderPass(defaultRenderTarget, params);
            api.draw(state, triangle.getRenderPrimitive(), 0, 3, 1);
            api.endRenderPass();

            triangleIndex++;
        }

        api.flush();
        api.commit(swapChain);
        api.endFrame(0);

        api.stopCapture(0);
    }

    executeCommands();
}

// This test renders two triangles in two separate draw calls. Between the draw calls, a uniform
// buffer object is partially updated.
TEST_F(BufferUpdatesTest, BufferObjectUpdateWithOffset) {
    auto& api = getDriverApi();
    Cleanup cleanup(api);

    // Create a platform-specific SwapChain and make it current.
    auto swapChain = cleanup.add(createSwapChain());
    api.makeCurrent(swapChain, swapChain);

    // Create a program.
    Shader shader = createShader();

    // Create a uniform buffer.
    // We use STATIC here, even though the buffer is updated, to force the Metal backend to use a
    // GPU buffer, which is more interesting to test.
    auto ubuffer = cleanup.add(api.createBufferObject(sizeof(MaterialParams) + 64,
            BufferObjectBinding::UNIFORM, BufferUsage::STATIC));

    shader.bindUniform<MaterialParams>(api, ubuffer, kBindingConfig);

    // Create a render target.
    auto colorTexture = cleanup.add(api.createTexture(SamplerType::SAMPLER_2D, 1,
            TextureFormat::RGBA8, 1, 512, 512, 1, TextureUsage::COLOR_ATTACHMENT));
    auto renderTarget = cleanup.add(api.createRenderTarget(
            TargetBufferFlags::COLOR0, 512, 512, 1, 0, {{colorTexture}}, {}, {}));

    // Upload uniforms for the first triangle.
    // Upload the uniform, but with an offset to accommodate the padding in the shader's
    // uniform definition.
    shader.uploadUniform(api, ubuffer, kBindingConfig, MaterialParams{
            .color = { 1.0f, 0.0f, 0.5f, 1.0f },
            .offset = { 0.0f, 0.0f, 0.0f, 0.0f }
    });

    RenderPassParams params = {};
    params.flags.clear = TargetBufferFlags::COLOR;
    params.clearColor = {0.f, 0.f, 1.f, 1.f};
    params.flags.discardStart = TargetBufferFlags::ALL;
    params.flags.discardEnd = TargetBufferFlags::NONE;
    params.viewport.height = 512;
    params.viewport.width = 512;
    renderTriangle({{ DescriptorSetLayoutHandle{}, shader.getDescriptorSetLayout() }},
            renderTarget, swapChain, shader.getProgram(), params);

    // Upload uniforms for the second triangle. To test partial buffer updates, we'll only update
    // color.b, color.a, offset.x, and offset.y.
    shader.uploadUniform(api, ubuffer, UniformBindingConfig{
                    .dataSize = sizeof(std::array<float, 4>),
                    .bufferSize = kBindingConfig.bufferSize,
                    .byteOffset = *kBindingConfig.byteOffset + offsetof(MaterialParams, color.b),
            },
            std::array<float, 4>{
                    // color.b, color.a
                    1.0f, 1.0f,
                    // offset.x, offset.y
                    0.5f, 0.5f }
    );

    params.flags.clear = TargetBufferFlags::NONE;
    params.flags.discardStart = TargetBufferFlags::NONE;
    renderTriangle({{ DescriptorSetLayoutHandle{}, shader.getDescriptorSetLayout() }},
            renderTarget, swapChain, shader.getProgram(), params);

    static const uint32_t expectedHash = 91322442;
    readPixelsAndAssertHash(
            "BufferObjectUpdateWithOffset", 512, 512, renderTarget, expectedHash, true);

    api.flush();
    api.commit(swapChain);
    api.endFrame(0);

    // This ensures all driver commands have finished before exiting the test.
    api.finish();

    executeCommands();

    getDriver().purge();
}

} // namespace test
