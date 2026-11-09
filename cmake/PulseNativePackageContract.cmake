# Verification is shared by startup store reads and native module execution.
get_property(native_image_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(native_image_target IN LISTS native_image_targets)
    get_target_property(native_image_sources ${native_image_target} SOURCES)
    if("${native_image_sources}" MATCHES "package_store\\.cpp")
        if(NOT "${native_image_sources}" MATCHES "native_image_verify\\.cpp")
            target_sources(${native_image_target} PRIVATE src/app/extensions/native_image_verify.cpp)
        endif()
        target_link_libraries(${native_image_target} PRIVATE bcrypt)
    endif()
endforeach()

add_executable(pulse_extension_native_contract_test EXCLUDE_FROM_ALL
    src/bench/extension_native_contract_test.cpp
    src/app/extensions/manifest.cpp
    src/app/extensions/package_store.cpp
    src/app/extensions/package_signature.cpp
    src/app/extensions/native_image_verify.cpp
    src/app/extensions/ui_tree.cpp
    src/common/json_value.cpp)
target_compile_features(pulse_extension_native_contract_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_extension_native_contract_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603)
target_compile_options(pulse_extension_native_contract_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_extension_native_contract_test PRIVATE bcrypt)
