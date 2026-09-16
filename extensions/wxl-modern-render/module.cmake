# wxl-render-modern: the post-process effects run a D3D12 backend on the proxy's shared device. The DLL target
# needs the module's own src/ on the include path (effects in src/effects/** include "gpu/Framework.hpp" etc.
# by module-root path, not relative to themselves) and the d3d12 import library (for D3D12SerializeRootSignature;
# every other D3D12 call goes through the ID3D12Device handed over by the proxy via WxlD3D12Device). The vendor
# shaders are pulled in by relative path from each effect, so they need no extra include directory. dxgi is
# header-only here (DXGI_FORMAT enums), so no extra link is needed; d3dcompiler is already linked by the core.
#
# Linking the "d3d9" CMake target (not the Windows SDK's own d3d9.lib -- CMake resolves the name to our
# own target first) pulls in that DLL's import library, resolving WxlD3D12Device/WxlD3D12DrainDebug/
# WxlGetSsaaFactor/WxlSetSsaaFactor (see src/gpu/Proxy.hpp) against the proxy's own exports -- a separate
# DLL boundary from the WXL_Api table, which only ever connects to WarcraftXL.dll.
#
# This file is included by the root CMakeLists with the extension's own target (wxl_ext_name)
# already defined -- not WarcraftXL, which is a separate DLL this extension doesn't link against.
target_include_directories(${wxl_ext_name} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")
target_link_libraries(${wxl_ext_name} PRIVATE d3d12 d3d9)
