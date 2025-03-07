// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <glad/glad.h>
#ifdef ARCHITECTURE_ARM64
#include <android/log.h>
#include <arm_neon.h>
#endif
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/vector_math.h"
#include "core/hw/gpu.h"
#include "core/settings.h"
#include "video_core/pica_state.h"
#include "video_core/regs_framebuffer.h"
#include "video_core/regs_rasterizer.h"
#include "video_core/regs_texturing.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/on_screen_display.h"
#include "video_core/renderer_opengl/pica_to_gl.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/video_core.h"

namespace OpenGL {

using PixelFormat = SurfaceParams::PixelFormat;
using SurfaceType = SurfaceParams::SurfaceType;

static bool IsVendorMali() {
    std::string gpu_vendor{reinterpret_cast<char const*>(glGetString(GL_VENDOR))};
    return gpu_vendor.find("ARM") != std::string::npos;
}

RasterizerOpenGL::RasterizerOpenGL()
    : is_mali_gpu(IsVendorMali()), shader_dirty(true),
      vertex_buffer(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE),
      uniform_buffer(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE),
      index_buffer(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE),
      texture_buffer(GL_TEXTURE_BUFFER, is_mali_gpu ? 11264 : TEXTURE_BUFFER_SIZE),
      texture_lf_buffer(GL_TEXTURE_BUFFER, is_mali_gpu ? 525312 : TEXTURE_BUFFER_SIZE) {

    AllowShadow = (GLAD_GL_ARB_shader_image_load_store && GLAD_GL_ARB_shader_image_size &&
                   GLAD_GL_ARB_framebuffer_no_attachments) ||
                  Settings::values.allow_shadow;

    // Clipping plane 0 is always enabled for PICA fixed clip plane z <= 0
    state.clip_distance[0] = true;

    // Create sampler objects
    for (std::size_t i = 0; i < texture_samplers.size(); ++i) {
        texture_samplers[i].Create();
        state.texture_units[i].sampler = texture_samplers[i].sampler.handle;
    }

    // Create cubemap texture and sampler objects
    texture_cube_sampler.Create();
    state.texture_cube_unit.sampler = texture_cube_sampler.sampler.handle;

    // Generate VAO
    sw_vao.Create();
    hw_vao.Create();

    // Create render framebuffer
    framebuffer.Create();
    state.draw.draw_framebuffer = framebuffer.handle;

    // null texture
    texture_null.Create();
    state.texture_units[0].texture_2d = texture_null.handle;

    // Allocate and bind texture buffer lut textures
    texture_buffer_lut_lf.Create();
    texture_buffer_lut_rg.Create();
    texture_buffer_lut_rgba.Create();
    state.texture_buffer_lut_lf.texture_buffer = texture_buffer_lut_lf.handle;
    state.texture_buffer_lut_rg.texture_buffer = texture_buffer_lut_rg.handle;
    state.texture_buffer_lut_rgba.texture_buffer = texture_buffer_lut_rgba.handle;

    uniform_block_data.dirty = true;

    uniform_block_data.lighting_lut_dirty.fill(true);
    uniform_block_data.lighting_lut_dirty_any = true;
    uniform_block_data.light_dirty = true;

    uniform_block_data.fog_lut_dirty = true;

    uniform_block_data.proctex_noise_lut_dirty = true;
    uniform_block_data.proctex_color_map_dirty = true;
    uniform_block_data.proctex_alpha_map_dirty = true;
    uniform_block_data.proctex_lut_dirty = true;
    uniform_block_data.proctex_diff_lut_dirty = true;

    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_buffer_alignment);
    uniform_size_aligned_vs =
        Common::AlignUp<std::size_t>(sizeof(VSUniformData), uniform_buffer_alignment);
    uniform_size_aligned_fs =
        Common::AlignUp<std::size_t>(sizeof(UniformData), uniform_buffer_alignment);
    uniform_size_aligned_light =
        Common::AlignUp<std::size_t>(sizeof(UniformLightData), uniform_buffer_alignment);

    // Set vertex attributes for software shader path
    state.draw.vertex_array = sw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    state.Apply();

    glVertexAttribPointer(ATTRIBUTE_POSITION, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, position));
    glEnableVertexAttribArray(ATTRIBUTE_POSITION);

    glVertexAttribPointer(ATTRIBUTE_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, color));
    glEnableVertexAttribArray(ATTRIBUTE_COLOR);

    glVertexAttribPointer(ATTRIBUTE_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord0));
    glVertexAttribPointer(ATTRIBUTE_TEXCOORD1, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord1));
    glVertexAttribPointer(ATTRIBUTE_TEXCOORD2, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord2));
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD0);
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD1);
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD2);

    glVertexAttribPointer(ATTRIBUTE_TEXCOORD0_W, 1, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord0_w));
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD0_W);

    glVertexAttribPointer(ATTRIBUTE_NORMQUAT, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, normquat));
    glEnableVertexAttribArray(ATTRIBUTE_NORMQUAT);

    glVertexAttribPointer(ATTRIBUTE_VIEW, 3, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, view));
    glEnableVertexAttribArray(ATTRIBUTE_VIEW);

    // Create a 1x1 clear texture to use in the NULL case,
    // instead of OpenGL's default of solid black
    // For some reason alpha 0 wraps around to 1.0, so use 1/255 instead
    u8 null_data[4] = {0, 0, 0, 1};
    glActiveTexture(TextureUnits::PicaTexture(0).Enum());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, null_data);

    glActiveTexture(TextureUnits::TextureBufferLUT_LF.Enum());
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, texture_lf_buffer.GetHandle());
    glActiveTexture(TextureUnits::TextureBufferLUT_RG.Enum());
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, texture_buffer.GetHandle());
    glActiveTexture(TextureUnits::TextureBufferLUT_RGBA.Enum());
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, texture_buffer.GetHandle());

    // restore
    state.texture_units[0].texture_2d = 0;
    // Bind index buffer for hardware shader path
    state.draw.vertex_array = hw_vao.handle;
    state.Apply();

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer.GetHandle());

    // 845需要开启分离着色器，但开启后Mali GPU会挂掉，究极日也有显示问题！
    const bool use_separable_shader = Settings::values.use_separable_shader;
    shader_program_manager = std::make_unique<ShaderProgramManager>(use_separable_shader);

    // init opengl state
    glEnable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);

    SyncEntireState();
}

RasterizerOpenGL::~RasterizerOpenGL() = default;

void RasterizerOpenGL::SyncEntireState() {
    // Sync fixed function OpenGL state
    SyncClipEnabled();
    SyncCullMode();
    SyncBlendEnabled();
    SyncBlendFuncs();
    SyncBlendColor();
    SyncLogicOp();
    SyncStencilTest();
    SyncDepthTest();
    SyncColorWriteMask();
    SyncStencilWriteMask();
    SyncDepthWriteMask();

    // Sync uniforms
    SyncClipCoef();
    SyncDepthScale();
    SyncDepthOffset();
    SyncAlphaTest();
    SyncCombinerColor();
    auto& tev_stages = Pica::g_state.regs.texturing.GetTevStages();
    for (std::size_t index = 0; index < tev_stages.size(); ++index)
        SyncTevConstColor(index, tev_stages[index]);

    SyncGlobalAmbient();
    SyncLightingLutData();
    for (u32 light_index = 0; light_index < 8; light_index++) {
        SyncLightSpecular0(light_index);
        SyncLightSpecular1(light_index);
        SyncLightDiffuse(light_index);
        SyncLightAmbient(light_index);
        SyncLightPosition(light_index);
        SyncLightDistanceAttenuationBias(light_index);
        SyncLightDistanceAttenuationScale(light_index);
    }

    SyncFogColor();
    SyncFogLutData();
    SyncProcTexNoise();
    SyncProcTexBias();
    SyncShadowBias();
    SyncShadowTextureBias();
    SyncLightingLutScale();
}

/**
 * This is a helper function to resolve an issue when interpolating opposite quaternions. See below
 * for a detailed description of this issue (yuriks):
 *
 * For any rotation, there are two quaternions Q, and -Q, that represent the same rotation. If you
 * interpolate two quaternions that are opposite, instead of going from one rotation to another
 * using the shortest path, you'll go around the longest path. You can test if two quaternions are
 * opposite by checking if Dot(Q1, Q2) < 0. In that case, you can flip either of them, therefore
 * making Dot(Q1, -Q2) positive.
 *
 * This solution corrects this issue per-vertex before passing the quaternions to OpenGL. This is
 * correct for most cases but can still rotate around the long way sometimes. An implementation
 * which did `lerp(lerp(Q1, Q2), Q3)` (with proper weighting), applying the dot product check
 * between each step would work for those cases at the cost of being more complex to implement.
 *
 * Fortunately however, the 3DS hardware happens to also use this exact same logic to work around
 * these issues, making this basic implementation actually more accurate to the hardware.
 */
static bool AreQuaternionsOpposite(Common::Vec4<Pica::float24> qa, Common::Vec4<Pica::float24> qb) {
#ifdef ARCHITECTURE_ARM64
    const float32_t a[4] = {qa.x.ToFloat32(), qa.y.ToFloat32(), qa.z.ToFloat32(), qa.w.ToFloat32()};
    const float32_t b[4] = {qb.x.ToFloat32(), qb.y.ToFloat32(), qb.z.ToFloat32(), qb.w.ToFloat32()};
    const float32x4_t aa = vld1q_f32(a);
    const float32x4_t bb = vld1q_f32(b);
    const float32x4_t mm = vmulq_f32(aa, bb);
    const float32x2_t s2 = vadd_f32(vget_high_f32(mm), vget_low_f32(mm));
    return (vget_lane_f32(vpadd_f32(s2, s2), 0) < 0.f);
#else
    Common::Vec4f a{qa.x.ToFloat32(), qa.y.ToFloat32(), qa.z.ToFloat32(), qa.w.ToFloat32()};
    Common::Vec4f b{qb.x.ToFloat32(), qb.y.ToFloat32(), qb.z.ToFloat32(), qb.w.ToFloat32()};
    return (Common::Dot(a, b) < 0.f);
#endif
}

void RasterizerOpenGL::AddTriangle(const Pica::Shader::OutputVertex& v0,
                                   const Pica::Shader::OutputVertex& v1,
                                   const Pica::Shader::OutputVertex& v2) {
    vertex_batch.emplace_back(v0, false);
    vertex_batch.emplace_back(v1, AreQuaternionsOpposite(v0.quat, v1.quat));
    vertex_batch.emplace_back(v2, AreQuaternionsOpposite(v0.quat, v2.quat));
}

static constexpr std::array<GLenum, 4> vs_attrib_types{
    GL_BYTE,          // VertexAttributeFormat::BYTE
    GL_UNSIGNED_BYTE, // VertexAttributeFormat::UBYTE
    GL_SHORT,         // VertexAttributeFormat::SHORT
    GL_FLOAT          // VertexAttributeFormat::FLOAT
};

