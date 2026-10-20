# Native ARM64 counterpart of x64-windows-imgpack: static CRT, with the
# LGPL libraries kept as separate, replaceable DLLs. Decoder features are unchanged.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
if(PORT MATCHES "^(libheif|libde265)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
set(VCPKG_BUILD_TYPE release)
