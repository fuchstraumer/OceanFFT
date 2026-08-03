/**
 * OceanShaderCompiler
 * Loads Slang modules, enumerates defined entrypoints, compiles each to WGSL,
 * and bakes all variants into a C++ header.
 *
 * Usage:
 *   OceanShaderCompiler --output <header.hpp> [--wave-variants]
 *                       [--define K[=V]]... <module.slang>...
 * --O<n>: optimization level, 0-3, s, or z. If unspecified, defaults to 0 (no optimizations).
 *         this is probably the one to use, since we just feed this into Tint at runtime
 */

#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <variant>
#include <ranges>
#include <algorithm>

namespace fs = std::filesystem;

struct Macro
{
    std::string key;
    std::string val;
};

struct CompiledEntryPoint
{
    std::string name;
    std::string suffix;
    std::string code;
};

// Add compile options from command line invocations
static std::vector<slang::CompilerOptionEntry> s_CompileOptions;
constexpr static const char* k_allWarningsAsErrorsStr = "all";
constexpr static SlangInt k_WgslTargetIndex = 0;

void AddDefaultCompileOptions()
{
    // catch all warnings as errors, since wgsl is a picky and strict target!
    slang::CompilerOptionEntry optWarningLevel{};
    optWarningLevel.name = slang::CompilerOptionName::WarningLevel;
    optWarningLevel.value.kind = slang::CompilerOptionValueKind::Int;
    // only level enabled by default is "extra". so we add pedantic and all
    optWarningLevel.value.intValue0 = SlangWarningLevel::SLANG_WARNING_LEVEL_PEDANTIC;
    s_CompileOptions.push_back(optWarningLevel);
    optWarningLevel.value.intValue0 = SlangWarningLevel::SLANG_WARNING_LEVEL_ALL;
    s_CompileOptions.push_back(optWarningLevel);
    // all warnings should be errors. also slang takes the string as a view, so its a constexpr static
    // at program scope. note that a lot of these should be caught from how much we've been testing
    // with test_ifft.py, but still I want to be as sure as I can be
    slang::CompilerOptionEntry optAllWarningsAsErrors{};
    optAllWarningsAsErrors.value.kind = slang::CompilerOptionValueKind::String;
    optAllWarningsAsErrors.value.stringValue0 = k_allWarningsAsErrorsStr;
    optAllWarningsAsErrors.name = slang::CompilerOptionName::WarningsAsErrors;
    s_CompileOptions.push_back(optAllWarningsAsErrors);
    // we want to use fast math, we were pretty intentional about our math and used FMA/mad where we could for precision
    slang::CompilerOptionEntry optFloatingPointMode{};
    optFloatingPointMode.value.kind = slang::CompilerOptionValueKind::Int;
    optFloatingPointMode.name = slang::CompilerOptionName::FloatingPointMode;
    optFloatingPointMode.value.intValue0 = static_cast<int32_t>(SlangFloatingPointMode::SLANG_FLOATING_POINT_MODE_FAST);
    s_CompileOptions.push_back(optFloatingPointMode);
    // disabling debug info, we've tested these shaders in SlangPy using scripts/test_ifft.py.
    // that's the nice thing about using Slang, at least - source modifications will carry here too
    slang::CompilerOptionEntry optDebugInfoLevel{};
    optDebugInfoLevel.name = slang::CompilerOptionName::DebugInfoIncludeSource;
    optDebugInfoLevel.value.kind = slang::CompilerOptionValueKind::Int;
    optDebugInfoLevel.value.intValue0 = static_cast<int32_t>(false);
    s_CompileOptions.push_back(optDebugInfoLevel);
    optDebugInfoLevel.name = slang::CompilerOptionName::DebugInformation;
    optDebugInfoLevel.value.kind = slang::CompilerOptionValueKind::Int;
    optDebugInfoLevel.value.intValue0 = SlangDebugInfoLevel::SLANG_DEBUG_INFO_LEVEL_NONE;
    s_CompileOptions.push_back(optDebugInfoLevel);
}

static std::string SlangBlobToStr(slang::IBlob* b)
{
    return b ? std::string{ static_cast<const char*>(b->getBufferPointer()), b->getBufferSize() } : "";
}