RasterizerOpenGL::VertexArrayInfo RasterizerOpenGL::AnalyzeVertexArray(bool is_indexed) {
    const auto& regs = Pica::g_state.regs;
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;

    u32 vertex_min;
    u32 vertex_max;
    if (is_indexed) {
        const auto& index_info = regs.pipeline.index_array;
        const PAddr address = vertex_attributes.GetPhysicalBaseAddress() + index_info.offset;
        const u8* index_address_8 = VideoCore::Memory()->GetPhysicalPointer(address);
        const u16* index_address_16 = reinterpret_cast<const u16*>(index_address_8);
        const bool index_u16 = index_info.format != 0;

        vertex_min = 0xFFFF;
        vertex_max = 0;
        // u32 size = regs.pipeline.num_vertices * (index_u16 ? 2 : 1);
        // res_cache.FlushRegion(address, size, nullptr);
        if (index_u16) {
            for (u32 i = 0; i < regs.pipeline.num_vertices; ++i) {
                u32 vertex = index_address_16[i];
                if (vertex < vertex_min)
                    vertex_min = vertex;
                if (vertex > vertex_max)
                    vertex_max = vertex;
            }
        } else {
            for (u32 i = 0; i < regs.pipeline.num_vertices; ++i) {
                u32 vertex = index_address_8[i];
                if (vertex < vertex_min)
                    vertex_min = vertex;
                if (vertex > vertex_max)
                    vertex_max = vertex;
            }
        }
    } else {
        vertex_min = regs.pipeline.vertex_offset;
        vertex_max = regs.pipeline.vertex_offset + regs.pipeline.num_vertices - 1;
    }

    const u32 vertex_num = vertex_max - vertex_min + 1;
    u32 vs_input_size = 0;
    for (const auto& loader : vertex_attributes.attribute_loaders) {
        if (loader.component_count != 0) {
            vs_input_size += loader.byte_count * vertex_num;
        }
    }

    return {vertex_min, vertex_max, vs_input_size};
}

void RasterizerOpenGL::SetupVertexArray(u8* array_ptr, GLintptr buffer_offset,
                                        GLuint vs_input_index_min, GLuint vs_input_index_max) {
    MICROPROFILE_SCOPE(OpenGL_VAO);
    const auto& regs = Pica::g_state.regs;
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    PAddr base_address = vertex_attributes.GetPhysicalBaseAddress();

    std::array<bool, 16> enable_attributes{};

    for (const auto& loader : vertex_attributes.attribute_loaders) {
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }

        u32 offset = 0;
        for (u32 comp = 0; comp < loader.component_count && comp < 12; ++comp) {
            u32 attribute_index = loader.GetComponent(comp);
            if (attribute_index < 12) {
                if (vertex_attributes.GetNumElements(attribute_index) != 0) {
                    offset = Common::AlignUp(
                        offset, vertex_attributes.GetElementSizeInBytes(attribute_index));

                    u32 input_reg = regs.vs.GetRegisterForAttribute(attribute_index);
                    GLint size = vertex_attributes.GetNumElements(attribute_index);
                    GLenum type = vs_attrib_types[static_cast<u32>(
                        vertex_attributes.GetFormat(attribute_index))];
                    GLsizei stride = loader.byte_count;
                    glVertexAttribPointer(input_reg, size, type, GL_FALSE, stride,
                                          reinterpret_cast<GLvoid*>(buffer_offset + offset));
                    enable_attributes[input_reg] = true;

                    offset += vertex_attributes.GetStride(attribute_index);
                }
            } else {
                // Attribute ids 12, 13, 14 and 15 signify 4, 8, 12 and 16-byte paddings,
                // respectively
                offset = Common::AlignUp(offset, 4);
                offset += (attribute_index - 11) * 4;
            }
        }

        PAddr data_addr =
            base_address + loader.data_offset + (vs_input_index_min * loader.byte_count);

        u32 vertex_num = vs_input_index_max - vs_input_index_min + 1;
        u32 data_size = loader.byte_count * vertex_num;

        // res_cache.FlushRegion(data_addr, data_size, nullptr);
        std::memcpy(array_ptr, VideoCore::Memory()->GetPhysicalPointer(data_addr), data_size);

        array_ptr += data_size;
        buffer_offset += data_size;
    }

    for (std::size_t i = 0; i < enable_attributes.size(); ++i) {
        if (enable_attributes[i] != hw_vao_enabled_attributes[i]) {
            if (enable_attributes[i]) {
                glEnableVertexAttribArray(i);
            } else {
                glDisableVertexAttribArray(i);
            }
            hw_vao_enabled_attributes[i] = enable_attributes[i];
        }

        if (vertex_attributes.IsDefaultAttribute(i)) {
            const u32 reg = regs.vs.GetRegisterForAttribute(i);
            if (!enable_attributes[reg]) {
                const auto& attr = Pica::g_state.input_default_attributes.attr[i];
                glVertexAttrib4f(reg, attr.x.ToFloat32(), attr.y.ToFloat32(), attr.z.ToFloat32(),
                                 attr.w.ToFloat32());
            }
        }
    }
}

bool RasterizerOpenGL::SetupVertexShader() {
    MICROPROFILE_SCOPE(OpenGL_VS);
    return shader_program_manager->UseProgrammableVertexShader(Pica::g_state.regs,
                                                               Pica::g_state.vs);
}

bool RasterizerOpenGL::SetupGeometryShader() {
    MICROPROFILE_SCOPE(OpenGL_GS);
    const auto& regs = Pica::g_state.regs;
    if (regs.pipeline.use_gs == Pica::PipelineRegs::UseGS::No) {
        shader_program_manager->UseFixedGeometryShader(regs);
        return true;
    } else {
        LOG_ERROR(Render_OpenGL, "Accelerate draw doesn't support geometry shader");
        return false;
    }
}

bool RasterizerOpenGL::AccelerateDrawBatch(bool is_indexed) {
    const auto& regs = Pica::g_state.regs;
    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) {
        return false;
    }

    if (!SetupVertexShader())
        return false;

    if (!SetupGeometryShader())
        return false;

    return Draw(true, is_indexed);
}

void RasterizerOpenGL::CheckForConfigChanges() {
    u16 scale_factor = VideoCore::GetResolutionScaleFactor();
    if (res_cache.GetScaleFactor() != scale_factor) {
        framebuffer_info = {};
        res_cache.SetScaleFactor(scale_factor);
    }
}

void RasterizerOpenGL::OnFrameUpdate() {
    const u32 current_frame = VideoCore::GetCurrentFrame();
    if (current_frame >= last_clean_frame + CLEAN_FRAME_INTERVAL) {
        res_cache.CleanUp(last_clean_frame);
        last_clean_frame = current_frame;
    }
}

static GLenum GetCurrentPrimitiveMode() {
    const GLenum prims[] = {GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_TRIANGLES};
    return prims[static_cast<u32>(Pica::g_state.regs.pipeline.triangle_topology.Value())];
}

bool RasterizerOpenGL::AccelerateDrawBatchInternal(bool is_indexed) {
    const auto& regs = Pica::g_state.regs;
    GLenum primitive_mode = GetCurrentPrimitiveMode();

    auto [vs_input_index_min, vs_input_index_max, vs_input_size] = AnalyzeVertexArray(is_indexed);

    if (vs_input_size > VERTEX_BUFFER_SIZE) {
        LOG_WARNING(Render_OpenGL, "Too large vertex input size {}", vs_input_size);
        return false;
    }

    state.draw.vertex_array = hw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    shader_program_manager->ApplyTo(state);
    state.Apply();

    u8* buffer_ptr;
    GLintptr buffer_offset;
    std::tie(buffer_ptr, buffer_offset, std::ignore) = vertex_buffer.Map(vs_input_size, 4);
    SetupVertexArray(buffer_ptr, buffer_offset, vs_input_index_min, vs_input_index_max);
    vertex_buffer.Unmap(vs_input_size);

    if (is_indexed) {
        bool index_u16 = regs.pipeline.index_array.format != 0;
        std::size_t index_buffer_size = regs.pipeline.num_vertices * (index_u16 ? 2 : 1);

        if (index_buffer_size > INDEX_BUFFER_SIZE) {
            LOG_WARNING(Render_OpenGL, "Too large index input size {}", index_buffer_size);
            return false;
        }

        const u8* index_data = VideoCore::Memory()->GetPhysicalPointer(
            regs.pipeline.vertex_attributes.GetPhysicalBaseAddress() +
            regs.pipeline.index_array.offset);
        std::tie(buffer_ptr, buffer_offset, std::ignore) = index_buffer.Map(index_buffer_size, 4);
        std::memcpy(buffer_ptr, index_data, index_buffer_size);
        index_buffer.Unmap(index_buffer_size);

        glDrawRangeElementsBaseVertex(
            primitive_mode, vs_input_index_min, vs_input_index_max, regs.pipeline.num_vertices,
            index_u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
            reinterpret_cast<const void*>(buffer_offset), -static_cast<GLint>(vs_input_index_min));
    } else {
        glDrawArrays(primitive_mode, 0, regs.pipeline.num_vertices);
    }

    return true;
}

void RasterizerOpenGL::DrawTriangles() {
    if (vertex_batch.empty())
        return;
    Draw(false, false);
}

void RasterizerOpenGL::BindFramebufferColor(OpenGLState& state, const Surface& surface) {
    state.draw.draw_framebuffer = framebuffer.handle;
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);
    if (framebuffer_info.color_attachment != surface->texture.handle) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               surface->texture.handle, 0);
        framebuffer_info.color_attachment = surface->texture.handle;
        framebuffer_info.color_width = surface->width;
        framebuffer_info.color_height = surface->height;
    }

    if (framebuffer_info.depth_attachment &&
        (framebuffer_info.color_width > framebuffer_info.depth_width ||
         framebuffer_info.color_height > framebuffer_info.depth_height)) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);
        framebuffer_info.depth_attachment = 0;
    }
}

void RasterizerOpenGL::BindFramebufferDepthStencil(OpenGLState& state, const Surface& surface) {
    state.draw.draw_framebuffer = framebuffer.handle;
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);
    if (framebuffer_info.depth_attachment != surface->texture.handle) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               surface->texture.handle, 0);
        framebuffer_info.depth_attachment = surface->texture.handle;
        framebuffer_info.depth_width = surface->width;
        framebuffer_info.depth_height = surface->height;
    }

    if (framebuffer_info.color_attachment &&
        (framebuffer_info.depth_width > framebuffer_info.color_width ||
         framebuffer_info.depth_height > framebuffer_info.color_height)) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.color_attachment = 0;
    }
}

