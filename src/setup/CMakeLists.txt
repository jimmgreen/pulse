# Pulse setup bootstrapper (replaces Inno Setup; see installer-plan).
# A directory of its own so that it can be optimized for size (/O1) without
# overriding the project's /O2 per target (D9025).

foreach(lang C CXX)
    string(REPLACE "/O2" "/O1" CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "/Ob1" CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
endforeach()

# The project adds /W4 for every directory; set it per target here so the
# vendored LZMA SDK can use /W0 without D9025 override warnings.
get_directory_property(setup_dir_options COMPILE_OPTIONS)
list(REMOVE_ITEM setup_dir_options /W4)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${setup_dir_options}")

set(PULSE_LZMA_DIR "${PROJECT_SOURCE_DIR}/third_party/lzma/C")
add_library(pulse_lzma_dec STATIC
    ${PULSE_LZMA_DIR}/7zCrc.c ${PULSE_LZMA_DIR}/7zCrcOpt.c ${PULSE_LZMA_DIR}/Alloc.c
    ${PULSE_LZMA_DIR}/Bra.c ${PULSE_LZMA_DIR}/Bra86.c ${PULSE_LZMA_DIR}/BraIA64.c
    ${PULSE_LZMA_DIR}/CpuArch.c ${PULSE_LZMA_DIR}/Delta.c ${PULSE_LZMA_DIR}/Lzma2Dec.c
    ${PULSE_LZMA_DIR}/LzmaDec.c ${PULSE_LZMA_DIR}/Sha256.c ${PULSE_LZMA_DIR}/Sha256Opt.c
    ${PULSE_LZMA_DIR}/Xz.c ${PULSE_LZMA_DIR}/XzCrc64.c ${PULSE_LZMA_DIR}/XzCrc64Opt.c
    ${PULSE_LZMA_DIR}/XzDec.c)
target_include_directories(pulse_lzma_dec PUBLIC ${PULSE_LZMA_DIR})
target_compile_definitions(pulse_lzma_dec PUBLIC Z7_ST)
target_compile_options(pulse_lzma_dec PRIVATE /W0 /TC)

add_library(pulse_setup_payload STATIC setup_payload.cpp)
target_link_libraries(pulse_setup_payload PUBLIC pulse_lzma_dec)

add_executable(pulse_setup_p0 setup_p0_main.cpp)
target_link_libraries(pulse_setup_p0 PRIVATE pulse_setup_payload)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/setup.ico
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/pack_installer/make_setup_icon.py
            ${PROJECT_SOURCE_DIR}/src/app/pulse.ico ${CMAKE_CURRENT_BINARY_DIR}/setup.ico
    DEPENDS ${PROJECT_SOURCE_DIR}/src/app/pulse.ico ${PROJECT_SOURCE_DIR}/tools/pack_installer/make_setup_icon.py
    VERBATIM)
set_source_files_properties(setup.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/setup.ico)

# Wizard UI + install engine; installed copies run as the uninstaller.
add_executable(pulse_setup WIN32
    setup_runtime_transaction.cpp setup_dialog_versions.cpp setup_dialog_migration.cpp setup_file_locks.cpp setup_main.cpp setup_engine.cpp setup_common.cpp setup_log.cpp setup_options.cpp setup_process.cpp
    setup_registry.cpp setup_service.cpp setup_shortcut.cpp setup_transaction.cpp setup_strings.cpp
    start_pin_registration.cpp
    setup_ui.cpp setup_uninstall.cpp setup.rc setup.manifest ${CMAKE_CURRENT_BINARY_DIR}/setup.ico)
