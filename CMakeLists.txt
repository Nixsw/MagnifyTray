cmake_minimum_required(VERSION 3.24)

project(MagnifyTray VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT MSVC OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    message(FATAL_ERROR "MagnifyTray supports only x64 builds. Configure with -A x64.")
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()
option(
    MAGNIFYTRAY_ENABLE_LIVE_TESTS
    "Register the screen-changing live Magnifier acceptance test with CTest"
    OFF)

add_executable(MagnifyTray WIN32
    resources/MagnifyTray.rc
    src/main.cpp
    src/magnifier_controller.cpp
    src/tray_application.cpp
    src/win32_process_backend.cpp
)

target_compile_features(MagnifyTray PRIVATE cxx_std_17)
target_compile_definitions(MagnifyTray PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(MagnifyTray PRIVATE /W4 /WX /permissive- /utf-8 /guard:cf)
    target_link_options(MagnifyTray PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT /MANIFEST:NO)
endif()

target_link_libraries(MagnifyTray PRIVATE shell32 user32)

install(TARGETS MagnifyTray RUNTIME DESTINATION .)

add_executable(MagnifyTrayTests
    src/magnifier_controller.cpp
    tests/controller_tests.cpp
)

target_compile_features(MagnifyTrayTests PRIVATE cxx_std_17)
target_include_directories(MagnifyTrayTests PRIVATE src)

if(MSVC)
    target_compile_options(MagnifyTrayTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()

add_test(NAME MagnifyTrayTests COMMAND MagnifyTrayTests)

add_executable(Win32BackendTests
    src/win32_process_backend.cpp
    tests/file_identity_tests.cpp
)

add_executable(MagnifyNameFixture
    tests/process_fixture.cpp
)
set_target_properties(MagnifyNameFixture PROPERTIES OUTPUT_NAME Magnify)
add_dependencies(Win32BackendTests MagnifyNameFixture)

target_compile_features(Win32BackendTests PRIVATE cxx_std_17)
target_include_directories(Win32BackendTests PRIVATE src)

if(MSVC)
    target_compile_options(Win32BackendTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()

target_link_libraries(Win32BackendTests PRIVATE shell32 user32)

add_test(NAME Win32BackendTests COMMAND Win32BackendTests)

add_executable(AppLifecycleTests
    tests/app_lifecycle_tests.cpp
)

target_compile_features(AppLifecycleTests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(AppLifecycleTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()

target_link_libraries(AppLifecycleTests PRIVATE user32)
add_dependencies(AppLifecycleTests MagnifyTray)
add_test(NAME AppLifecycleTests COMMAND AppLifecycleTests)

add_executable(LiveAcceptanceTests
    src/win32_process_backend.cpp
    tests/live_acceptance_tests.cpp
)

target_compile_features(LiveAcceptanceTests PRIVATE cxx_std_17)
target_compile_definitions(LiveAcceptanceTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(LiveAcceptanceTests PRIVATE /W4 /WX /permissive- /utf-8)
endif()

target_include_directories(LiveAcceptanceTests PRIVATE src)
target_link_libraries(LiveAcceptanceTests PRIVATE shell32 user32)
add_dependencies(LiveAcceptanceTests MagnifyTray)

if(MAGNIFYTRAY_ENABLE_LIVE_TESTS)
    add_test(NAME LiveAcceptanceTests COMMAND LiveAcceptanceTests)
    set_tests_properties(LiveAcceptanceTests PROPERTIES LABELS "live")
endif()