void RasterizerOpenGL::BindFramebufferDepth(OpenGLState& state, const Surface& surface) {
    state.draw.draw_framebuffer = framebuffer.handle;
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);
    if (framebuffer_info.depth_attachment != surface->texture.handle) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               surface->texture.handle, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.depth_attachment = surface->texture.handle;
        framebuffer_info.depth_width = surface->width;
        framebuffer_info.depth_height = surface->height;
    }

    if (framebuffer_info.color_attachment &&
        (framebuffer_info.depth_width > framebuffer_info.color_width ||
         framebuffer_info.depth_height > framebuffer_info.color_height)) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.color_attachment = 0;
    }
}

bool RasterizerOpenGL::Draw(bool accelerate, bool is_indexed) {
    const auto& regs = Pica::g_state.regs;
    const bool shadow_rendering = regs.framebuffer.IsShadowRendering();
    if (shadow_rendering && !AllowShadow) {
        return true;
    }

    const bool has_stencil = regs.framebuffer.HasStencil();

    const bool write_color_fb = shadow_rendering || state.color_mask.red_enabled == GL_TRUE ||
                                state.color_mask.green_enabled == GL_TRUE ||
                                state.color_mask.blue_enabled == GL_TRUE ||
                                state.color_mask.alpha_enabled == GL_TRUE;
    const bool write_depth_fb =
        (state.depth.test_enabled && state.depth.write_mask == GL_TRUE) ||
        (has_stencil && state.stencil.test_enabled && state.stencil.write_mask != 0);

    const bool using_color_fb =
        regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress() != 0 && write_color_fb;
    const bool using_depth_fb =
        !shadow_rendering && regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress() != 0 &&
        (write_depth_fb || regs.framebuffer.output_merger.depth_test_enable != 0 ||
         (has_stencil && state.stencil.test_enabled));

    const Common::Rectangle<s32> viewport_rect_unscaled = regs.rasterizer.GetViewportRect();

    Surface color_surface;
    Surface depth_surface;
    Common::Rectangle<u32> surfaces_rect;
    std::tie(color_surface, depth_surface, surfaces_rect) =
        res_cache.GetFramebufferSurfaces(using_color_fb, using_depth_fb, viewport_rect_unscaled);

    u32 res_scale = 1;
    GLuint color_attachment = 0;
    if (color_surface) {
        res_scale = color_surface->res_scale;
        color_attachment = color_surface->texture.handle;
        framebuffer_info.color_width = color_surface->width;
        framebuffer_info.color_height = color_surface->height;
    } else if (depth_surface) {
        res_scale = depth_surface->res_scale;
    }

    // Sync and bind the texture surfaces
    const auto pica_textures = regs.texturing.GetTextures();
    for (unsigned texture_index = 0; texture_index < pica_textures.size(); ++texture_index) {
        const auto& texture = pica_textures[texture_index];

        if (texture.enabled) {
            if (texture_index == 0) {
                using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
                switch (texture.config.type.Value()) {
                case TextureType::Shadow2D: {
                    Surface surface = res_cache.GetTextureSurface(texture);
                    state.image_shadow_texture_px =
                        surface != nullptr ? surface->texture.handle : 0;
                    continue;
                }
                case TextureType::ShadowCube: {
                    Pica::Texture::TextureInfo info = Pica::Texture::TextureInfo::FromPicaRegister(
                        texture.config, texture.format);
                    Surface surface;

                    using CubeFace = Pica::TexturingRegs::CubeFace;
                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_px =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_nx =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_py =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_ny =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_pz =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_nz =
                        surface != nullptr ? surface->texture.handle : 0;
                    continue;
                }
                case TextureType::TextureCube:
                    using CubeFace = Pica::TexturingRegs::CubeFace;
                    TextureCubeConfig config;
                    config.px = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX);
                    config.nx = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX);
                    config.py = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY);
                    config.ny = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY);
                    config.pz = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ);
                    config.nz = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ);
                    config.width = texture.config.width;
                    config.format = texture.format;
                    state.texture_cube_unit.texture_cube =
                        res_cache.GetTextureCube(config).texture.handle;

                    texture_cube_sampler.SyncWithConfig(texture.config);
                    state.texture_units[texture_index].texture_2d = 0;
                    continue; // Texture unit 0 setup finished. Continue to next unit
                }
                state.texture_cube_unit.texture_cube = 0;
            }

            texture_samplers[texture_index].SyncWithConfig(texture.config);
            Surface surface = res_cache.GetTextureSurface(texture);
            if (surface) {
                if (surface->texture.handle == color_attachment) {
                    state.texture_units[texture_index].texture_2d = surface->GetTextureCopyHandle();
                } else {
                    state.texture_units[texture_index].texture_2d = surface->texture.handle;
                }
            } else {
                /// texture_null
                // Can occur when texture addr is null or its memory is unmapped/invalid
                // HACK: In this case, the correct behaviour for the PICA is to use the last
                // rendered colour. But because this would be impractical to implement, the
                // next best alternative is to use a clear texture, essentially skipping
                // the geometry in question.
                // For example: a bug in Pokemon X/Y causes NULL-texture squares to be drawn
                // on the male character's face, which in the OpenGL default appear black.
                state.texture_units[texture_index].texture_2d = texture_null.handle;
            }
        } else {
            state.texture_units[texture_index].texture_2d = 0;
        }
    }

    Common::Rectangle<u32> draw_rect{
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.left) +
                                             viewport_rect_unscaled.left * res_scale,
                                         surfaces_rect.left, surfaces_rect.right)), // Left
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.bottom) +
                                             viewport_rect_unscaled.top * res_scale,
                                         surfaces_rect.bottom, surfaces_rect.top)), // Top
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.left) +
                                             viewport_rect_unscaled.right * res_scale,
                                         surfaces_rect.left, surfaces_rect.right)), // Right
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.bottom) +
                                             viewport_rect_unscaled.bottom * res_scale,
                                         surfaces_rect.bottom, surfaces_rect.top))}; // Bottom

    // Sync the viewport
    state.viewport.x =
        static_cast<GLint>(surfaces_rect.left) + viewport_rect_unscaled.left * res_scale;
    state.viewport.y =
        static_cast<GLint>(surfaces_rect.bottom) + viewport_rect_unscaled.bottom * res_scale;
    state.viewport.width = static_cast<GLsizei>(viewport_rect_unscaled.GetWidth() * res_scale);
    state.viewport.height = static_cast<GLsizei>(viewport_rect_unscaled.GetHeight() * res_scale);

    // Bind the framebuffer surfaces
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);

    if (shadow_rendering) {
        if (color_surface == nullptr) {
            return true;
        }
        framebuffer_info.color_attachment = 0;
        framebuffer_info.depth_attachment = 0;
        glFramebufferParameteri(GL_DRAW_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH,
                                color_surface->GetScaledWidth());
        glFramebufferParameteri(GL_DRAW_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT,
                                color_surface->GetScaledHeight());
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);
        state.image_shadow_buffer = color_surface->texture.handle;
    } else {
        if (framebuffer_info.color_attachment != color_attachment) {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   color_attachment, 0);
            framebuffer_info.color_attachment = color_attachment;
        }
        if (depth_surface != nullptr) {
            if (has_stencil) {
                if (framebuffer_info.depth_attachment != depth_surface->texture.handle) {
                    // attach both depth and stencil
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                           GL_TEXTURE_2D, depth_surface->texture.handle, 0);
                    framebuffer_info.depth_attachment = depth_surface->texture.handle;
                    framebuffer_info.depth_width = depth_surface->width;
                    framebuffer_info.depth_height = depth_surface->height;
                }
            } else if (framebuffer_info.depth_attachment != depth_surface->texture.handle) {
                // attach depth
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                                       depth_surface->texture.handle, 0);
                // clear stencil attachment
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                                       0);
                framebuffer_info.depth_attachment = depth_surface->texture.handle;
                framebuffer_info.depth_width = depth_surface->width;
                framebuffer_info.depth_height = depth_surface->height;
            }
        } else if (framebuffer_info.depth_attachment != 0) {
            if (framebuffer_info.depth_width < surfaces_rect.right / res_scale ||
                framebuffer_info.depth_height < surfaces_rect.top / res_scale) {
                // clear both depth and stencil attachment
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                       GL_TEXTURE_2D, 0, 0);
                framebuffer_info.depth_attachment = 0;
            } else {
                state.depth.test_enabled = false;
                state.depth.write_mask = GL_FALSE;
            }
        }
    }

    // Scissor checks are window-, not viewport-relative, which means that if the cached texture
    // sub-rect changes, the scissor bounds also need to be updated.
    GLint scissor_x1 =
        static_cast<GLint>(surfaces_rect.left + regs.rasterizer.scissor_test.x1 * res_scale);
    GLint scissor_y1 =
        static_cast<GLint>(surfaces_rect.bottom + regs.rasterizer.scissor_test.y1 * res_scale);
    // x2, y2 have +1 added to cover the entire pixel area, otherwise you might get cracks when
    // scaling or doing multisampling.
    GLint scissor_x2 =
        static_cast<GLint>(surfaces_rect.left + (regs.rasterizer.scissor_test.x2 + 1) * res_scale);
    GLint scissor_y2 = static_cast<GLint>(surfaces_rect.bottom +
                                          (regs.rasterizer.scissor_test.y2 + 1) * res_scale);

    if (uniform_block_data.data.scissor_x1 != scissor_x1 ||
        uniform_block_data.data.scissor_x2 != scissor_x2 ||
        uniform_block_data.data.scissor_y1 != scissor_y1 ||
        uniform_block_data.data.scissor_y2 != scissor_y2) {

        uniform_block_data.data.scissor_x1 = scissor_x1;
        uniform_block_data.data.scissor_x2 = scissor_x2;
        uniform_block_data.data.scissor_y1 = scissor_y1;
        uniform_block_data.data.scissor_y2 = scissor_y2;
        uniform_block_data.dirty = true;
    }

    // Sync and bind the shader
    if (shader_dirty) {
        SetShader();
        shader_dirty = false;
    }

    // Sync the LUTs within the texture buffer
    SyncAndUploadLUTs();
    SyncAndUploadLUTsLF();

    // Sync the uniform data
    UploadUniforms(accelerate);

    // Viewport can have negative offsets or larger
    // dimensions than our framebuffer sub-rect.
    // Enable scissor test to prevent drawing
    // outside of the framebuffer region
    state.scissor.enabled = true;
    state.scissor.x = draw_rect.left;
    state.scissor.y = draw_rect.bottom;
    state.scissor.width = draw_rect.GetWidth();
    state.scissor.height = draw_rect.GetHeight();

    // Draw the vertex batch
    bool succeeded = true;
    if (accelerate) {
        succeeded = AccelerateDrawBatchInternal(is_indexed);
    } else {
        state.draw.vertex_array = sw_vao.handle;
        state.draw.vertex_buffer = vertex_buffer.GetHandle();
        shader_program_manager->UseTrivialVertexShader();
        shader_program_manager->UseTrivialGeometryShader();
        shader_program_manager->ApplyTo(state);
        state.Apply();

        std::size_t max_vertices = 3 * (VERTEX_BUFFER_SIZE / (3 * sizeof(HardwareVertex)));
        for (std::size_t base_vertex = 0; base_vertex < vertex_batch.size();
             base_vertex += max_vertices) {
            const std::size_t vertices = std::min(max_vertices, vertex_batch.size() - base_vertex);
            const std::size_t vertex_size = vertices * sizeof(HardwareVertex);
            u8* vbo;
            GLintptr offset;
            std::tie(vbo, offset, std::ignore) =
                vertex_buffer.Map(vertex_size, sizeof(HardwareVertex));
            std::memcpy(vbo, vertex_batch.data() + base_vertex, vertex_size);
            vertex_buffer.Unmap(vertex_size);
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(offset / sizeof(HardwareVertex)),
                         static_cast<GLsizei>(vertices));
        }
    }

    vertex_batch.clear();

    // Reset textures in rasterizer state context because the rasterizer cache might delete them
    for (auto& texture_unit : state.texture_units) {
        texture_unit.texture_2d = 0;
    }
    state.texture_cube_unit.texture_cube = 0;
    if (AllowShadow) {
        state.image_shadow_texture_px = 0;
        state.image_shadow_texture_nx = 0;
        state.image_shadow_texture_py = 0;
        state.image_shadow_texture_ny = 0;
        state.image_shadow_texture_pz = 0;
        state.image_shadow_texture_nz = 0;
        state.image_shadow_buffer = 0;
    }
    state.Apply();

    if (shadow_rendering) {
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                        GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    }

    // Mark framebuffer surfaces as dirty
    Common::Rectangle<u32> draw_rect_unscaled{draw_rect.left / res_scale, draw_rect.top / res_scale,
                                              draw_rect.right / res_scale,
                                              draw_rect.bottom / res_scale};

    if (color_surface != nullptr) {
        auto interval = color_surface->GetSubRectInterval(draw_rect_unscaled);
        res_cache.InvalidateRegion(boost::icl::first(interval), boost::icl::length(interval),
                                   color_surface);
    }
    if (depth_surface != nullptr && write_depth_fb) {
        auto interval = depth_surface->GetSubRectInterval(draw_rect_unscaled);
        res_cache.InvalidateRegion(boost::icl::first(interval), boost::icl::length(interval),
                                   depth_surface);
    }

    return succeeded;
}

