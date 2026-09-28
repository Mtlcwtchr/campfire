// Offline HLSL -> SPIR-V -> Metal validation. No SDL video, window or GPU device.
#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: asr_shader_check FILE vertex|fragment|compute ENTRY\n";
        return 2;
    }
    const std::string stageName = argv[2];
    if (stageName != "vertex" && stageName != "fragment" && stageName != "compute") return 2;
    const auto stage = stageName == "vertex" ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX :
        stageName == "fragment" ? SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT : SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;
    const auto path = std::filesystem::absolute(argv[1]);
    std::ifstream file(path);
    if (!file) { std::cerr << "cannot read " << path << '\n'; return 1; }
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    const auto includes = path.parent_path().string();
    if (!SDL_ShaderCross_Init()) { std::cerr << SDL_GetError() << '\n'; return 1; }
    SDL_ShaderCross_HLSL_Info hlsl{};
    hlsl.source = source.c_str(); hlsl.entrypoint = argv[3];
    hlsl.include_dir = includes.c_str(); hlsl.shader_stage = stage;
    std::size_t size = 0;
    void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl, &size);
    if (!spirv) {
        std::cerr << SDL_GetError() << '\n';
        SDL_ShaderCross_Quit(); return 1;
    }
    SDL_ShaderCross_SPIRV_Info info{};
    info.bytecode = static_cast<const Uint8*>(spirv); info.bytecode_size = size;
    info.entrypoint = argv[3]; info.shader_stage = stage;
    info.props = SDL_CreateProperties();
    SDL_SetStringProperty(info.props, SDL_SHADERCROSS_PROP_SPIRV_MSL_VERSION_STRING, "2.1.0");
    void* metal = SDL_ShaderCross_TranspileMSLFromSPIRV(&info);
    const bool ok = metal != nullptr;
    if (!ok) std::cerr << SDL_GetError() << '\n';
    else std::cout << path.filename().string() << ':' << argv[3] << " SPIR-V/Metal OK (" << size << " bytes)\n";
    SDL_free(metal); SDL_free(spirv); SDL_DestroyProperties(info.props);
    SDL_ShaderCross_Quit();
    return ok ? 0 : 1;
}