static Slang::ComPtr<slang::ISession> MakeSlangSession(
    slang::IGlobalSession* global,
    std::span<const std::string> searchPaths)
{
    std::vector<const char*> paths;
    paths.reserve(searchPaths.size());
    for (const std::string& p : searchPaths)
    {
        paths.push_back(p.c_str());
    }

    slang::TargetDesc td{};
    td.format = SlangCompileTarget::SLANG_WGSL;
    td.profile = global->findProfile("spirv_1_6");
    const std::vector<slang::TargetDesc> targets = {td};

    slang::SessionDesc sd{};
    sd.targets                  = targets.data();
    sd.targetCount              = static_cast<SlangInt>(targets.size());
    sd.preprocessorMacros       = nullptr;
    sd.preprocessorMacroCount   = 0;
    sd.searchPaths              = paths.data();
    sd.searchPathCount          = static_cast<SlangInt>(paths.size());

    if (!s_CompileOptions.empty())
    {
        sd.compilerOptionEntries = s_CompileOptions.data();
        sd.compilerOptionEntryCount = static_cast<SlangInt>(s_CompileOptions.size());
    }
    else
    {
        sd.compilerOptionEntries = nullptr;
        sd.compilerOptionEntryCount = 0;
    }

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(sd, session.writeRef())))
    {
        std::println(stderr, "[slang] createSession failed\n");
    }
    return session;
}

std::string ExtractEntryPointBytecodeWGSL(Slang::ComPtr<slang::IComponentType> program_pointer, SlangInt entryPointIndex)
{
    Slang::ComPtr<slang::IBlob> codeBlob;
    Slang::ComPtr<slang::IBlob> diag;
    if (SLANG_FAILED(program_pointer->getEntryPointCode(entryPointIndex, k_WgslTargetIndex, codeBlob.writeRef(), diag.writeRef())))
    {
        std::println(stderr, "[shader_compiler] ExtractEntryPointBytecodeWGSL({}) failed\n", entryPointIndex);
        if (diag && diag->getBufferSize())
        {
            std::println(stderr, "[shader_compiler] Diagnostics: {}\n", SlangBlobToStr(diag.get()));
        }
        return {};
    }

    return SlangBlobToStr(codeBlob.get());
}

static std::vector<CompiledEntryPoint> CompileModule(slang::IGlobalSession* global, const fs::path& modPath)
{
    std::vector<std::string> searchPaths = {modPath.parent_path().string()};
    std::string stem = modPath.stem().string();

    Slang::ComPtr<slang::ISession> session = MakeSlangSession(global, searchPaths);
    if (!session)
    {
        std::println(stderr, "[shader_compiler] session init failed for {}\n", stem);
        return {};
    }

    Slang::ComPtr<slang::IBlob> diag;
    slang::IModule* mod = session->loadModule(stem.c_str(), diag.writeRef());
    if (!mod)
    {
        std::println(stderr, "loadModule({}): {}\n", stem, SlangBlobToStr(diag.get()));
        return {};
    }
    if (diag && diag->getBufferSize())
    {
        std::println(stderr, "loadModule warnings: {}\n", SlangBlobToStr(diag.get()));
    }

    // gather all entrypoints: primitive approach is to compile and link these one by one, but we can
    // actually do a single link and then query the entrypoints from the module. this is a bit more efficient,
    // and also allows us to get the entrypoint names for free. (DiamondDogs does this)
    const SlangInt epCount = mod->getDefinedEntryPointCount();
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints(epCount);
    // annoying thing: entrypoints are accessed by index after linking, but we can only get the names before linking, so we
    // need to have this map to map the indices back to the name
    std::unordered_map<SlangInt, std::string> entryPointNames;
    for (SlangInt i = 0; i < epCount; ++i)
    {
        Slang::ComPtr<slang::IEntryPoint> ep;
        if (SLANG_FAILED(mod->getDefinedEntryPoint(i, ep.writeRef())))
        {
            std::println(stderr, "[shader_compiler] getDefinedEntryPoint({}) failed for {}\n", i, stem);
            continue;
        }

        assert(!entryPointNames.contains(i));
        entryPointNames[i] = ep->getFunctionReflection()->getName();
        entryPoints[i] = std::move(ep);

    }

    // okay, now to link we create a composite component type with the module and all entrypoints, and then link that
    std::vector<slang::IComponentType*> parts;
    parts.reserve(1 + entryPoints.size());
    parts.push_back(mod);
    for (const auto& ep : entryPoints)
    {
        parts.push_back(ep.get());
    }

    Slang::ComPtr<slang::IComponentType> composite;
    if (SLANG_FAILED(session->createCompositeComponentType(parts.data(), static_cast<SlangInt>(parts.size()), composite.writeRef(), diag.writeRef())))
    {
        std::println(stderr, "[shader_compiler] createComposite failed: {}\n", SlangBlobToStr(diag.get()));
        return {};
    }

    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(composite->link(linked.writeRef(), diag.writeRef())))
    {
        std::println(stderr, "[shader_compiler] composite link failed: {}\n", SlangBlobToStr(diag.get()));
        return {};
    }


    return results;
}