void RasterizerOpenGL::NotifyPicaRegisterChanged(u32 id) {
    const auto& regs = Pica::g_state.regs;

    switch (id) {
    // Culling
    case PICA_REG_INDEX(rasterizer.cull_mode):
        SyncCullMode();
        break;

    // Clipping plane
    case PICA_REG_INDEX(rasterizer.clip_enable):
        SyncClipEnabled();
        break;

    case PICA_REG_INDEX(rasterizer.clip_coef[0]):
    case PICA_REG_INDEX(rasterizer.clip_coef[1]):
    case PICA_REG_INDEX(rasterizer.clip_coef[2]):
    case PICA_REG_INDEX(rasterizer.clip_coef[3]):
        SyncClipCoef();
        break;

    // Depth modifiers
    case PICA_REG_INDEX(rasterizer.viewport_depth_range):
        SyncDepthScale();
        break;
    case PICA_REG_INDEX(rasterizer.viewport_depth_near_plane):
        SyncDepthOffset();
        break;

    // Depth buffering
    case PICA_REG_INDEX(rasterizer.depthmap_enable):
        shader_dirty = true;
        break;

    // Blending
    case PICA_REG_INDEX(framebuffer.output_merger.alphablend_enable):
        SyncBlendEnabled();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.alpha_blending):
        SyncBlendFuncs();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.blend_const):
        SyncBlendColor();
        break;

    // Shadow texture
    case PICA_REG_INDEX(texturing.shadow):
        SyncShadowTextureBias();
        break;

    // Fog state
    case PICA_REG_INDEX(texturing.fog_color):
        SyncFogColor();
        break;
    /*case PICA_REG_INDEX(texturing.fog_lut_data[0]):
    case PICA_REG_INDEX(texturing.fog_lut_data[1]):
    case PICA_REG_INDEX(texturing.fog_lut_data[2]):
    case PICA_REG_INDEX(texturing.fog_lut_data[3]):
    case PICA_REG_INDEX(texturing.fog_lut_data[4]):
    case PICA_REG_INDEX(texturing.fog_lut_data[5]):
    case PICA_REG_INDEX(texturing.fog_lut_data[6]):
    case PICA_REG_INDEX(texturing.fog_lut_data[7]):
        SyncFogLutData();
        break;*/

    // ProcTex state
    case PICA_REG_INDEX(texturing.proctex):
    case PICA_REG_INDEX(texturing.proctex_lut):
    case PICA_REG_INDEX(texturing.proctex_lut_offset):
        SyncProcTexBias();
        break;

    case PICA_REG_INDEX(texturing.proctex_noise_u):
    case PICA_REG_INDEX(texturing.proctex_noise_v):
    case PICA_REG_INDEX(texturing.proctex_noise_frequency):
        SyncProcTexNoise();
        break;

    /*case PICA_REG_INDEX(texturing.proctex_lut_data[0]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[1]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[2]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[3]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[4]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[5]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[6]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[7]):
        SyncProcTexLutData();
        break;*/

    // Alpha test
    case PICA_REG_INDEX(framebuffer.output_merger.alpha_test):
        SyncAlphaTest();
        shader_dirty = true;
        break;

    // Sync GL stencil test + stencil write mask
    // (Pica stencil test function register also contains a stencil write mask)
    case PICA_REG_INDEX(framebuffer.output_merger.stencil_test.raw_func):
        SyncStencilTest();
        SyncStencilWriteMask();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.stencil_test.raw_op):
    case PICA_REG_INDEX(framebuffer.framebuffer.depth_format):
        SyncStencilTest();
        break;

    // Sync GL depth test + depth and color write mask
    // (Pica depth test function register also contains a depth and color write mask)
    case PICA_REG_INDEX(framebuffer.output_merger.depth_test_enable):
        SyncDepthTest();
        SyncDepthWriteMask();
        SyncColorWriteMask();
        break;

    // Sync GL depth and stencil write mask
    // (This is a dedicated combined depth / stencil write-enable register)
    case PICA_REG_INDEX(framebuffer.framebuffer.allow_depth_stencil_write):
        SyncDepthWriteMask();
        SyncStencilWriteMask();
        break;

    // Sync GL color write mask
    // (This is a dedicated color write-enable register)
    case PICA_REG_INDEX(framebuffer.framebuffer.allow_color_write):
        SyncColorWriteMask();
        break;

    case PICA_REG_INDEX(framebuffer.shadow):
        SyncShadowBias();
        break;

    // Scissor test
    case PICA_REG_INDEX(rasterizer.scissor_test.mode):
        shader_dirty = true;
        break;

    // Logic op
    case PICA_REG_INDEX(framebuffer.output_merger.logic_op):
        SyncLogicOp();
        break;

    case PICA_REG_INDEX(texturing.main_config):
        shader_dirty = true;
        break;

    // Texture 0 type
    case PICA_REG_INDEX(texturing.texture0.type):
        shader_dirty = true;
        break;

    // TEV stages
    // (This also syncs fog_mode and fog_flip which are part of tev_combiner_buffer_input)
    case PICA_REG_INDEX(texturing.tev_stage0.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage0.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage0.color_op):
    case PICA_REG_INDEX(texturing.tev_stage0.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage1.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage1.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage1.color_op):
    case PICA_REG_INDEX(texturing.tev_stage1.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage2.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage2.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage2.color_op):
    case PICA_REG_INDEX(texturing.tev_stage2.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage3.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage3.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage3.color_op):
    case PICA_REG_INDEX(texturing.tev_stage3.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage4.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage4.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage4.color_op):
    case PICA_REG_INDEX(texturing.tev_stage4.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage5.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage5.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage5.color_op):
    case PICA_REG_INDEX(texturing.tev_stage5.color_scale):
    case PICA_REG_INDEX(texturing.tev_combiner_buffer_input):
        shader_dirty = true;
        break;
    case PICA_REG_INDEX(texturing.tev_stage0.const_r):
        SyncTevConstColor(0, regs.texturing.tev_stage0);
        break;
    case PICA_REG_INDEX(texturing.tev_stage1.const_r):
        SyncTevConstColor(1, regs.texturing.tev_stage1);
        break;
    case PICA_REG_INDEX(texturing.tev_stage2.const_r):
        SyncTevConstColor(2, regs.texturing.tev_stage2);
        break;
    case PICA_REG_INDEX(texturing.tev_stage3.const_r):
        SyncTevConstColor(3, regs.texturing.tev_stage3);
        break;
    case PICA_REG_INDEX(texturing.tev_stage4.const_r):
        SyncTevConstColor(4, regs.texturing.tev_stage4);
        break;
    case PICA_REG_INDEX(texturing.tev_stage5.const_r):
        SyncTevConstColor(5, regs.texturing.tev_stage5);
        break;

    // TEV combiner buffer color
    case PICA_REG_INDEX(texturing.tev_combiner_buffer_color):
        SyncCombinerColor();
        break;

    // Fragment lighting switches
    /*case PICA_REG_INDEX(lighting.disable):
    case PICA_REG_INDEX(lighting.max_light_index):
    case PICA_REG_INDEX(lighting.config0):
    case PICA_REG_INDEX(lighting.config1):
    case PICA_REG_INDEX(lighting.abs_lut_input):
    case PICA_REG_INDEX(lighting.lut_input):
    case PICA_REG_INDEX(lighting.lut_scale):
    case PICA_REG_INDEX(lighting.light_enable):
        break;*/
    case PICA_REG_INDEX(lighting.lut_scale):
        SyncLightingLutScale();
        break;

    // Fragment lighting specular 0 color
    case PICA_REG_INDEX(lighting.light[0].specular_0):
    case PICA_REG_INDEX(lighting.light[1].specular_0):
    case PICA_REG_INDEX(lighting.light[2].specular_0):
    case PICA_REG_INDEX(lighting.light[3].specular_0):
    case PICA_REG_INDEX(lighting.light[4].specular_0):
    case PICA_REG_INDEX(lighting.light[5].specular_0):
    case PICA_REG_INDEX(lighting.light[6].specular_0):
    case PICA_REG_INDEX(lighting.light[7].specular_0):
        id -= PICA_REG_INDEX(lighting.light[0].specular_0);
        SyncLightSpecular0(id / 16);
        break;

    // Fragment lighting specular 1 color
    case PICA_REG_INDEX(lighting.light[0].specular_1):
    case PICA_REG_INDEX(lighting.light[1].specular_1):
    case PICA_REG_INDEX(lighting.light[2].specular_1):
    case PICA_REG_INDEX(lighting.light[3].specular_1):
    case PICA_REG_INDEX(lighting.light[4].specular_1):
    case PICA_REG_INDEX(lighting.light[5].specular_1):
    case PICA_REG_INDEX(lighting.light[6].specular_1):
    case PICA_REG_INDEX(lighting.light[7].specular_1):
        id -= PICA_REG_INDEX(lighting.light[0].specular_1);
        SyncLightSpecular1(id / 16);
        break;

    // Fragment lighting diffuse color
    case PICA_REG_INDEX(lighting.light[0].diffuse):
    case PICA_REG_INDEX(lighting.light[1].diffuse):
    case PICA_REG_INDEX(lighting.light[2].diffuse):
    case PICA_REG_INDEX(lighting.light[3].diffuse):
    case PICA_REG_INDEX(lighting.light[4].diffuse):
    case PICA_REG_INDEX(lighting.light[5].diffuse):
    case PICA_REG_INDEX(lighting.light[6].diffuse):
    case PICA_REG_INDEX(lighting.light[7].diffuse):
        id -= PICA_REG_INDEX(lighting.light[0].diffuse);
        SyncLightDiffuse(id / 16);
        break;

    // Fragment lighting ambient color
    case PICA_REG_INDEX(lighting.light[0].ambient):
    case PICA_REG_INDEX(lighting.light[1].ambient):
    case PICA_REG_INDEX(lighting.light[2].ambient):
    case PICA_REG_INDEX(lighting.light[3].ambient):
    case PICA_REG_INDEX(lighting.light[4].ambient):
    case PICA_REG_INDEX(lighting.light[5].ambient):
    case PICA_REG_INDEX(lighting.light[6].ambient):
    case PICA_REG_INDEX(lighting.light[7].ambient):
        id -= PICA_REG_INDEX(lighting.light[0].ambient);
        SyncLightAmbient(id / 16);
        break;

    // Fragment lighting position
    case PICA_REG_INDEX(lighting.light[0].x):
    case PICA_REG_INDEX(lighting.light[0].z):
    case PICA_REG_INDEX(lighting.light[1].x):
    case PICA_REG_INDEX(lighting.light[1].z):
    case PICA_REG_INDEX(lighting.light[2].x):
    case PICA_REG_INDEX(lighting.light[2].z):
    case PICA_REG_INDEX(lighting.light[3].x):
    case PICA_REG_INDEX(lighting.light[3].z):
    case PICA_REG_INDEX(lighting.light[4].x):
    case PICA_REG_INDEX(lighting.light[4].z):
    case PICA_REG_INDEX(lighting.light[5].x):
    case PICA_REG_INDEX(lighting.light[5].z):
    case PICA_REG_INDEX(lighting.light[6].x):
    case PICA_REG_INDEX(lighting.light[6].z):
    case PICA_REG_INDEX(lighting.light[7].x):
    case PICA_REG_INDEX(lighting.light[7].z):
        id -= PICA_REG_INDEX(lighting.light[0].x);
        SyncLightPosition(id / 16);
        break;

    // Fragment spot lighting direction
    case PICA_REG_INDEX(lighting.light[0].spot_x):
    case PICA_REG_INDEX(lighting.light[0].spot_z):
    case PICA_REG_INDEX(lighting.light[1].spot_x):
    case PICA_REG_INDEX(lighting.light[1].spot_z):
    case PICA_REG_INDEX(lighting.light[2].spot_x):
    case PICA_REG_INDEX(lighting.light[2].spot_z):
    case PICA_REG_INDEX(lighting.light[3].spot_x):
    case PICA_REG_INDEX(lighting.light[3].spot_z):
    case PICA_REG_INDEX(lighting.light[4].spot_x):
    case PICA_REG_INDEX(lighting.light[4].spot_z):
    case PICA_REG_INDEX(lighting.light[5].spot_x):
    case PICA_REG_INDEX(lighting.light[5].spot_z):
    case PICA_REG_INDEX(lighting.light[6].spot_x):
    case PICA_REG_INDEX(lighting.light[6].spot_z):
    case PICA_REG_INDEX(lighting.light[7].spot_x):
    case PICA_REG_INDEX(lighting.light[7].spot_z):
        id -= PICA_REG_INDEX(lighting.light[0].spot_x);
        SyncLightSpotDirection(id / 16);
        break;

    // Fragment lighting light source config
    case PICA_REG_INDEX(lighting.light[0].config):
    case PICA_REG_INDEX(lighting.light[1].config):
    case PICA_REG_INDEX(lighting.light[2].config):
    case PICA_REG_INDEX(lighting.light[3].config):
    case PICA_REG_INDEX(lighting.light[4].config):
    case PICA_REG_INDEX(lighting.light[5].config):
    case PICA_REG_INDEX(lighting.light[6].config):
    case PICA_REG_INDEX(lighting.light[7].config):
        shader_dirty = true;
        break;

    // Fragment lighting distance attenuation bias
    case PICA_REG_INDEX(lighting.light[0].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[1].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[2].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[3].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[4].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[5].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[6].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[7].dist_atten_bias):
        id -= PICA_REG_INDEX(lighting.light[0].dist_atten_bias);
        SyncLightDistanceAttenuationBias(id / 16);
        break;

    // Fragment lighting distance attenuation scale
    case PICA_REG_INDEX(lighting.light[0].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[1].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[2].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[3].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[4].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[5].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[6].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[7].dist_atten_scale):
        id -= PICA_REG_INDEX(lighting.light[0].dist_atten_scale);
        SyncLightDistanceAttenuationScale(id / 16);
        break;

    // Fragment lighting global ambient color (emission + ambient * ambient)
    case PICA_REG_INDEX(lighting.global_ambient):
        SyncGlobalAmbient();
        break;

    // Fragment lighting lookup tables
    /*case PICA_REG_INDEX(lighting.lut_data[0]):
    case PICA_REG_INDEX(lighting.lut_data[1]):
    case PICA_REG_INDEX(lighting.lut_data[2]):
    case PICA_REG_INDEX(lighting.lut_data[3]):
    case PICA_REG_INDEX(lighting.lut_data[4]):
    case PICA_REG_INDEX(lighting.lut_data[5]):
    case PICA_REG_INDEX(lighting.lut_data[6]):
    case PICA_REG_INDEX(lighting.lut_data[7]): {
        SyncLightingLutData();
        break;
    }*/
    }
}

