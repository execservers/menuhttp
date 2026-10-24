cmake_minimum_required(VERSION 3.21)
project(menuhttp_inject LANGUAGES CXX)

# OFF removes the logger implementation and compiles out all logging calls.
option(MENUHTTP_WITH_LOGGER "Write detailed menuhttp.log diagnostics" ON)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Build with Visual Studio on Windows (Win32 or x64).")
endif()

include(FetchContent)
FetchContent_Declare(garrysmod_common
  GIT_REPOSITORY https://github.com/danielga/garrysmod_common.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)
FetchContent_GetProperties(garrysmod_common)
if(NOT garrysmod_common_POPULATED)
  FetchContent_Populate(garrysmod_common)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(GMOD_BITNESS 64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(GMOD_BITNESS 32)
else()
  message(FATAL_ERROR "Unsupported Windows architecture")
endif()

add_library(menuhttp_inject SHARED
  src/menuhttp.cpp
  src/inject.cpp
)
if(MENUHTTP_WITH_LOGGER)
  target_sources(menuhttp_inject PRIVATE src/log.cpp)
endif()
target_include_directories(menuhttp_inject PRIVATE "${garrysmod_common_SOURCE_DIR}/include")
target_compile_features(menuhttp_inject PRIVATE cxx_std_17)
# Avoid dependencies on missing VC++ runtime DLLs in the injected process.
set_property(TARGET menuhttp_inject PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(menuhttp_inject PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  MENUHTTP_WITH_LOGGER=$<BOOL:${MENUHTTP_WITH_LOGGER}>
)
target_link_libraries(menuhttp_inject PRIVATE winhttp user32 shell32)
set_target_properties(menuhttp_inject PROPERTIES PREFIX "" OUTPUT_NAME "menuhttp_inject_win${GMOD_BITNESS}")
target_compile_options(menuhttp_inject PRIVATE /W4 /EHsc /permissive-)
