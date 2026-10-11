// SPIR-V -> MSL (see shader_msl.h). The only translation unit of this backend that sees SPIRV-Cross;
// its std:: API types stay inside it. SPIRV-Cross reports failures by throwing
// spirv_cross::CompilerError, so this file alone is built with exceptions (CMakeLists.txt) and turns
// them into Results at the boundary.

#include "shader_msl.h"

#include <spirv_msl.hpp>

#include <string>  // mce-allow: SPIRV-Cross returns std::string
#include <vector>  // mce-allow: SPIRV-Cross takes SPIR-V as std::vector<uint32_t>

namespace mce::rhi::sdlgpu {

ResultV spirv_to_msl(ShaderStage stage, const Array<u32>& spirv, const StageResources& resources, MslPlatform platform,
                     const char* debug_name, Array<char>& msl, FixedString<1024>& log) {
    const char* name = debug_name ? debug_name : "";
    msl.clear();
    std::string text;  // mce-allow: SPIRV-Cross output
    try {
        std::vector<u32> words(spirv.begin(), spirv.end());  // mce-allow: SPIRV-Cross API type
        spirv_cross::CompilerMSL compiler(std::move(words));
        const spv::ExecutionModel model =
            stage == ShaderStage::Vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
        spirv_cross::CompilerMSL::Options mo;
        mo.platform = platform == MslPlatform::iOS ? spirv_cross::CompilerMSL::Options::iOS
                                                   : spirv_cross::CompilerMSL::Options::macOS;
        mo.set_msl_version(2, 3);
        mo.enable_point_size_builtin = true;
        compiler.set_msl_options(mo);
        // The SPIR-V already has SDL GPU's clip space; nothing to fix up here.
        spirv_cross::CompilerGLSL::Options go = compiler.get_common_options();
        go.vertex.fixup_clipspace = false;
        go.vertex.flip_vert_y = false;
        compiler.set_common_options(go);

        const u32 resource_set = stage == ShaderStage::Vertex ? 0 : 2;
        auto bind = [&](u32 set, u32 binding, u32 buffer, u32 texture, u32 sampler) {
            spirv_cross::MSLResourceBinding mb;
            mb.stage = model;
            mb.desc_set = set;
            mb.binding = binding;
            mb.count = 1;
            mb.msl_buffer = buffer;
            mb.msl_texture = texture;
            mb.msl_sampler = sampler;
            compiler.add_msl_resource_binding(mb);
        };
        for (u32 i = 0; i < resources.sampler_count; ++i) bind(resource_set, i, 0, i, i);
        for (u32 j = 0; j < resources.storage_count; ++j)
            bind(resource_set, resources.sampler_count + j, resources.uniform_count + j, 0, 0);
        for (u32 b = 0; b < resources.uniform_count; ++b) bind(resource_set + 1, b, b, 0, 0);
        compiler.rename_entry_point("main", kMslEntryPoint, model);
        text = compiler.compile();
    } catch (const spirv_cross::CompilerError& e) {
        log.append(e.what());
        return Error(Errc::ShaderCompile, "rhi.sdlgpu.msl", name);
    }
    msl.resize(text.size() + 1);
    for (usize i = 0; i < text.size(); ++i) msl[i] = text[i];
    msl[text.size()] = '\0';
    return ok();
}

}  // namespace mce::rhi::sdlgpu