void RasterizerOpenGL::FlushAll() {
    res_cache.FlushAll();
}

void RasterizerOpenGL::FlushRegion(PAddr addr, u32 size) {
    res_cache.FlushRegion(addr, size);
}

void RasterizerOpenGL::InvalidateRegion(PAddr addr, u32 size) {
    res_cache.InvalidateRegion(addr, size, nullptr);
}

void RasterizerOpenGL::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size, nullptr);
}

bool RasterizerOpenGL::AccelerateDisplayTransfer(const GPU::Regs::DisplayTransferConfig& config) {
    SurfaceParams src_params;
    src_params.addr = config.GetPhysicalInputAddress();
    src_params.width = config.output_width;
    src_params.stride = config.input_width;
    src_params.height = config.output_height;
    src_params.is_tiled = !config.input_linear;
    src_params.pixel_format = SurfaceParams::PixelFormatFromGPUPixelFormat(config.input_format);
    src_params.UpdateParams();

    SurfaceParams dst_params;
    dst_params.addr = config.GetPhysicalOutputAddress();
    dst_params.width = config.scaling != config.NoScale ? config.output_width.Value() / 2
                                                        : config.output_width.Value();
    dst_params.height = config.scaling == config.ScaleXY ? config.output_height.Value() / 2
                                                         : config.output_height.Value();
    dst_params.is_tiled = config.input_linear != config.dont_swizzle;
    dst_params.pixel_format = SurfaceParams::PixelFormatFromGPUPixelFormat(config.output_format);
    dst_params.UpdateParams();

    // hack for Tales of the Abyss / Pac Man Party 3D
    if (Settings::values.display_transfer_hack) {
        if (dst_params.height == 400) {
            if (dst_params.addr == 0x183CE430) {
                dst_params.addr -= 0xCE430;
                dst_params.end -= 0xCE430;
            } else if (dst_params.addr == 0x18387F30) {
                dst_params.addr -= 0x41A30;
                dst_params.end -= 0x41A30;
            }
        } else {
            if (dst_params.addr == 0x180B4830) {
                dst_params.addr -= 0x34830;
                dst_params.end -= 0x34830;
            } else if (dst_params.addr == 0x1807C430) {
                dst_params.addr += 0x3BFD0;
                dst_params.end += 0x3BFD0;
            }
        }
    }

    Common::Rectangle<u32> src_rect;
    Surface src_surface;
    std::tie(src_surface, src_rect) =
        res_cache.GetSurfaceSubRect(src_params, ScaleMatch::Ignore, true);
    if (src_surface == nullptr)
        return false;

    dst_params.res_scale = src_surface->res_scale;

    Common::Rectangle<u32> dst_rect;
    Surface dst_surface;
    std::tie(dst_surface, dst_rect) =
        res_cache.GetSurfaceSubRect(dst_params, ScaleMatch::Upscale, false);
    if (dst_surface == nullptr)
        return false;

    if (src_surface->is_tiled != dst_surface->is_tiled)
        std::swap(src_rect.top, src_rect.bottom);

    if (config.flip_vertically)
        std::swap(src_rect.top, src_rect.bottom);

    if (!res_cache.BlitSurfaces(src_surface, src_rect, dst_surface, dst_rect))
        return false;

    res_cache.InvalidateRegion(dst_params.addr, dst_params.size, dst_surface);
    return true;
}

