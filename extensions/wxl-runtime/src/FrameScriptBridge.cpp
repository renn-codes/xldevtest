// Restricted FrameScript callback and bootstrap owner for extension DLLs.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"

#include "engine/events/Event.hpp"
#include "game/Script.hpp"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    namespace ev = wxl::events;
    namespace script = wxl::game::script;

    struct FunctionEntry { std::string name; WXL_LuaCFunction function = nullptr; };
    struct ScriptEntry { std::string name; std::string source; };
    struct CVarEntry { std::string name; std::string defaultValue; bool installed = false; };
    struct Registry
    {
        std::mutex mutex;
        std::vector<FunctionEntry> functions;
        std::vector<ScriptEntry> scripts;
        std::vector<CVarEntry> cvars;
    };

    Registry& Entries() { static Registry value; return value; }

    script::ValidateCallbackFn g_originalValidate = nullptr;
    std::atomic_uint32_t g_generation{0};
    void* g_installedState = nullptr;
    uint32_t g_installedGeneration = 0;
    size_t g_installedFunctions = 0;
    size_t g_installedScripts = 0;
    constexpr char kTaintName[] = "WarcraftXL";

    bool IsRegistered(uintptr_t function)
    {
        Registry& entries = Entries();
        std::lock_guard lock(entries.mutex);
        for (const FunctionEntry& entry : entries.functions)
            if (reinterpret_cast<uintptr_t>(entry.function) == function) return true;
        return false;
    }

    void __cdecl ValidateFunctionPointer(uintptr_t function)
    {
        if (IsRegistered(function)) return;
        if (g_originalValidate) g_originalValidate(function);
    }

    void InstallCurrent(bool forceFunctions = false)
    {
        if (!g_originalValidate) return;
        void* state = script::CurrentState();
        if (!state) return;

        const uint32_t generation = g_generation.load(std::memory_order_acquire);
        const bool newState = state != g_installedState;
        if (!newState && generation == g_installedGeneration && !forceFunctions) return;

        std::vector<FunctionEntry> functions;
        std::vector<ScriptEntry> scripts;
        std::vector<CVarEntry> cvars;
        {
            Registry& entries = Entries();
            std::lock_guard lock(entries.mutex);
            functions = entries.functions;
            scripts = entries.scripts;
            cvars = entries.cvars;
        }

        // WoW can clear and rebuild the FrameScript globals during a world transition while
        // retaining the same state address. Re-register native functions in that case, matching
        // the pre-v1.1 lifecycle. Bootstrap scripts only rerun for a genuinely new state so they
        // cannot duplicate frames or event handlers on ordinary map changes.
        size_t functionStart = (newState || forceFunctions) ? 0 : g_installedFunctions;
        size_t scriptStart = newState ? 0 : g_installedScripts;
        if (functionStart > functions.size()) functionStart = 0;
        if (scriptStart > scripts.size()) scriptStart = 0;

        for (const CVarEntry& entry : cvars)
        {
            if (entry.installed) continue;
            void* cvar = script::RegisterCVar(
                entry.name.c_str(), entry.defaultValue.c_str(), true);
            if (!cvar)
            {
                WLOG_ERROR("framescript: native CVar registration failed name='%s'",
                           entry.name.c_str());
                continue;
            }
            Registry& entries = Entries();
            std::lock_guard lock(entries.mutex);
            for (CVarEntry& registered : entries.cvars)
                if (_stricmp(registered.name.c_str(), entry.name.c_str()) == 0)
                {
                    registered.installed = true;
                    break;
                }
        }
        for (size_t i = functionStart; i < functions.size(); ++i)
            script::Register(functions[i].name.c_str(), functions[i].function);
        for (size_t i = scriptStart; i < scripts.size(); ++i)
            script::Execute(scripts[i].source.c_str(), scripts[i].name.c_str(), kTaintName);

        g_installedState = state;
        g_installedGeneration = generation;
        g_installedFunctions = functions.size();
        g_installedScripts = scripts.size();
        WLOG_INFO("framescript: state=%p functions=%zu scripts=%zu cvars=%zu",
                  state, functions.size(), scripts.size(), cvars.size());
    }

    void __cdecl OnUpdate(void*, const void*)
    {
        __try { InstallCurrent(false); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN("framescript: state changed during bridge installation");
        }
    }

    void __cdecl OnWorldEnter(void*, const void*)
    {
        __try { InstallCurrent(true); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN("framescript: state changed during world-enter installation");
        }
    }

    int __cdecl RegisterFunction(const char* name, WXL_LuaCFunction function)
    {
        if (!name || !*name || !function) return 0;
        Registry& entries = Entries();
        {
            std::lock_guard lock(entries.mutex);
            for (const FunctionEntry& entry : entries.functions)
                if (entry.name == name) return entry.function == function ? 1 : 0;
            entries.functions.push_back(FunctionEntry{name, function});
        }
        g_generation.fetch_add(1, std::memory_order_release);
        return 1;
    }

    int __cdecl RegisterScript(const char* name, const char* source)
    {
        if (!name || !*name || !source || !*source) return 0;
        Registry& entries = Entries();
        {
            std::lock_guard lock(entries.mutex);
            for (const ScriptEntry& entry : entries.scripts)
                if (entry.name == name) return entry.source == source ? 1 : 0;
            entries.scripts.push_back(ScriptEntry{name, source});
        }
        g_generation.fetch_add(1, std::memory_order_release);
        return 1;
    }

    int __cdecl Execute(const char* source, const char* name)
    {
        if (!source || !*source) return 0;
        int ok = 0;
        __try
        {
            void* state = script::CurrentState();
            if (state)
            {
                script::Execute(source, name && *name ? name : "WarcraftXL", kTaintName);
                ok = 1;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        return ok;
    }

    int __cdecl RegisterCVar(const char* name, const char* defaultValue)
    {
        if (!name || !*name || !defaultValue) return 0;
        const unsigned char first = static_cast<unsigned char>(*name);
        if (!(std::isalpha(first) || first == '_')) return 0;
        for (const unsigned char ch : std::string(name))
            if (!(std::isalnum(ch) || ch == '_' || ch == '.' || ch == '-')) return 0;

        Registry& entries = Entries();
        {
            std::lock_guard lock(entries.mutex);
            for (const CVarEntry& entry : entries.cvars)
                if (_stricmp(entry.name.c_str(), name) == 0)
                    return entry.defaultValue == defaultValue ? 1 : 0;
            entries.cvars.push_back(CVarEntry{name, defaultValue, false});
        }
        g_generation.fetch_add(1, std::memory_order_release);
        return 1;
    }

    const WXL_FrameScriptApi g_frameScriptApi{
        sizeof(WXL_FrameScriptApi), WXL_FRAME_SCRIPT_API_VERSION,
        &RegisterFunction, &RegisterScript, &Execute, &RegisterCVar,
    };
}

const WXL_FrameScriptApi* wxl_runtime::FrameScriptApi() { return &g_frameScriptApi; }

bool wxl_runtime::InstallFrameScriptBridge()
{
    if (!HookAttach("FrameScript.ValidateFunctionPointer",
                    script::kValidateCallbackSeam,
                    &ValidateFunctionPointer, &g_originalValidate))
        return false;
    g_api->Subscribe(uint32_t(ev::Event::OnUpdate), &OnUpdate, nullptr);
    g_api->Subscribe(uint32_t(ev::Event::OnWorldEnter), &OnWorldEnter, nullptr);
    OnUpdate(nullptr, nullptr);
    return true;
}
