cmake_minimum_required(VERSION 3.20)

project(JoystickTesting LANGUAGES CXX RC)

set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/src)
set(RESOURCE_DIR ${CMAKE_SOURCE_DIR}/res)
set(INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)

set(RESOURCE_FILE ${RESOURCE_DIR}/res.rc)

file(GLOB SOURCE_FILES CONFIGURE_DEPENDS ${SOURCE_DIR}/*.cpp)

add_executable(JoystickTesting WIN32
    ${SOURCE_FILES}
    ${RESOURCE_FILE}
)

target_include_directories(JoystickTesting PRIVATE ${INCLUDE_DIR})

target_compile_features(JoystickTesting PRIVATE cxx_std_20)

target_link_libraries(JoystickTesting PRIVATE
    comctl32
    dinput8
    dxguid
    ole32
    oleaut32
    shell32
    wbemuuid
    winhttp
)

target_link_options(JoystickTesting PRIVATE "/MANIFEST:NO")

target_compile_definitions(JoystickTesting PRIVATE
    UNICODE _UNICODE
)

# If you want static CRT (/MT) uncomment:
set_property(TARGET JoystickTesting PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
