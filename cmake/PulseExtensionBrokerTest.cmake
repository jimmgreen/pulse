add_executable(pulse_extension_broker_test EXCLUDE_FROM_ALL
    src/bench/extension_broker_test.cpp
    src/extension_host/capability_broker.cpp
    src/extension_host/isolation.cpp)
target_compile_features(pulse_extension_broker_test PRIVATE cxx_std_20)
target_compile_options(pulse_extension_broker_test PRIVATE /W4 /WX /permissive- /utf-8)
set_property(TARGET pulse_extension_broker_test PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET pulse_extension_broker_test PROPERTY
    CXX_STANDARD_LIBRARIES "kernel32.lib")
target_link_options(pulse_extension_broker_test PRIVATE
    /NODEFAULTLIB:user32.lib
    /NODEFAULTLIB:gdi32.lib
    /NODEFAULTLIB:shell32.lib)
target_link_libraries(pulse_extension_broker_test PRIVATE
    kernel32
    advapi32
    bcrypt)
