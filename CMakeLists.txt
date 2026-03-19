cmake_minimum_required(VERSION 3.20)

project(PolyX VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(POLYX_FBXSDK_ROOT "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9" CACHE PATH "Root directory of the Autodesk FBX SDK" FORCE)
set(POLYX_FBXSDK_INCLUDE_DIR "${POLYX_FBXSDK_ROOT}/include" CACHE PATH "FBX SDK include directory" FORCE)
set(POLYX_FBXSDK_LIBRARY_DEBUG "${POLYX_FBXSDK_ROOT}/lib/x64/debug/libfbxsdk.lib" CACHE FILEPATH "FBX SDK debug library" FORCE)
set(POLYX_FBXSDK_LIBRARY_RELEASE "${POLYX_FBXSDK_ROOT}/lib/x64/release/libfbxsdk.lib" CACHE FILEPATH "FBX SDK release library" FORCE)
set(POLYX_FBXSDK_DLL_DEBUG "${POLYX_FBXSDK_ROOT}/lib/x64/debug/libfbxsdk.dll" CACHE FILEPATH "FBX SDK debug runtime DLL" FORCE)
set(POLYX_FBXSDK_DLL_RELEASE "${POLYX_FBXSDK_ROOT}/lib/x64/release/libfbxsdk.dll" CACHE FILEPATH "FBX SDK release runtime DLL" FORCE)

if(NOT EXISTS "${POLYX_FBXSDK_INCLUDE_DIR}/fbxsdk.h")
  message(FATAL_ERROR "FBX SDK header not found at: ${POLYX_FBXSDK_INCLUDE_DIR}/fbxsdk.h")
endif()

if(NOT EXISTS "${POLYX_FBXSDK_LIBRARY_DEBUG}")
  message(FATAL_ERROR "FBX SDK debug library not found at: ${POLYX_FBXSDK_LIBRARY_DEBUG}")
endif()

if(NOT EXISTS "${POLYX_FBXSDK_LIBRARY_RELEASE}")
  message(FATAL_ERROR "FBX SDK release library not found at: ${POLYX_FBXSDK_LIBRARY_RELEASE}")
endif()

if(NOT EXISTS "${POLYX_FBXSDK_DLL_DEBUG}")
  message(FATAL_ERROR "FBX SDK debug DLL not found at: ${POLYX_FBXSDK_DLL_DEBUG}")
endif()

if(NOT EXISTS "${POLYX_FBXSDK_DLL_RELEASE}")
  message(FATAL_ERROR "FBX SDK release DLL not found at: ${POLYX_FBXSDK_DLL_RELEASE}")
endif()

add_library(AutodeskFBXSDK SHARED IMPORTED GLOBAL)
set_target_properties(AutodeskFBXSDK PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${POLYX_FBXSDK_LIBRARY_DEBUG}"
  IMPORTED_IMPLIB_RELWITHDEBINFO "${POLYX_FBXSDK_LIBRARY_RELEASE}"
  IMPORTED_IMPLIB_RELEASE "${POLYX_FBXSDK_LIBRARY_RELEASE}"
  IMPORTED_LOCATION_DEBUG "${POLYX_FBXSDK_DLL_DEBUG}"
  IMPORTED_LOCATION_RELWITHDEBINFO "${POLYX_FBXSDK_DLL_RELEASE}"
  IMPORTED_LOCATION_RELEASE "${POLYX_FBXSDK_DLL_RELEASE}"
  INTERFACE_INCLUDE_DIRECTORIES "${POLYX_FBXSDK_INCLUDE_DIR}")

target_compile_definitions(AutodeskFBXSDK INTERFACE FBXSDK_SHARED)

add_executable(PolyX
  app/main.cpp
  app/BatchProcessor.cpp
  core/Config.cpp
  core/Logger.cpp
  fbx/FbxLoader.cpp
  uv/UVAnalyzer.cpp
  atlas/AtlasBuilder.cpp
  atlas/TgaLoader.cpp)

target_include_directories(PolyX PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(PolyX PRIVATE AutodeskFBXSDK gdiplus)

if(WIN32)
  target_link_libraries(PolyX PRIVATE ws2_32 winmm version shlwapi)
endif()

add_custom_command(TARGET PolyX POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<IF:$<CONFIG:Debug>,${POLYX_FBXSDK_DLL_DEBUG},${POLYX_FBXSDK_DLL_RELEASE}>"
    "$<TARGET_FILE_DIR:PolyX>")