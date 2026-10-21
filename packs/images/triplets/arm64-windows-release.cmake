# Native build-time tools, matching vcpkg's x64-windows-release host triplet.
# The pinned vcpkg tree does not provide an arm64-windows-release triplet.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
