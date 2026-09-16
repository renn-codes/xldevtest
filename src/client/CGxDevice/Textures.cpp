// Texture create/upload detours: publish BLP-load / texture-upload events and guard the mip-source singleton.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "config.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "engine/diag/AssetProfile.hpp"

#include "common/Log.hpp"
#include "offsets/engine/Gx.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    namespace ev    = wxl::events;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace aprof = wxl::runtime::assetprof;

    gxoff::TextureCreateFn g_origTexCreate = nullptr;
    gxoff::TextureUpdateFn g_origTexUpdate = nullptr;
    std::atomic<uint32_t>  g_textureUpdateFaults{ 0 };

    // Keep SEH in a POD-only leaf. TextureUpdate invokes the texture's completion callback before it
    // returns; a late font-atlas/cache callback can retain a row/tree pointer whose owner was rebuilt
    // during world entry. Letting that AV escape kills the client from inside the texture completion callback.
    bool SafeTextureUpdate(void* pendingUpdate) noexcept
    {
        __try
        {
            g_origTexUpdate(pendingUpdate);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /**
     * @brief Detours texture upload, emitting OnTextureUpload before the device update.
     *
     * The one argument is what the client's own call sites pass: the object its rectangle lookup
     * returned, which carries the texture and the rectangle together. Nothing here can name a width or
     * a height without opening that object, and inventing them off the stack is how a fault in this
     * very function came to be reported as an overrun of a rectangle nobody had asked for.
     *
     * The mip source the upload reads is a process-wide singleton (kMipTablePtr is a pointer whose
     * buffer holds the per-mip source pointers; kMipTableValid gates the read). A build fills it with
     * raw aliases into its transient IO buffer, then uploads. Two ways that singleton turns into an
     * access-violation use-after-free, both fixed without ever clearing kMipTableValid
     * (which would route atlas icons through a source callback that has no self-heal and blank them):
     *  - a NESTED build run while this upload is mid-copy overwrites the table and frees its buffer; the
     *    async-drain serializer (streaming) snapshots and restores the table around the nested build it
     *    runs, so the outer upload keeps reading its own live aliases.
     *  - a TRUNCATED mip chain (common in custom-map BLPs) only fills the low slots, so the dimension-
     *    driven upload would read a prior build's freed alias left in a high slot. Clearing the table
     *    after each upload leaves the next build's unfilled slots at 0, which the upload's source-not-null
     *    blit guard skips. Done after the original consumed the table, so the live upload is unaffected.
     * @param pendingUpdate the texture-and-rectangle pair the client is sending.
     */
    void __cdecl hkTexUpdate(void* pendingUpdate)
    {
        ev::TextureUploadArgs a{ pendingUpdate };
        ev::Emit(ev::Event::OnTextureUpload, &a);
        const uint64_t started = aprof::Now();
        const bool completed = SafeTextureUpdate(pendingUpdate);
        if (!completed)
        {
            const uint32_t faults = g_textureUpdateFaults.fetch_add(1, std::memory_order_relaxed) + 1;
            if (faults == 1 || (faults & (faults - 1)) == 0)
                WLOG_WARN("texture: a native upload faulted and was skipped (faults=%u pending=%p)",
                          faults, pendingUpdate);
        }
        else if (started)
        {
            aprof::Record(aprof::Phase::TextureUpload, aprof::Now() - started, 0);
        }
        if (auto* tbl = *reinterpret_cast<uint32_t**>(gxoff::kMipTablePtr))
            std::memset(tbl, 0, gxoff::kMipTableSlots * sizeof(uint32_t));
    }

    /**
     * @brief Detours the by-name texture create, emitting OnBlpLoad after the request resolves.
     *
     * Fires on every reference (returns the cached handle on a hit), so the event carries the requested
     * name and a subscriber can watch for one specific BLP.
     * @param name    requested texture path (full virtual path).
     * @param flags   native load flags.
     * @param status  native status out-pointer.
     * @param flags2  native load flags.
     * @return the resolved texture handle (null on failure).
     */
    void* __cdecl hkTexCreate(const char* name, uint32_t flags, int* status, uint32_t flags2)
    {
        const uint64_t started = aprof::Now();
        void* handle = g_origTexCreate(name, flags, status, flags2);
        if (started) aprof::Record(aprof::Phase::TextureRequest, aprof::Now() - started);

        ev::BlpLoadArgs a{ name, handle };
        ev::Emit(ev::Event::OnBlpLoad, &a);

        return handle;
    }

    bool InstallTextures()
    {
        wxl::hook::Install("TextureUpdate", gxoff::kTextureUpdate, &hkTexUpdate, &g_origTexUpdate);
        wxl::hook::Install("TextureCreate", gxoff::kTextureCreate, &hkTexCreate, &g_origTexCreate);
        return true;
    }
}

WXL_REGISTER_FEATURE("textures", true, InstallTextures)