bool RasterizerOpenGL::AccelerateTextureCopy(const GPU::Regs::DisplayTransferConfig& config) {
    u32 copy_size = Common::AlignDown(config.texture_copy.size, 16);
    if (copy_size == 0) {
        return false;
    }

    u32 input_gap = config.texture_copy.input_gap * 16;
    u32 input_width = config.texture_copy.input_width * 16;
    if (input_width == 0 && input_gap != 0) {
        return false;
    }
    if (input_gap == 0 || input_width >= copy_size) {
        input_width = copy_size;
        input_gap = 0;
    }
    if (copy_size % input_width != 0) {
        return false;
    }

    u32 output_gap = config.texture_copy.output_gap * 16;
    u32 output_width = config.texture_copy.output_width * 16;
    if (output_width == 0 && output_gap != 0) {
        return false;
    }
    if (output_gap == 0 || output_width >= copy_size) {
        output_width = copy_size;
        output_gap = 0;
    }
    if (copy_size % output_width != 0) {
        return false;
    }

    SurfaceParams src_params;
    src_params.addr = config.GetPhysicalInputAddress();
    src_params.stride = input_width + input_gap; // stride in bytes
    src_params.width = input_width;              // width in bytes
    src_params.height = copy_size / input_width;
    src_params.size = ((src_params.height - 1) * src_params.stride) + src_params.width;
    src_params.end = src_params.addr + src_params.size;

    Common::Rectangle<u32> src_rect;
    Surface src_surface;
    std::tie(src_surface, src_rect) = res_cache.GetTexCopySurface(src_params);
    if (src_surface == nullptr) {
        return false;
    }

    if (output_gap != 0 &&
        (output_width != src_surface->BytesInPixels(src_rect.GetWidth() / src_surface->res_scale) *
                             (src_surface->is_tiled ? 8 : 1) ||
         output_gap % src_surface->BytesInPixels(src_surface->is_tiled ? 64 : 1) != 0)) {
        return false;
    }

    SurfaceParams dst_params = *src_surface;
    dst_params.addr = config.GetPhysicalOutputAddress();
    dst_params.width = src_rect.GetWidth() / src_surface->res_scale;
    dst_params.stride = dst_params.width + src_surface->PixelsInBytes(
                                               src_surface->is_tiled ? output_gap / 8 : output_gap);
    dst_params.height = src_rect.GetHeight() / src_surface->res_scale;
    dst_params.res_scale = src_surface->res_scale;
    dst_params.UpdateParams();

    // Since we are going to invalidate the gap if there is one, we will have to load it first
    const bool load_gap = output_gap != 0;
    Common::Rectangle<u32> dst_rect;
    Surface dst_surface;
    std::tie(dst_surface, dst_rect) =
        res_cache.GetSurfaceSubRect(dst_params, ScaleMatch::Upscale, load_gap);
    if (dst_surface == nullptr) {
        return false;
    }

    if (dst_surface->type == SurfaceType::Texture) {
        return false;
    }

    if (!res_cache.BlitSurfaces(src_surface, src_rect, dst_surface, dst_rect)) {
        return false;
    }

    res_cache.InvalidateRegion(dst_params.addr, dst_params.size, dst_surface);
    return true;
}

bool RasterizerOpenGL::AccelerateFill(const GPU::Regs::MemoryFillConfig& config) {
    Surface dst_surface = res_cache.GetFillSurface(config);
    res_cache.InvalidateRegion(dst_surface->addr, dst_surface->size, dst_surface);
    return true;
}

bool RasterizerOpenGL::AccelerateDisplay(const GPU::Regs::FramebufferConfig& config,
                                         PAddr framebuffer_addr, u32 pixel_stride,
                                         ScreenInfo& screen_info) {
    if (framebuffer_addr == 0) {
        return false;
    }
    MICROPROFILE_SCOPE(OpenGL_CacheManagement);

    SurfaceParams src_params;
    src_params.addr = framebuffer_addr;
    src_params.width = std::min(config.width.Value(), pixel_stride);
    src_params.height = config.height;
    src_params.stride = pixel_stride;
    src_params.is_tiled = false;
    src_params.pixel_format = SurfaceParams::PixelFormatFromGPUPixelFormat(config.color_format);
    src_params.UpdateParams();

    Common::Rectangle<u32> src_rect;
    Surface src_surface;
    std::tie(src_surface, src_rect) =
        res_cache.GetSurfaceSubRect(src_params, ScaleMatch::Ignore, true);

    if (src_surface == nullptr) {
        return false;
    }

    u32 scaled_width = src_surface->GetScaledWidth();
    u32 scaled_height = src_surface->GetScaledHeight();

    screen_info.display_texcoords = Common::Rectangle<float>(
        (float)src_rect.bottom / (float)scaled_height, (float)src_rect.left / (float)scaled_width,
        (float)src_rect.top / (float)scaled_height, (float)src_rect.right / (float)scaled_width);

    screen_info.display_texture = src_surface->texture.handle;

    return true;
}

void RasterizerOpenGL::SamplerInfo::Create() {
    sampler.Create();
    mag_filter = min_filter = mip_filter = TextureConfig::Linear;
    wrap_s = wrap_t = TextureConfig::Repeat;
    border_color = 0;
    lod_min = lod_max = 0;
    lod_bias = 0;

    // default is 1000 and -1000
    // Other attributes have correct defaults
    glSamplerParameterf(sampler.handle, GL_TEXTURE_MAX_LOD, static_cast<float>(lod_max));
    glSamplerParameterf(sampler.handle, GL_TEXTURE_MIN_LOD, static_cast<float>(lod_min));
}

void RasterizerOpenGL::SamplerInfo::SyncWithConfig(const TextureConfig& config) {
    GLuint s = sampler.handle;

    using TextureFilter = Pica::TexturingRegs::TextureConfig::TextureFilter;
    bool use_linear_filter =
        Settings::values.use_linear_filter && config.mag_filter == TextureFilter::Nearest;

    if (!use_linear_filter) {
        if (mag_filter != config.mag_filter) {
            mag_filter = config.mag_filter;
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER,
                                PicaToGL::TextureMagFilterMode(mag_filter));
        }

        if (min_filter != config.min_filter || mip_filter != config.mip_filter) {
            min_filter = config.min_filter;
            mip_filter = config.mip_filter;
            if (config.type == Pica::TexturingRegs::TextureConfig::TextureCube) {
                // HACK: use mag filter converter for min filter because they are the same anyway
                glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER,
                                    PicaToGL::TextureMagFilterMode(min_filter));
            } else {
                glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER,
                                    PicaToGL::TextureMinFilterMode(min_filter, mip_filter));
            }
        }
    }

    if (wrap_s != config.wrap_s) {
        wrap_s = config.wrap_s;
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, PicaToGL::WrapMode(wrap_s));
    }
    if (wrap_t != config.wrap_t) {
        wrap_t = config.wrap_t;
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, PicaToGL::WrapMode(wrap_t));
    }

    if (wrap_s == TextureConfig::ClampToBorder || wrap_t == TextureConfig::ClampToBorder) {
        if (border_color != config.border_color.raw) {
            border_color = config.border_color.raw;
            auto gl_color = PicaToGL::ColorRGBA8(border_color);
            glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, gl_color.data());
        }
    }

    if (lod_min != config.lod.min_level) {
        lod_min = config.lod.min_level;
        glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, static_cast<float>(lod_min));
    }

    if (lod_max != config.lod.max_level) {
        lod_max = config.lod.max_level;
        glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, static_cast<float>(lod_max));
    }

    if (lod_bias != config.lod.bias) {
        lod_bias = config.lod.bias;
        if (GLES) {
            LOG_DEBUG(Render_OpenGL, "sync sampler lod bias: {}", lod_bias);
        } else {
            glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, lod_bias / 256.0f);
        }
    }
}

void RasterizerOpenGL::SetShader() {
    shader_program_manager->UseFragmentShader(Pica::g_state.regs);
}

void RasterizerOpenGL::SyncClipEnabled() {
    state.clip_distance[1] = Pica::g_state.regs.rasterizer.clip_enable != 0;
}

