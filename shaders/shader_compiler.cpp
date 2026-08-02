/**
 * OceanShaderCompiler
 * Loads Slang modules, enumerates defined entrypoints, compiles each to WGSL,
 * and bakes all variants into a C++ header.
 *
 * Usage:
 *   OceanShaderCompiler --output <header.hpp> [--wave-variants]
 *                       [--define K[=V]]... <module.slang>...
 *
 * --wave-variants: compile every entrypoint twice — once normally (_WaveOps)
 *                  and once with OCEAN_FFT_DISABLE_WAVE_OPS=1 (_NoWaveOps) —
 *                  and emit a Get<Name>Wgsl(bool waveOps) selector for each pair.
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

namespace fs = std::filesystem;

struct Macro
{
    std::string key;
    std::string val;
};

struct CompiledEP
{
    std::string name;    // entrypoint name, e.g. "Radix2_IFFT"
    std::string suffix;  // "", "_WaveOps", or "_NoWaveOps"
    std::string wgsl;
};

// Add compile options from command line invocations
static std::vector<slang::CompilerOptionEntry> s_CompileOptions;

void AddDefaultCompileOptions()
{
    // catch all warnings as errors, since wgsl is a picky and strict target!
    slang::CompilerOptionEntry optAllWarningsAsErrors{};
    // slang seems to take this as a view, and it's intended to be used inline when compiling, 
    // so we can just use a static string literal here and it should be fine
    slang::CompilerOptionEntry optWarningLevel{};
    optWarningLevel.name = slang::CompilerOptionName::WarningLevel;
    optWarningLevel.value.kind = slang::CompilerOptionValueKind::Int;
    optWarningLevel.value.intValue0 = SlangWarningLevel::SLANG_WARNING_LEVEL_PEDANTIC;
    s_CompileOptions.push_back(optWarningLevel);
    optWarningLevel.value.intValue0 = SlangWarningLevel::SLANG_WARNING_LEVEL_ALL;
    s_CompileOptions.push_back(optWarningLevel);
    
    constexpr const char* k_allWarningsAsErrors = "all";
    optAllWarningsAsErrors.value.kind = slang::CompilerOptionValueKind::String;
    optAllWarningsAsErrors.value.stringValue0 = k_allWarningsAsErrors;
    optAllWarningsAsErrors.name = slang::CompilerOptionName::WarningsAsErrors;
    // Disabled for now! some of it's suggested changes broke compile.
    s_CompileOptions.push_back(optAllWarningsAsErrors);
    // we want to use fast math, we were pretty intentional about our math and used FMA/mad where we could for precision
    slang::CompilerOptionEntry optFloatingPointMode{};
    optFloatingPointMode.value.kind = slang::CompilerOptionValueKind::Int;
    optFloatingPointMode.name = slang::CompilerOptionName::FloatingPointMode;
    optFloatingPointMode.value.intValue0 = static_cast<int32_t>(SlangFloatingPointMode::SLANG_FLOATING_POINT_MODE_PRECISE);
    s_CompileOptions.push_back(optFloatingPointMode);
    // try to disable name mangling, since we want to be able to read our compiled-in shaders
    // and they'll be passing through Tint at runtime anyways (correctness beats perf, for web)
    slang::CompilerOptionEntry optDisableNameMangling{}; // note this is experimental
    optDisableNameMangling.value.kind = slang::CompilerOptionValueKind::Int;
    optDisableNameMangling.name = slang::CompilerOptionName::NoMangle;
    optDisableNameMangling.value.intValue0 = static_cast<int32_t>(true);
    //s_CompileOptions.push_back(optDisableNameMangling);
    slang::CompilerOptionEntry optDebugInfoLevel{};
    optDebugInfoLevel.name = slang::CompilerOptionName::DebugInfoIncludeSource;
    optDebugInfoLevel.value.kind = slang::CompilerOptionValueKind::Int;
    optDebugInfoLevel.value.intValue0 = static_cast<int32_t>(true);
    s_CompileOptions.push_back(optDebugInfoLevel);
    optDebugInfoLevel.name = slang::CompilerOptionName::DebugInformation;
    optDebugInfoLevel.value.kind = slang::CompilerOptionValueKind::Int;
    optDebugInfoLevel.value.intValue0 = SlangDebugInfoLevel::SLANG_DEBUG_INFO_LEVEL_MAXIMAL;
    s_CompileOptions.push_back(optDebugInfoLevel);
}

static std::string blobStr(slang::IBlob* b)
{
    return b ? std::string{static_cast<const char*>(b->getBufferPointer()), b->getBufferSize()} : "";
}

static Slang::ComPtr<slang::ISession> makeSession(
    slang::IGlobalSession* global,
    std::span<const Macro> macros,
    std::span<const std::string> searchPaths)
{
    std::vector<slang::PreprocessorMacroDesc> mds;
    mds.reserve(macros.size());
    for (const Macro& m : macros)
    {
        mds.push_back({m.key.c_str(), m.val.c_str()});
    }

    std::vector<const char*> paths;
    paths.reserve(searchPaths.size());
    for (const std::string& p : searchPaths)
    {
        paths.push_back(p.c_str());
    }

    slang::TargetDesc td{};
    td.format = SLANG_WGSL;
    td.profile = global->findProfile("sm_6_10");

    slang::SessionDesc sd{};
    sd.targets          = &td;
    sd.targetCount      = 1;
    sd.preprocessorMacros      = mds.data();
    sd.preprocessorMacroCount  = static_cast<SlangInt>(mds.size());
    sd.searchPaths      = paths.data();
    sd.searchPathCount  = static_cast<SlangInt>(paths.size());
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


static std::optional<std::string> compileEP(
    slang::ISession* session,
    slang::IModule* mod,
    slang::IEntryPoint* ep)
{
    Slang::ComPtr<slang::IBlob> diag, code;
    Slang::ComPtr<slang::IComponentType> composite, linked;

    slang::IComponentType* parts[] = {mod, ep};
    if (SLANG_FAILED(session->createCompositeComponentType(parts, 2, composite.writeRef(), diag.writeRef())))
    {
        std::println(stderr, "[slang] createComposite failed: {}\n", blobStr(diag.get()));
        return {};
    }

    if (SLANG_FAILED(composite->link(linked.writeRef(), diag.writeRef())))
    {
        std::println(stderr, "[slang] composite link failed: {}\n", blobStr(diag.get()));
        return {};
    }

    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), diag.writeRef())))
    {
        std::println(stderr, "[slang] getEntryPointCode failed: {}\n", blobStr(diag.get()));
        return {};
    }

    return blobStr(code.get());
}

static std::vector<CompiledEP> compileModule(
    slang::IGlobalSession* global,
    const fs::path& modPath,
    std::span<const Macro> macros,
    std::string_view suffix)
{
    std::vector<std::string> searchPaths = {modPath.parent_path().string()};
    std::string stem = modPath.stem().string();

    Slang::ComPtr<slang::ISession> session = makeSession(global, macros, searchPaths);
    if (!session)
    {
        std::println(stderr, "[shader_compiler] session init failed for {}\n", stem);
        return {};
    }

    Slang::ComPtr<slang::IBlob> diag;
    slang::IModule* mod = session->loadModule(stem.c_str(), diag.writeRef());
    if (!mod)
    {
        std::println(stderr, "loadModule({}): {}\n", stem, blobStr(diag.get()));
        return {};
    }
    if (diag && diag->getBufferSize())
    {
        std::println(stderr, "loadModule warnings: {}\n", blobStr(diag.get()));
    }

    std::vector<CompiledEP> results;
    const SlangInt epCount = mod->getDefinedEntryPointCount();
    for (SlangInt i = 0; i < epCount; ++i)
    {
        Slang::ComPtr<slang::IEntryPoint> ep;
        if (SLANG_FAILED(mod->getDefinedEntryPoint(i, ep.writeRef())))
        {
            continue;
        }

        const char* name = ep->getFunctionReflection()->getName();
        std::println(stderr, "[shader_compiler] Compiling entry point | [Module] {} [Entry Point] {}{}", stem, name, suffix);

        std::optional<std::string> wgsl = compileEP(session.get(), mod, ep.get());
        if (wgsl)
        {
            results.push_back({name, std::string{suffix}, std::move(*wgsl)});
        }
        else
        {
            std::println(stderr, "  FAILED: {}{}\n", name, suffix);
        }
    }
    return results;
}

static std::string makeIdent(std::string_view name, std::string_view suffix)
{
    std::string s{"k_"};
    for (char c : name)   s += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    for (char c : suffix) s += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return s;
}

static void writeHeader(const std::vector<CompiledEP>& shaders, const fs::path& outPath)
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

    constexpr std::string_view DELIM = "WGSL_END";

    for (const CompiledEP& s : shaders)
    {
        f << "inline constexpr std::string_view " << makeIdent(s.name, s.suffix)
          << " = R\"" << DELIM << "(\n" << s.wgsl << ")" << DELIM << "\";\n\n";
    }

    // Selectors for wave variant pairs
    std::unordered_map<std::string, std::array<bool, 2>> seen; // [hasWave, hasNoWave]
    for (const CompiledEP& s : shaders)
    {
        if (s.suffix == "_WaveOps")   seen[s.name][0] = true;
        if (s.suffix == "_NoWaveOps") seen[s.name][1] = true;
    }

    for (const auto& [name, flags] : seen)
    {

        if (!flags[0] || !flags[1])
        {
            continue;
        }

        f << "inline std::string_view Get" << name << "Wgsl(bool waveOps) noexcept\n{\n"
          << "    return waveOps ? " << makeIdent(name, "_WaveOps")
          << " : " << makeIdent(name, "_NoWaveOps") << ";\n"
          << "}\n\n";
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
        else if (arg == "--wave-variants")
        {
            waveVariants = true;
        }
        else if ((arg == "--define" || arg == "-D") && i + 1 < argc)
        {
            std::string def{argv[++i]};
            auto eq = def.find('=');
            defines.push_back(eq != std::string::npos
                ? Macro{def.substr(0, eq), def.substr(eq + 1)}
                : Macro{def, "1"});
        }
        else if (arg.starts_with("-D") && arg.size() > 2)
        {
            std::string def{arg.substr(2)};
            auto eq = def.find('=');
            defines.push_back(eq != std::string::npos
                ? Macro{def.substr(0, eq), def.substr(eq + 1)}
                : Macro{def, "1"});
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
    auto hasOpt = std::find_if(s_CompileOptions.begin(), s_CompileOptions.end(),
        [](const slang::CompilerOptionEntry& opt) {
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
                     " [--wave-variants] [--define K[=V]]..."
                     " <module.slang>...\n";
        return 1;
    }

    Slang::ComPtr<slang::IGlobalSession> global;
    slang::createGlobalSession(global.writeRef());

    std::vector<CompiledEP> all;
    for (auto& mod : modulePaths)
    {
        std::println(stderr, "[shader_compiler] Compiling module: {}", mod.string());
        if (waveVariants)
        {
            auto wave = compileModule(global.get(), mod, defines, "_WaveOps");
            std::vector<Macro> noWaveDefs = defines;
            noWaveDefs.push_back({"OCEAN_FFT_DISABLE_WAVE_OPS", "1"});
            auto nowave = compileModule(global.get(), mod, noWaveDefs, "_NoWaveOps");
            for (auto& e : wave)   all.push_back(std::move(e));
            for (auto& e : nowave) all.push_back(std::move(e));
        }
        else
        {
            auto compiled = compileModule(global.get(), mod, defines, "");
            for (auto& e : compiled) all.push_back(std::move(e));
        }
    }

    if (all.empty())
    {
        std::cerr << "no entrypoints compiled\n";
        return 1;
    }

    writeHeader(all, outputPath);
    return 0;
}