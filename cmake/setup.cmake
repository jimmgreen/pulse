# Pulse self-extracting setup (replaces Inno Setup). Included from the root
# CMakeLists.txt; the targets live in src/setup/CMakeLists.txt.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/setup ${CMAKE_BINARY_DIR}/setup)
pulse_add_version_resource(pulse_setup "Pulse Setup" "PulseSetup" "PulseSetup.exe")