static std::string GetShaderCodeArrayName(std::string_view name, std::string_view suffix)
{
    std::string s{"k_"};
    for (char c : name)
    {
        s += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    for (char c : suffix)
    {
        s += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return s;
}

std::string WriteWgslShaderSourceToCppArray(const std::string& wgslSource, const std::string& arrayName)
{
    std::string cppArray;
    cppArray += "inline constexpr std::string_view " + arrayName + " = R\"WGSL_END(\n";
    cppArray += wgslSource;
    cppArray += ")WGSL_END\";\n";
    return cppArray;
}

static void WriteHeader(
    const std::vector<CompiledEntryPoint>& shaders,
    const fs::path& outPath)
{
    fs::path parentDir = outPath.parent_path();
    if (!fs::exists(parentDir))
    {
        fs::create_directories(parentDir);
    }
    else if (!fs::is_directory(parentDir))
    {
        std::println(stderr, "[shader_compiler] output path parent is not a directory: {}\n", parentDir.string());
        return;
    }

    std::ofstream f{outPath};
    if (!f.is_open())
    {
        std::println(stderr, "[shader_compiler] failed to open output file: {}\n", outPath.string());
        return;
    }

    f << "#pragma once\n"
         "// Auto-generated by shaders/shader_compiler.cpp -- do not edit manually.\n"
         "#include <string_view>\n\n"
         "namespace OceanFFT::Shaders\n{\n\n";

    for (const CompiledEntryPoint& shader : shaders)
    {
        f << WriteWgslShaderSourceToCppArray(shader.code, GetShaderCodeArrayName(shader.name, shader.suffix));
    }

    f << "} // namespace OceanFFT::Shaders\n";
    f.close();

    std::println(stderr, "wrote {} entrypoints -> {}", shaders.size(), outPath.string());
}

int main(int argc, char** argv)
{
    fs::path outputPath;
    std::vector<fs::path> modulePaths;
    std::vector<Macro> defines;
    bool waveVariants = false;

    AddDefaultCompileOptions();

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg{argv[i]};
        if ((arg == "--output" || arg == "-o") && i + 1 < argc)
        {
            outputPath = argv[++i];
        }
        else if (arg.starts_with("--O"))
        {
            // find level of optimization, e.g. --O0, --O1, --O2, --O3, --Os, --Oz
            char level = arg.at(3);
            slang::CompilerOptionEntry optLevel{};
            optLevel.name = slang::CompilerOptionName::Optimization;
            switch (level)
            {
            case '0':
                optLevel.value.intValue0 = SLANG_OPTIMIZATION_LEVEL_NONE;
                break;
            case '1':
                optLevel.value.intValue0 = SLANG_OPTIMIZATION_LEVEL_DEFAULT;
                break;
            case '2':
                optLevel.value.intValue0 = SLANG_OPTIMIZATION_LEVEL_HIGH;
                break;
            case '3':
                optLevel.value.intValue0 = SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
                break;
            }
            s_CompileOptions.push_back(optLevel);
        }
        else if (!arg.starts_with('-'))
        {
            modulePaths.emplace_back(argv[i]);
        }
    }

    // if no optimization option in s_CompilerOptions, add default one that disables all optimizations
    auto hasOpt = std::find_if(
        s_CompileOptions.begin(),
        s_CompileOptions.end(),
        [](const slang::CompilerOptionEntry& opt)
        {
            return opt.name == slang::CompilerOptionName::Optimization;
        });

    if (hasOpt == s_CompileOptions.end())
    {
        slang::CompilerOptionEntry optLevel{};
        optLevel.name = slang::CompilerOptionName::Optimization;
        optLevel.value.intValue0 = SLANG_OPTIMIZATION_LEVEL_NONE;
        s_CompileOptions.push_back(optLevel);
    }

    if (outputPath.empty() || modulePaths.empty())
    {
        std::cerr << "Usage: OceanShaderCompiler --output <header.hpp>"
                     " [-O<level>]..."
                     " <module.slang>...\n";
        return 1;
    }

    Slang::ComPtr<slang::IGlobalSession> global;
    slang::createGlobalSession(global.writeRef());

    std::vector<CompiledEntryPoint> all_entry_points;
    for (auto& module_path : modulePaths)
    {
        std::println(stderr, "[shader_compiler] Compiling module: {}", module_path.string());
        std::vector<CompiledEntryPoint> all_entry_points = CompileModule(global.get(), module_path);
    }

    if (all_entry_points.empty())
    {
        std::cerr << "no entrypoints compiled\n";
        return 1;
    }

    WriteHeader(all_entry_points, outputPath);
    return 0;
}