target_include_directories(pulse_setup PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_library(pulse_setup_dialog_bundle STATIC ../common/dialog_bundle.cpp ../common/json_value.cpp)
target_compile_definitions(pulse_setup_dialog_bundle PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_dialog_bundle PRIVATE /W4)
target_link_libraries(pulse_setup_dialog_bundle PUBLIC bcrypt)
target_link_libraries(pulse_setup PRIVATE pulse_setup_dialog_bundle pulse_dialog_auto_registration pulse_setup_payload shell32 ole32 uuid advapi32 user32 gdi32 comctl32
    d2d1 dwrite windowscodecs dwmapi Rstrtmgr)
target_compile_definitions(pulse_setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(pulse_setup PRIVATE /MANIFESTUAC:level='asInvoker')

get_target_property(PULSE_SETUP_CAPTURE_SOURCES pulse_setup SOURCES)
add_executable(pulse_setup_ui_capture WIN32 EXCLUDE_FROM_ALL ${PULSE_SETUP_CAPTURE_SOURCES})
target_include_directories(pulse_setup_ui_capture PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(pulse_setup_ui_capture PRIVATE "$<TARGET_PROPERTY:pulse_setup,LINK_LIBRARIES>")
target_compile_definitions(pulse_setup_ui_capture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX PULSE_SETUP_UI_CAPTURE=1)
target_compile_options(pulse_setup_ui_capture PRIVATE /W4)
target_link_options(pulse_setup_ui_capture PRIVATE /MANIFESTUAC:level='asInvoker')
set_target_properties(pulse_setup_ui_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

foreach(setup_target pulse_lzma_dec pulse_setup_payload pulse_setup_p0 pulse_setup)
    target_compile_options(${setup_target} PRIVATE $<$<CONFIG:Release>:/Gy /Gw>)
endforeach()
foreach(setup_target pulse_setup_payload pulse_setup_p0 pulse_setup)
    target_compile_options(${setup_target} PRIVATE /W4)
endforeach()
foreach(setup_target pulse_setup_p0 pulse_setup)
    set_target_properties(${setup_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_options(${setup_target} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endforeach()

add_executable(pulse_setup_options_test setup_options_test.cpp setup_options.cpp setup_common.cpp)
target_link_libraries(pulse_setup_options_test PRIVATE shell32 ole32 advapi32 user32)
target_compile_definitions(pulse_setup_options_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_options_test PRIVATE /W4)
set_target_properties(pulse_setup_options_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(pulse_setup_file_locks_test setup_file_locks_test.cpp setup_file_locks.cpp)
target_link_libraries(pulse_setup_file_locks_test PRIVATE Rstrtmgr)
target_compile_definitions(pulse_setup_file_locks_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_file_locks_test PRIVATE /W4)
set_target_properties(pulse_setup_file_locks_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(pulse_setup_dialog_migration_test setup_dialog_migration_test.cpp setup_dialog_migration.cpp setup_transaction.cpp setup_file_locks.cpp setup_common.cpp setup_log.cpp)
target_link_libraries(pulse_setup_dialog_migration_test PRIVATE pulse_dialog_auto_registration Rstrtmgr shell32 ole32 advapi32 user32)
target_compile_definitions(pulse_setup_dialog_migration_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_dialog_migration_test PRIVATE /W4)
set_target_properties(pulse_setup_dialog_migration_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(pulse_setup_dialog_versions_test setup_runtime_transaction.cpp setup_service.cpp setup_dialog_versions_test.cpp setup_dialog_versions.cpp setup_dialog_migration.cpp setup_transaction.cpp setup_file_locks.cpp setup_common.cpp setup_log.cpp)
target_link_libraries(pulse_setup_dialog_versions_test PRIVATE pulse_setup_dialog_bundle pulse_dialog_auto_registration shell32 ole32 advapi32 user32 Rstrtmgr)
target_compile_definitions(pulse_setup_dialog_versions_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_dialog_versions_test PRIVATE /W4)
set_target_properties(pulse_setup_dialog_versions_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(pulse_setup_service_test setup_service_test.cpp)
target_link_libraries(pulse_setup_service_test PRIVATE advapi32)
target_compile_definitions(pulse_setup_service_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_service_test PRIVATE /W4)
set_target_properties(pulse_setup_service_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(pulse_setup_install_handoff_test setup_install_handoff_test.cpp setup_common.cpp)
target_link_libraries(pulse_setup_install_handoff_test PRIVATE shell32 ole32 advapi32 user32)
target_compile_definitions(pulse_setup_install_handoff_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_install_handoff_test PRIVATE /W4)
set_target_properties(pulse_setup_install_handoff_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(pulse_setup_shortcut_test EXCLUDE_FROM_ALL setup_shortcut_test.cpp setup_shortcut.cpp setup_common.cpp)
target_link_libraries(pulse_setup_shortcut_test PRIVATE pulse_app_identity shell32 ole32 propsys uuid advapi32 user32)
target_compile_definitions(pulse_setup_shortcut_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_setup_shortcut_test PRIVATE /W4)
set_target_properties(pulse_setup_shortcut_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
