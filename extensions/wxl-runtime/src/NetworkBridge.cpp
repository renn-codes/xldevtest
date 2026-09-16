// Shared single-owner transport for WarcraftXL's post-3.3.5 opcode namespace.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"

#include "engine/events/Event.hpp"
#include "game/Network.hpp"
#include "wxl/NetworkApi.h"
#include "wxl/NetworkObserverApi.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace ev = wxl::events;
    namespace native = wxl::game::network;
    namespace netoff = wxl::offsets::game::network;

    constexpr uint16_t kFirstWxlOpcode = 0x051F;
    constexpr uint32_t kMaxPayload = 10235;
    constexpr size_t kPendingPacketQuota = 4u * 1024u * 1024u;
    constexpr size_t kMaxPendingPackets = 128;
    constexpr size_t kDispatchByteBudget = 512u * 1024u;
    constexpr size_t kMaxDispatchPackets = 32;

    struct ServerEntry
    {
        std::string name;
        WXL_NetworkPacketHandler handler = nullptr;
        void* user = nullptr;
    };

    struct ObserverEntry
    {
        std::string name;
        WXL_NetworkPacketHandler handler = nullptr;
        void* user = nullptr;
    };

    struct Definitions
    {
        std::mutex mutex;
        std::map<uint16_t, std::string> client;
        std::map<uint16_t, ServerEntry> server;
        std::map<uint16_t, std::vector<ObserverEntry>> observers;
    };

    struct PendingPacket
    {
        uint16_t opcode = 0;
        std::vector<uint8_t> payload;
    };

    Definitions& OpcodeDefinitions()
    {
        static Definitions definitions;
        return definitions;
    }

    netoff::ProcessMessageFn g_originalProcessMessage = nullptr;
    std::mutex g_pendingMutex;
    std::deque<PendingPacket> g_pendingPackets;
    size_t g_pendingBytes = 0;
    uint32_t g_lastQueueDropLogMs = 0;

    bool IsRegisteredClientOpcode(uint16_t opcode)
    {
        Definitions& definitions = OpcodeDefinitions();
        const std::lock_guard lock(definitions.mutex);
        return definitions.client.contains(opcode);
    }

    bool IsRegisteredServerOpcode(uint16_t opcode)
    {
        Definitions& definitions = OpcodeDefinitions();
        const std::lock_guard lock(definitions.mutex);
        return definitions.server.contains(opcode) || definitions.observers.contains(opcode);
    }

    void QueuePacket(uint16_t opcode, const uint8_t* data, size_t size)
    {
        PendingPacket packet;
        packet.opcode = opcode;
        try
        {
            if (size) packet.payload.assign(data, data + size);
        }
        catch (...)
        {
            WLOG_WARN("network: allocation failed while queuing opcode=0x%04X bytes=%zu",
                      opcode, size);
            return;
        }

        bool queued = false;
        {
            const std::lock_guard lock(g_pendingMutex);
            if (g_pendingPackets.size() < kMaxPendingPackets &&
                size <= kPendingPacketQuota - g_pendingBytes)
            {
                g_pendingBytes += size;
                g_pendingPackets.push_back(std::move(packet));
                queued = true;
            }
        }

        if (!queued)
        {
            const uint32_t now = GetTickCount();
            if (!g_lastQueueDropLogMs || now - g_lastQueueDropLogMs >= 5000u)
            {
                g_lastQueueDropLogMs = now;
                WLOG_WARN("network: game-thread receive queue full; opcode=0x%04X dropped",
                          opcode);
            }
        }
    }

    bool TakePending(PendingPacket& packet)
    {
        const std::lock_guard lock(g_pendingMutex);
        if (g_pendingPackets.empty()) return false;
        packet = std::move(g_pendingPackets.front());
        g_pendingPackets.pop_front();
        const size_t size = packet.payload.size();
        g_pendingBytes = size <= g_pendingBytes ? g_pendingBytes - size : 0;
        return true;
    }

    void Dispatch(PendingPacket& packet)
    {
        ServerEntry entry;
        std::vector<ObserverEntry> observers;
        {
            Definitions& definitions = OpcodeDefinitions();
            const std::lock_guard lock(definitions.mutex);
            const auto found = definitions.server.find(packet.opcode);
            if (found != definitions.server.end()) entry = found->second;
            const auto observed = definitions.observers.find(packet.opcode);
            if (observed != definitions.observers.end()) observers = observed->second;
        }

        if (entry.handler)
        {
            try
            {
                entry.handler(packet.payload.data(),
                              static_cast<uint32_t>(packet.payload.size()), entry.user);
            }
            catch (...)
            {
                WLOG_ERROR("network: handler '%s' threw for opcode=0x%04X",
                           entry.name.c_str(), packet.opcode);
            }
        }

        for (const ObserverEntry& observer : observers)
        {
            if (!observer.handler) continue;
            try
            {
                observer.handler(packet.payload.data(),
                                 static_cast<uint32_t>(packet.payload.size()), observer.user);
            }
            catch (...)
            {
                WLOG_ERROR("network: observer '%s' threw for opcode=0x%04X",
                           observer.name.c_str(), packet.opcode);
            }
        }
    }

    void OnUpdate(void*, const void*)
    {
        size_t dispatchedBytes = 0;
        for (size_t count = 0; count < kMaxDispatchPackets; ++count)
        {
            PendingPacket packet;
            if (!TakePending(packet)) break;
            dispatchedBytes += packet.payload.size();
            Dispatch(packet);
            if (dispatchedBytes >= kDispatchByteBudget) break;
        }
    }

    // __fastcall with an unused edx slot emulates __thiscall on a free function (MSVC only allows
    // __thiscall on an actual member function) -- client is "this" in ECX, the rest go on the stack.
    void __fastcall ProcessMessage(void* client, void* /*edx*/, int time,
                                    native::ClientPacket* packet, int unused)
    {
        if (packet && packet->buffer && packet->read <= packet->size &&
            packet->size - packet->read >= sizeof(uint16_t))
        {
            const uint8_t* wire = packet->buffer + packet->read;
            uint16_t opcode = 0;
            std::memcpy(&opcode, wire, sizeof(opcode));
            if (IsRegisteredServerOpcode(opcode))
            {
                QueuePacket(opcode, wire + sizeof(opcode),
                            packet->size - packet->read - sizeof(opcode));
                return;
            }
        }

        if (g_originalProcessMessage)
            g_originalProcessMessage(client, time, packet, unused);
    }

    int __cdecl RegisterClientOpcode(uint16_t opcode, const char* name)
    {
        if (opcode < kFirstWxlOpcode || !name || !*name) return 0;
        Definitions& definitions = OpcodeDefinitions();
        const std::lock_guard lock(definitions.mutex);
        const auto [found, inserted] = definitions.client.emplace(opcode, name);
        return inserted || found->second == name;
    }

    int __cdecl RegisterServerOpcode(uint16_t opcode, const char* name,
                                     WXL_NetworkPacketHandler handler, void* user)
    {
        if (opcode < kFirstWxlOpcode || !name || !*name || !handler) return 0;
        Definitions& definitions = OpcodeDefinitions();
        const std::lock_guard lock(definitions.mutex);
        const auto [found, inserted] = definitions.server.emplace(
            opcode, ServerEntry{std::string(name), handler, user});
        return inserted || (found->second.name == name &&
                            found->second.handler == handler &&
                            found->second.user == user);
    }

    int __cdecl RegisterServerObserver(uint16_t opcode, const char* name,
                                        WXL_NetworkPacketHandler handler, void* user)
    {
        if (opcode < kFirstWxlOpcode || !name || !*name || !handler) return 0;
        Definitions& definitions = OpcodeDefinitions();
        const std::lock_guard lock(definitions.mutex);
        std::vector<ObserverEntry>& observers = definitions.observers[opcode];
        for (const ObserverEntry& entry : observers)
            if (entry.name == name)
                return entry.handler == handler && entry.user == user ? 1 : 0;
        observers.push_back(ObserverEntry{std::string(name), handler, user});
        return 1;
    }

    int __cdecl Send(uint16_t opcode, const uint8_t* payload, uint32_t payloadSize)
    {
        if (!IsRegisteredClientOpcode(opcode) || payloadSize > kMaxPayload ||
            (payloadSize && !payload))
        {
            WLOG_WARN("network: refused opcode=0x%04X bytes=%u", opcode, payloadSize);
            return 0;
        }

        const size_t wireSize = sizeof(uint32_t) + payloadSize;
        auto* wire = new (std::nothrow) uint8_t[wireSize];
        auto* packet = new (std::nothrow) native::ClientPacket{};
        if (!wire || !packet)
        {
            delete[] wire;
            delete packet;
            return 0;
        }

        const uint32_t wireOpcode = opcode;
        std::memcpy(wire, &wireOpcode, sizeof(wireOpcode));
        if (payloadSize) std::memcpy(wire + sizeof(wireOpcode), payload, payloadSize);

        __try
        {
            native::Initialize(packet);
            packet->buffer = wire;
            packet->size = static_cast<uint32_t>(wireSize);
            packet->allocation = static_cast<uint32_t>(wireSize);
            native::Finalize(packet);
            native::Send(packet);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            delete[] wire;
            delete packet;
            WLOG_WARN("network: native send fault opcode=0x%04X", opcode);
            return 0;
        }
    }

    WXL_NetworkApi g_networkApi = {
        sizeof(WXL_NetworkApi),
        WXL_NETWORK_API_VERSION,
        &RegisterClientOpcode,
        &RegisterServerOpcode,
        &Send,
    };

    WXL_NetworkObserverApi g_networkObserverApi = {
        sizeof(WXL_NetworkObserverApi),
        WXL_NETWORK_OBSERVER_API_VERSION,
        &RegisterServerObserver,
    };
}

namespace wxl_runtime
{
    bool InstallNetworkBridge()
    {
        if (!g_api->HookAttachByName("Network.ProcessMessage",
                                     reinterpret_cast<void*>(&ProcessMessage),
                                     reinterpret_cast<void**>(&g_originalProcessMessage),
                                     WXL_HOOK_DEFAULT_PRIORITY))
            return false;

        g_api->Subscribe(static_cast<uint32_t>(ev::Event::OnUpdate), &OnUpdate, nullptr);
        g_api->PublishInterface("wxl.network", WXL_NETWORK_API_VERSION, &g_networkApi);
        g_api->PublishInterface("wxl.network-observer", WXL_NETWORK_OBSERVER_API_VERSION,
                                 &g_networkObserverApi);
        WLOG_INFO("network: custom opcode transport published");
        return true;
    }
}
