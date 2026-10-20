# Static CRT; LibRaw stays a replaceable DLL under its existing LGPL/CDDL terms.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
if(PORT STREQUAL "libraw")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
set(VCPKG_BUILD_TYPE release)
