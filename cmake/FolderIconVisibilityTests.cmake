set(folder_visibility_baseline "${CMAKE_CURRENT_SOURCE_DIR}/bench_data/folder-icon-flicker-20261126/original/src/ui")
if(EXISTS "${folder_visibility_baseline}/folder_thumbnail_cache.h")
    get_target_property(folder_visibility_sources pulse_folder_thumbnail_cache_test SOURCES)
    list(REMOVE_ITEM folder_visibility_sources src/ui/folder_thumbnail_cache.cpp)
    add_executable(pulse_folder_visibility_baseline EXCLUDE_FROM_ALL
        ${folder_visibility_sources} "${folder_visibility_baseline}/folder_thumbnail_cache.cpp")
    foreach(prop IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS)
        get_target_property(value pulse_folder_thumbnail_cache_test ${prop})
        if(value)
            set_property(TARGET pulse_folder_visibility_baseline PROPERTY ${prop} "${value}")
        endif()
    endforeach()
    target_include_directories(pulse_folder_visibility_baseline PRIVATE src/ui)
    target_compile_definitions(pulse_folder_visibility_baseline PRIVATE
        PULSE_FOLDER_VISIBILITY_BASELINE=1
        PULSE_FOLDER_HEADER="${folder_visibility_baseline}/folder_thumbnail_cache.h")
endif()