void RasterizerOpenGL::SyncClipCoef() {
    if (Settings::values.disable_clip_coef) {
        return;
    }

    const auto raw_clip_coef = Pica::g_state.regs.rasterizer.GetClipCoef();
    const GLvec4 new_clip_coef = {raw_clip_coef.x.ToFloat32(), raw_clip_coef.y.ToFloat32(),
                                  raw_clip_coef.z.ToFloat32(), raw_clip_coef.w.ToFloat32()};
    if (new_clip_coef != uniform_block_data.data.clip_coef) {
        uniform_block_data.data.clip_coef = new_clip_coef;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncCullMode() {
    const auto& regs = Pica::g_state.regs;
    state.cull.enabled = regs.rasterizer.cull_mode != Pica::RasterizerRegs::CullMode::KeepAll;
    state.cull.front_face =
        regs.rasterizer.cull_mode == Pica::RasterizerRegs::CullMode::KeepClockWise ? GL_CW : GL_CCW;
}

void RasterizerOpenGL::SyncDepthScale() {
    float depth_scale =
        Pica::float24::FromRaw(Pica::g_state.regs.rasterizer.viewport_depth_range).ToFloat32();
    if (depth_scale != uniform_block_data.data.depth_scale) {
        uniform_block_data.data.depth_scale = depth_scale;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncDepthOffset() {
    float depth_offset =
        Pica::float24::FromRaw(Pica::g_state.regs.rasterizer.viewport_depth_near_plane).ToFloat32();
    if (depth_offset != uniform_block_data.data.depth_offset) {
        uniform_block_data.data.depth_offset = depth_offset;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncBlendEnabled() {
    state.blend.enabled = (Pica::g_state.regs.framebuffer.output_merger.alphablend_enable == 1);
}

void RasterizerOpenGL::SyncBlendFuncs() {
    const auto& regs = Pica::g_state.regs;
    state.blend.rgb_equation =
        PicaToGL::BlendEquation(regs.framebuffer.output_merger.alpha_blending.blend_equation_rgb);
    state.blend.a_equation =
        PicaToGL::BlendEquation(regs.framebuffer.output_merger.alpha_blending.blend_equation_a);
    state.blend.src_rgb_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_source_rgb);
    state.blend.dst_rgb_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_dest_rgb);
    state.blend.src_a_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_source_a);
    state.blend.dst_a_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_dest_a);
}

void RasterizerOpenGL::SyncBlendColor() {
    auto blend_color =
        PicaToGL::ColorRGBA8(Pica::g_state.regs.framebuffer.output_merger.blend_const.raw);
    state.blend.color.red = blend_color[0];
    state.blend.color.green = blend_color[1];
    state.blend.color.blue = blend_color[2];
    state.blend.color.alpha = blend_color[3];
}

void RasterizerOpenGL::SyncFogColor() {
    const auto& regs = Pica::g_state.regs;
    GLvec3 fog_color;
    fog_color[0] = uniform_block_data.data.fog_color[0];
    fog_color[1] = uniform_block_data.data.fog_color[1];
    fog_color[2] = uniform_block_data.data.fog_color[2];
    uniform_block_data.data.fog_color = {
        regs.texturing.fog_color.r.Value() / 255.0f,
        regs.texturing.fog_color.g.Value() / 255.0f,
        regs.texturing.fog_color.b.Value() / 255.0f,
    };
    if (fog_color[0] != uniform_block_data.data.fog_color[0] ||
        fog_color[1] != uniform_block_data.data.fog_color[1] ||
        fog_color[2] != uniform_block_data.data.fog_color[2]) {
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncFogLutData() {
    uniform_block_data.fog_lut_dirty = true;
}

void RasterizerOpenGL::SyncProcTexNoise() {
    const auto& regs = Pica::g_state.regs.texturing;
    uniform_block_data.data.proctex_noise_f = {
        Pica::float16::FromRaw(regs.proctex_noise_frequency.u).ToFloat32(),
        Pica::float16::FromRaw(regs.proctex_noise_frequency.v).ToFloat32(),
    };
    uniform_block_data.data.proctex_noise_a = {
        regs.proctex_noise_u.amplitude / 4095.0f,
        regs.proctex_noise_v.amplitude / 4095.0f,
    };
    uniform_block_data.data.proctex_noise_p = {
        Pica::float16::FromRaw(regs.proctex_noise_u.phase).ToFloat32(),
        Pica::float16::FromRaw(regs.proctex_noise_v.phase).ToFloat32(),
    };
    uniform_block_data.dirty = true;
}

void RasterizerOpenGL::SyncProcTexBias() {
    const auto& regs = Pica::g_state.regs.texturing;
    uniform_block_data.data.proctex_bias =
        Pica::float16::FromRaw(regs.proctex.bias_low | (regs.proctex_lut.bias_high << 8))
            .ToFloat32();
    uniform_block_data.dirty = true;
    shader_dirty = true;
}

void RasterizerOpenGL::SyncProcTexLutData() {
    using Pica::TexturingRegs;
    const auto& regs = Pica::g_state.regs;
    switch (regs.texturing.proctex_lut_config.ref_table.Value()) {
    case TexturingRegs::ProcTexLutTable::Noise:
        uniform_block_data.proctex_noise_lut_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::ColorMap:
        uniform_block_data.proctex_color_map_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::AlphaMap:
        uniform_block_data.proctex_alpha_map_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::Color:
        uniform_block_data.proctex_lut_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::ColorDiff:
        uniform_block_data.proctex_diff_lut_dirty = true;
        break;
    }
}

void RasterizerOpenGL::SyncAlphaTest() {
    const auto& regs = Pica::g_state.regs;
    if (regs.framebuffer.output_merger.alpha_test.ref != uniform_block_data.data.alphatest_ref) {
        uniform_block_data.data.alphatest_ref = regs.framebuffer.output_merger.alpha_test.ref;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncLogicOp() {
    const auto& regs = Pica::g_state.regs;
    state.logic_op = PicaToGL::LogicOp(regs.framebuffer.output_merger.logic_op);
    /*if (GLES && (GLAD_GL_ARM_shader_framebuffer_fetch || GLAD_GL_EXT_shader_framebuffer_fetch)) {
        shader_dirty = true;
    }*/
    if (GLES) {
        if (!regs.framebuffer.output_merger.alphablend_enable) {
            if (regs.framebuffer.output_merger.logic_op == Pica::FramebufferRegs::LogicOp::NoOp) {
                // Color output is disabled by logic operation. We use color write mask to skip
                // color but allow depth write.
                state.color_mask = {};
            }
        }
    }
}

void RasterizerOpenGL::SyncColorWriteMask() {
    const auto& regs = Pica::g_state.regs;
    if (GLES) {
        if (!regs.framebuffer.output_merger.alphablend_enable) {
            if (regs.framebuffer.output_merger.logic_op == Pica::FramebufferRegs::LogicOp::NoOp) {
                // Color output is disabled by logic operation. We use color write mask to skip
                // color but allow depth write. Return early to avoid overwriting this.
                return;
            }
        }
    }

    auto IsColorWriteEnabled = [&](u32 value) {
        return (regs.framebuffer.framebuffer.allow_color_write != 0 && value != 0) ? GL_TRUE
                                                                                   : GL_FALSE;
    };

    state.color_mask.red_enabled = IsColorWriteEnabled(regs.framebuffer.output_merger.red_enable);
    state.color_mask.green_enabled =
        IsColorWriteEnabled(regs.framebuffer.output_merger.green_enable);
    state.color_mask.blue_enabled = IsColorWriteEnabled(regs.framebuffer.output_merger.blue_enable);
    state.color_mask.alpha_enabled =
        IsColorWriteEnabled(regs.framebuffer.output_merger.alpha_enable);
}

void RasterizerOpenGL::SyncStencilWriteMask() {
    const auto& regs = Pica::g_state.regs;
    state.stencil.write_mask =
        (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0)
            ? static_cast<GLuint>(regs.framebuffer.output_merger.stencil_test.write_mask)
            : 0;
}

void RasterizerOpenGL::SyncDepthWriteMask() {
    const auto& regs = Pica::g_state.regs;
    state.depth.write_mask = (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0 &&
                              regs.framebuffer.output_merger.depth_write_enable)
                                 ? GL_TRUE
                                 : GL_FALSE;
}

void RasterizerOpenGL::SyncStencilTest() {
    const auto& regs = Pica::g_state.regs;
    state.stencil.test_enabled =
        regs.framebuffer.output_merger.stencil_test.enable &&
        regs.framebuffer.framebuffer.depth_format == Pica::FramebufferRegs::DepthFormat::D24S8;
    state.stencil.test_func =
        PicaToGL::CompareFunc(regs.framebuffer.output_merger.stencil_test.func);
    state.stencil.test_ref = regs.framebuffer.output_merger.stencil_test.reference_value;
    state.stencil.test_mask = regs.framebuffer.output_merger.stencil_test.input_mask;
    state.stencil.action_stencil_fail =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_stencil_fail);
    state.stencil.action_depth_fail =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_depth_fail);
    state.stencil.action_depth_pass =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_depth_pass);
}

void RasterizerOpenGL::SyncDepthTest() {
    const auto& regs = Pica::g_state.regs;
    state.depth.test_enabled = regs.framebuffer.output_merger.depth_test_enable == 1 ||
                               regs.framebuffer.output_merger.depth_write_enable == 1;
    state.depth.test_func =
        regs.framebuffer.output_merger.depth_test_enable == 1
            ? PicaToGL::CompareFunc(regs.framebuffer.output_merger.depth_test_func)
            : GL_ALWAYS;
}

void RasterizerOpenGL::SyncCombinerColor() {
    const auto combiner_color =
        PicaToGL::ColorRGBA8(Pica::g_state.regs.texturing.tev_combiner_buffer_color.raw);
    if (combiner_color != uniform_block_data.data.tev_combiner_buffer_color) {
        uniform_block_data.data.tev_combiner_buffer_color = combiner_color;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncTevConstColor(int stage_index,
                                         const Pica::TexturingRegs::TevStageConfig& tev_stage) {
    const auto const_color = PicaToGL::ColorRGBA8(tev_stage.const_color);
    if (const_color != uniform_block_data.data.const_color[stage_index]) {
        uniform_block_data.data.const_color[stage_index] = const_color;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncGlobalAmbient() {
    const auto ambient_color = PicaToGL::LightColor(Pica::g_state.regs.lighting.global_ambient);
    if (ambient_color != uniform_block_data.light_data.lighting_global_ambient) {
        uniform_block_data.light_data.lighting_global_ambient = ambient_color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightingLutScale() {
    const auto& regs = Pica::g_state.regs;
    GLfloat scales[7];
    scales[0] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.d0);
    scales[1] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.d1);
    scales[2] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.sp);
    scales[3] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.fr);
    scales[4] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.rb);
    scales[5] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.rg);
    scales[6] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.rr);
    for (int i = 0; i < 7; ++i) {
        if (scales[i] != uniform_block_data.light_data.lighting_lut_scales[i]) {
            uniform_block_data.light_data.lighting_lut_scales[i] = scales[i];
            uniform_block_data.light_dirty = true;
        }
    }
}

void RasterizerOpenGL::SyncLightingLutData() {
    const auto& regs = Pica::g_state.regs;
    auto& lut_config = regs.lighting.lut_config;
    uniform_block_data.lighting_lut_dirty[lut_config.type] = true;
    uniform_block_data.lighting_lut_dirty_any = true;
}

void RasterizerOpenGL::SyncLightSpecular0(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].specular_0);
    if (color != uniform_block_data.light_data.light_src[light_index].specular_0) {
        uniform_block_data.light_data.light_src[light_index].specular_0 = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightSpecular1(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].specular_1);
    if (color != uniform_block_data.light_data.light_src[light_index].specular_1) {
        uniform_block_data.light_data.light_src[light_index].specular_1 = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightDiffuse(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].diffuse);
    if (color != uniform_block_data.light_data.light_src[light_index].diffuse) {
        uniform_block_data.light_data.light_src[light_index].diffuse = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightAmbient(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].ambient);
    if (color != uniform_block_data.light_data.light_src[light_index].ambient) {
        uniform_block_data.light_data.light_src[light_index].ambient = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightPosition(int light_index) {
    GLvec3 position = {
        Pica::float16::FromRaw(Pica::g_state.regs.lighting.light[light_index].x).ToFloat32(),
        Pica::float16::FromRaw(Pica::g_state.regs.lighting.light[light_index].y).ToFloat32(),
        Pica::float16::FromRaw(Pica::g_state.regs.lighting.light[light_index].z).ToFloat32()};

    if (position != uniform_block_data.light_data.light_src[light_index].position) {
        uniform_block_data.light_data.light_src[light_index].position = position;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightSpotDirection(int light_index) {
    const auto& light = Pica::g_state.regs.lighting.light[light_index];
    GLvec3 spot_direction = {light.spot_x / 2047.0f, light.spot_y / 2047.0f,
                             light.spot_z / 2047.0f};

    if (spot_direction != uniform_block_data.light_data.light_src[light_index].spot_direction) {
        uniform_block_data.light_data.light_src[light_index].spot_direction = spot_direction;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightDistanceAttenuationBias(int light_index) {
    GLfloat dist_atten_bias =
        Pica::float20::FromRaw(Pica::g_state.regs.lighting.light[light_index].dist_atten_bias)
            .ToFloat32();

    if (dist_atten_bias != uniform_block_data.light_data.light_src[light_index].dist_atten_bias) {
        uniform_block_data.light_data.light_src[light_index].dist_atten_bias = dist_atten_bias;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightDistanceAttenuationScale(int light_index) {
    GLfloat dist_atten_scale =
        Pica::float20::FromRaw(Pica::g_state.regs.lighting.light[light_index].dist_atten_scale)
            .ToFloat32();

    if (dist_atten_scale != uniform_block_data.light_data.light_src[light_index].dist_atten_scale) {
        uniform_block_data.light_data.light_src[light_index].dist_atten_scale = dist_atten_scale;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncShadowBias() {
    const auto& shadow = Pica::g_state.regs.framebuffer.shadow;
    GLfloat constant = Pica::float16::FromRaw(shadow.constant).ToFloat32();
    GLfloat linear = Pica::float16::FromRaw(shadow.linear).ToFloat32();

    if (constant != uniform_block_data.data.shadow_bias_constant ||
        linear != uniform_block_data.data.shadow_bias_linear) {
        uniform_block_data.data.shadow_bias_constant = constant;
        uniform_block_data.data.shadow_bias_linear = linear;
        uniform_block_data.dirty = AllowShadow;
    }
}

void RasterizerOpenGL::SyncShadowTextureBias() {
    GLint bias = Pica::g_state.regs.texturing.shadow.bias << 1;
    if (bias != uniform_block_data.light_data.shadow_texture_bias) {
        uniform_block_data.light_data.shadow_texture_bias = bias;
        uniform_block_data.light_dirty = AllowShadow;
    }
}

void RasterizerOpenGL::SyncAndUploadLUTsLF() {
    constexpr std::size_t max_size =
        sizeof(GLvec2) * 256 * Pica::LightingRegs::NumLightingSampler + sizeof(GLvec2) * 128; // fog

    if (!uniform_block_data.lighting_lut_dirty_any && !uniform_block_data.fog_lut_dirty) {
        return;
    }

    u8* buffer;
    GLintptr offset;
    bool invalidate;
    std::size_t bytes_used = 0;
    glBindBuffer(GL_TEXTURE_BUFFER, texture_lf_buffer.GetHandle());
    std::tie(buffer, offset, invalidate) = texture_lf_buffer.Map(max_size, sizeof(GLvec4));

    // Sync the lighting luts
    if (uniform_block_data.lighting_lut_dirty_any || invalidate) {
        for (unsigned index = 0; index < uniform_block_data.lighting_lut_dirty.size(); index++) {
            if (uniform_block_data.lighting_lut_dirty[index] || invalidate) {
                bool is_changed = false;
                for (std::size_t i = 0; i < lighting_lut_data[index].size(); ++i) {
                    const auto& entry = Pica::g_state.lighting.luts[index][i];
                    GLvec2 new_data{entry.ToFloat(), entry.DiffToFloat()};
                    const auto& prev_data = lighting_lut_data[index][i];
                    if (new_data != prev_data) {
                        lighting_lut_data[index][i] = new_data;
                        is_changed = true;
                    }
                }
                if (is_changed || invalidate) {
                    std::memcpy(buffer + bytes_used, lighting_lut_data[index].data(),
                                lighting_lut_data[index].size() * sizeof(GLvec2));
                    uniform_block_data.light_data.lighting_lut_offset[index / 4][index % 4] =
                        (offset + bytes_used) / sizeof(GLvec2);
                    uniform_block_data.light_dirty = true;
                    bytes_used += lighting_lut_data[index].size() * sizeof(GLvec2);
                }
                uniform_block_data.lighting_lut_dirty[index] = false;
            }
        }
        uniform_block_data.lighting_lut_dirty_any = false;
    }

    // Sync the fog lut
    if (uniform_block_data.fog_lut_dirty || invalidate) {
        bool is_changed = false;
        for (std::size_t i = 0; i < fog_lut_data.size(); ++i) {
            const auto& entry = Pica::g_state.fog.lut[i];
            GLvec2 new_data{entry.ToFloat(), entry.DiffToFloat()};
            const auto& prev_data = fog_lut_data[i];
            if (new_data != prev_data) {
                fog_lut_data[i] = new_data;
                is_changed = true;
            }
        }
        if (is_changed || invalidate) {
            std::memcpy(buffer + bytes_used, fog_lut_data.data(),
                        fog_lut_data.size() * sizeof(GLvec2));
            uniform_block_data.data.fog_lut_offset = (offset + bytes_used) / sizeof(GLvec2);
            uniform_block_data.dirty = true;
            bytes_used += fog_lut_data.size() * sizeof(GLvec2);
        }
        uniform_block_data.fog_lut_dirty = false;
    }

    texture_lf_buffer.Unmap(bytes_used);
}

void RasterizerOpenGL::SyncAndUploadLUTs() {
    constexpr std::size_t max_size = sizeof(GLvec2) * 128 * 3 + // proctex: noise + color + alpha
                                     sizeof(GLvec4) * 256 +     // proctex
                                     sizeof(GLvec4) * 256;      // proctex diff

    if (!uniform_block_data.proctex_noise_lut_dirty &&
        !uniform_block_data.proctex_color_map_dirty &&
        !uniform_block_data.proctex_alpha_map_dirty && !uniform_block_data.proctex_lut_dirty &&
        !uniform_block_data.proctex_diff_lut_dirty) {
        return;
    }

    u8* buffer;
    GLintptr offset;
    bool invalidate;
    std::size_t bytes_used = 0;
    glBindBuffer(GL_TEXTURE_BUFFER, texture_buffer.GetHandle());
    std::tie(buffer, offset, invalidate) = texture_buffer.Map(max_size, sizeof(GLvec4));

    // helper function for SyncProcTexNoiseLUT/ColorMap/AlphaMap
    auto SyncProcTexValueLUT = [this, buffer, offset, invalidate, &bytes_used](
                                   const std::array<Pica::State::ProcTex::ValueEntry, 128>& lut,
                                   std::array<GLvec2, 128>& lut_data, GLint& lut_offset) {
        bool is_changed = false;
        for (std::size_t i = 0; i < lut_data.size(); ++i) {
            const auto& entry = lut[i];
            GLvec2 new_data{entry.ToFloat(), entry.DiffToFloat()};
            const auto& prev_data = lut_data[i];
            if (new_data != prev_data) {
                lut_data[i] = new_data;
                is_changed = true;
            }
        }
        if (is_changed || invalidate) {
            std::memcpy(buffer + bytes_used, lut_data.data(), lut_data.size() * sizeof(GLvec2));
            lut_offset = (offset + bytes_used) / sizeof(GLvec2);
            uniform_block_data.dirty = true;
            bytes_used += lut_data.size() * sizeof(GLvec2);
        }
    };

    // Sync the proctex noise lut
    if (uniform_block_data.proctex_noise_lut_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.noise_table, proctex_noise_lut_data,
                            uniform_block_data.data.proctex_noise_lut_offset);
        uniform_block_data.proctex_noise_lut_dirty = false;
    }

    // Sync the proctex color map
    if (uniform_block_data.proctex_color_map_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.color_map_table, proctex_color_map_data,
                            uniform_block_data.data.proctex_color_map_offset);
        uniform_block_data.proctex_color_map_dirty = false;
    }

    // Sync the proctex alpha map
    if (uniform_block_data.proctex_alpha_map_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.alpha_map_table, proctex_alpha_map_data,
                            uniform_block_data.data.proctex_alpha_map_offset);
        uniform_block_data.proctex_alpha_map_dirty = false;
    }

    // Sync the proctex lut
    if (uniform_block_data.proctex_lut_dirty || invalidate) {
        bool is_changed = false;
        for (std::size_t i = 0; i < proctex_lut_data.size(); ++i) {
            const auto& entry = Pica::g_state.proctex.color_table[i];
            GLvec4 new_color{static_cast<u8>(entry.r) / 255.0f, static_cast<u8>(entry.g) / 255.0f,
                             static_cast<u8>(entry.b) / 255.0f, static_cast<u8>(entry.a) / 255.0f};
            const auto& prev_color = proctex_lut_data[i];
            if (new_color != prev_color) {
                proctex_lut_data[i] = new_color;
                is_changed = true;
            }
        }
        if (is_changed || invalidate) {
            std::memcpy(buffer + bytes_used, proctex_lut_data.data(),
                        proctex_lut_data.size() * sizeof(GLvec4));
            uniform_block_data.data.proctex_lut_offset = (offset + bytes_used) / sizeof(GLvec4);
            uniform_block_data.dirty = true;
            bytes_used += proctex_lut_data.size() * sizeof(GLvec4);
        }
        uniform_block_data.proctex_lut_dirty = false;
    }

    // Sync the proctex difference lut
    if (uniform_block_data.proctex_diff_lut_dirty || invalidate) {
        bool is_changed = false;
        for (std::size_t i = 0; i < proctex_diff_lut_data.size(); ++i) {
            const auto& entry = Pica::g_state.proctex.color_diff_table[i];
            GLvec4 new_color{static_cast<u8>(entry.r) / 255.0f, static_cast<u8>(entry.g) / 255.0f,
                             static_cast<u8>(entry.b) / 255.0f, static_cast<u8>(entry.a) / 255.0f};
            const auto& prev_color = proctex_diff_lut_data[i];
            if (new_color != prev_color) {
                proctex_diff_lut_data[i] = new_color;
                is_changed = true;
            }
        }
        if (is_changed || invalidate) {
            std::memcpy(buffer + bytes_used, proctex_diff_lut_data.data(),
                        proctex_diff_lut_data.size() * sizeof(GLvec4));
            uniform_block_data.data.proctex_diff_lut_offset =
                (offset + bytes_used) / sizeof(GLvec4);
            uniform_block_data.dirty = true;
            bytes_used += proctex_diff_lut_data.size() * sizeof(GLvec4);
        }
        uniform_block_data.proctex_diff_lut_dirty = false;
    }

    texture_buffer.Unmap(bytes_used);
}

void RasterizerOpenGL::UploadUniforms(bool accelerate_draw) {
    // glBindBufferRange below also changes the generic buffer binding point, so we sync the state
    // first
    OpenGLState::BindUniformBuffer(uniform_buffer.GetHandle());

    const bool sync_vs = accelerate_draw;
    const bool sync_fs = uniform_block_data.dirty;
    const bool sync_light = uniform_block_data.light_dirty;

    std::size_t uniform_size = 0;
    if (sync_vs) {
        uniform_size += uniform_size_aligned_vs;
    }
    if (sync_fs) {
        uniform_size += uniform_size_aligned_fs;
    }
    if (sync_light) {
        uniform_size += uniform_size_aligned_light;
    }
    if (uniform_size == 0) {
        return;
    }

    std::size_t used_bytes = 0;
    u8* uniforms;
    GLintptr offset;
    bool invalidate;
    std::tie(uniforms, offset, invalidate) =
        uniform_buffer.Map(uniform_size, uniform_buffer_alignment);

    if (sync_vs) {
        VSUniformData vs_uniforms;
        vs_uniforms.uniforms.SetFromRegs(Pica::g_state.regs.vs, Pica::g_state.vs);
        std::memcpy(uniforms + used_bytes, &vs_uniforms, sizeof(vs_uniforms));
        glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(UniformBindings::VS),
                          uniform_buffer.GetHandle(), offset + used_bytes, sizeof(VSUniformData));
        used_bytes += uniform_size_aligned_vs;
    }

    if (sync_fs || invalidate) {
        std::memcpy(uniforms + used_bytes, &uniform_block_data.data, sizeof(UniformData));
        glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(UniformBindings::Common),
                          uniform_buffer.GetHandle(), offset + used_bytes, sizeof(UniformData));
        uniform_block_data.dirty = false;
        used_bytes += uniform_size_aligned_fs;
    }

    if (sync_light || invalidate) {
        std::memcpy(uniforms + used_bytes, &uniform_block_data.light_data, sizeof(UniformLightData));
        glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(UniformBindings::Light),
                          uniform_buffer.GetHandle(), offset + used_bytes, sizeof(UniformLightData));
        uniform_block_data.light_dirty = false;
        used_bytes += uniform_size_aligned_light;
    }

    uniform_buffer.Unmap(used_bytes);
}

} // namespace OpenGL
