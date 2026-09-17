cmake_minimum_required(VERSION 3.23)

project(OpenXRayAndroidHost LANGUAGES C CXX)

get_filename_component(OPENXRAY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(XRAY_SOURCE_DIR "${OPENXRAY_ROOT}")
list(APPEND CMAKE_MODULE_PATH "${OPENXRAY_ROOT}/cmake")
include(GNUInstallDirs)
include(utils)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${OPENXRAY_ROOT}/Externals/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/SDL2")

include(FetchContent)
include(ExternalProject)
FetchContent_Declare(openxray_lzo
    URL "https://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz"
    URL_HASH SHA1=4924676a9bae5db58ef129dc1cebce3baa3c4b5d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR openxray-no-upstream-cmake
)
FetchContent_MakeAvailable(openxray_lzo)
file(GLOB LZO_SOURCES CONFIGURE_DEPENDS "${openxray_lzo_SOURCE_DIR}/src/*.c")
add_library(openxray_lzo STATIC ${LZO_SOURCES})
add_library(LZO::LZO ALIAS openxray_lzo)
target_include_directories(openxray_lzo PUBLIC "${openxray_lzo_SOURCE_DIR}/include")
set_target_properties(openxray_lzo PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Keep the first audio dependency reproducible. A shared OpenAL Soft library
# preserves the LGPL replacement boundary and provides the same OpenAL::OpenAL
# target consumed by xrSound/xrEngine later.
set(LIBTYPE SHARED CACHE STRING "" FORCE)
set(ALSOFT_DLOPEN OFF CACHE BOOL "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_UPDATE_BUILD_VERSION OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OBOE OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OPENSL ON CACHE BOOL "" FORCE)
set(ALSOFT_REQUIRE_OPENSL ON CACHE BOOL "" FORCE)
# NDK r29 Clang accepts -Wfunction-effects, but its nonblocking inference has
# false positives for OpenAL Soft's 32-bit size_t literals on armeabi-v7a.
# Pre-seed the upstream feature probe to disable only this experimental check.
set(HAVE_WFUNCTION_EFFECTS FALSE CACHE INTERNAL "" FORCE)
FetchContent_Declare(openxray_openal_soft
    URL "https://github.com/kcat/openal-soft/archive/refs/tags/1.25.2.tar.gz"
    URL_HASH SHA256=fb27e5839aa11f0e5b9d33756965291fad5d6909ab928ea1f796f4a1a6877894
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(openxray_openal_soft)

# Multimedia formats used by xrSound, xrEngine video playback, and xrCore's
# image helpers. Keep all source revisions and archive digests pinned so the
# two Android ABIs are built from exactly the same upstream code.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxray_ogg
    URL "https://github.com/xiph/ogg/archive/refs/tags/v1.3.6.tar.gz"
    URL_HASH SHA256=95b643da661155d79db9de2fca55daed3a8d491039829def246aacb3d9201c81
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(openxray_ogg)
add_library(Ogg::Ogg ALIAS ogg)
set_target_properties(ogg PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The Vorbis 1.3.7 CMake project insists on finding an installed Ogg package.
# Define the two libraries OpenXRay consumes directly from the official source
# list instead, using the in-tree pinned Ogg target above.
FetchContent_Declare(openxray_vorbis
    URL "https://github.com/xiph/vorbis/archive/refs/tags/v1.3.7.tar.gz"
    URL_HASH SHA256=270c76933d0934e42c5ee0a54a36280e2d87af1de3cc3e584806357e237afd13
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR openxray-no-upstream-cmake
)
FetchContent_MakeAvailable(openxray_vorbis)
set(OPENXRAY_VORBIS_SOURCES
    mdct.c smallft.c block.c envelope.c window.c lsp.c lpc.c analysis.c
    synthesis.c psy.c info.c floor1.c floor0.c res0.c mapping0.c registry.c
    codebook.c sharedbook.c lookup.c bitrate.c
)
list(TRANSFORM OPENXRAY_VORBIS_SOURCES PREPEND "${openxray_vorbis_SOURCE_DIR}/lib/")
add_library(openxray_vorbis STATIC ${OPENXRAY_VORBIS_SOURCES})
add_library(Vorbis::Vorbis ALIAS openxray_vorbis)
target_include_directories(openxray_vorbis
    PUBLIC "${openxray_vorbis_SOURCE_DIR}/include"
    PRIVATE "${openxray_vorbis_SOURCE_DIR}/lib"
)
target_link_libraries(openxray_vorbis PUBLIC Ogg::Ogg PRIVATE m)
set_target_properties(openxray_vorbis PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(openxray_vorbisfile STATIC "${openxray_vorbis_SOURCE_DIR}/lib/vorbisfile.c")
add_library(Vorbis::VorbisFile ALIAS openxray_vorbisfile)
target_include_directories(openxray_vorbisfile PUBLIC "${openxray_vorbis_SOURCE_DIR}/include")
target_link_libraries(openxray_vorbisfile PUBLIC Vorbis::Vorbis)
set_target_properties(openxray_vorbisfile PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Theora 1.2.0 still ships Autotools/SCons only. OpenXRay only needs playback,
# so build the portable C decoder source set and omit encoder/CLI code and the
# generated legacy ARM assembly. Clang can optimize this portable path for
# arm64-v8a and armeabi-v7a without assembly-time ABI assumptions.
FetchContent_Declare(openxray_theora
    URL "https://github.com/xiph/theora/archive/refs/tags/v1.2.0.tar.gz"
    URL_HASH SHA256=e0c35771b425c32a052ffb358a2aed14219340ab850b48dc85b01939c0513a31
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR openxray-no-upstream-cmake
)
FetchContent_MakeAvailable(openxray_theora)
set(OPENXRAY_THEORA_DECODER_SOURCES
    apiwrapper.c bitpack.c decapiwrapper.c decinfo.c decode.c dequant.c
    fragment.c huffdec.c idct.c info.c internal.c quant.c state.c
)
list(TRANSFORM OPENXRAY_THEORA_DECODER_SOURCES PREPEND "${openxray_theora_SOURCE_DIR}/lib/")
add_library(openxray_theora STATIC ${OPENXRAY_THEORA_DECODER_SOURCES})
add_library(Theora::Theora ALIAS openxray_theora)
target_include_directories(openxray_theora
    PUBLIC "${openxray_theora_SOURCE_DIR}/include"
    PRIVATE "${openxray_theora_SOURCE_DIR}/lib"
)
target_link_libraries(openxray_theora PUBLIC Ogg::Ogg PRIVATE m)
set_target_properties(openxray_theora PROPERTIES POSITION_INDEPENDENT_CODE ON)

# libjpeg-turbo deliberately rejects add_subdirectory() integration. Build it
# as an isolated Android CMake project and expose its installed static library
# through the conventional JPEG::JPEG target consumed by xrCore.
set(OPENXRAY_JPEG_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/openxray-jpeg-install")
file(MAKE_DIRECTORY "${OPENXRAY_JPEG_INSTALL_DIR}/include")
ExternalProject_Add(openxray_libjpeg_turbo_project
    URL "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz"
    URL_HASH SHA256=6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    INSTALL_DIR "${OPENXRAY_JPEG_INSTALL_DIR}"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DANDROID_ABI=${ANDROID_ABI}"
        "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
        "-DANDROID_STL=${ANDROID_STL}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        "-DENABLE_SHARED=OFF"
        "-DENABLE_STATIC=ON"
        "-DWITH_SIMD=OFF"
        "-DWITH_TURBOJPEG=OFF"
        "-DWITH_TOOLS=OFF"
        "-DWITH_TESTS=OFF"
    BUILD_BYPRODUCTS "${OPENXRAY_JPEG_INSTALL_DIR}/lib/libjpeg.a"
)
add_library(openxray_jpeg STATIC IMPORTED GLOBAL)
add_dependencies(openxray_jpeg openxray_libjpeg_turbo_project)
set_target_properties(openxray_jpeg PROPERTIES
    IMPORTED_LOCATION "${OPENXRAY_JPEG_INSTALL_DIR}/lib/libjpeg.a"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENXRAY_JPEG_INSTALL_DIR}/include"
)
add_library(JPEG::JPEG ALIAS openxray_jpeg)

set(MEMORY_ALLOCATOR standard)
set(JPEG_FOUND TRUE)
query_git_info(XRAY_GIT_SHA XRAY_GIT_BRANCH)
add_subdirectory("${OPENXRAY_ROOT}/src/utils/xrMiscMath" "${CMAKE_CURRENT_BINARY_DIR}/xrMiscMath")
add_subdirectory("${OPENXRAY_ROOT}/src/xrCore" "${CMAKE_CURRENT_BINARY_DIR}/xrCore")
add_dependencies(xrCore openxray_libjpeg_turbo_project)

# LuaJIT's buildvm/minilua utilities execute on the Windows build host, while
# the generated VM and runtime compile with the NDK for the selected Android
# ABI. Externals/LuaJIT-proj contains the cross-build split used here.
set(XRAY_USE_LUAJIT ON CACHE BOOL "" FORCE)
set(LUAJIT_DIR "${OPENXRAY_ROOT}/Externals/LuaJIT/src" CACHE PATH "" FORCE)
set(LUAJIT_BUILD_APP OFF CACHE BOOL "" FORCE)
if (ANDROID_ABI STREQUAL "armeabi-v7a")
    # LuaJIT's 32-bit ARM buildvm layout relies on zero-length GNU C arrays,
    # which cannot be represented by the native MSVC host compiler. The LuaJIT
    # interpreter and all Lua/FFI APIs remain available for the compatibility
    # ABI; arm64-v8a retains the JIT backend.
    set(LUAJIT_DISABLE_JIT ON CACHE BOOL "" FORCE)
else()
    set(LUAJIT_DISABLE_JIT OFF CACHE BOOL "" FORCE)
endif()
set(LUA_USE_POSIX ON CACHE BOOL "" FORCE)
set(LUA_USE_DLOPEN ON CACHE BOOL "" FORCE)

# LuaJIT generates target tables with two small executables that must run on
# the Windows build host. Prefer a portable LLVM-MinGW toolchain when Visual
# Studio Build Tools are not installed; Android code itself still uses NDK
# Clang. The versioned directory lives beside the Android SDK/toolchains and is
# intentionally outside the source tree.
file(GLOB OPENXRAY_HOST_LLVM_MINGW_CANDIDATES LIST_DIRECTORIES TRUE
    "${OPENXRAY_ROOT}/../.toolchains/llvm-mingw-*-ucrt-x86_64")
if (OPENXRAY_HOST_LLVM_MINGW_CANDIDATES)
    list(SORT OPENXRAY_HOST_LLVM_MINGW_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    list(GET OPENXRAY_HOST_LLVM_MINGW_CANDIDATES 0 OPENXRAY_HOST_LLVM_MINGW_ROOT)
    set(OPENXRAY_HOST_LLVM_MINGW_ROOT "${OPENXRAY_HOST_LLVM_MINGW_ROOT}" CACHE PATH
        "Portable LLVM-MinGW root used for LuaJIT host generators" FORCE)
endif()
if (EXISTS "${OPENXRAY_ROOT}/../.toolchains/tcc-0.9.27-win64-bin/tcc/tcc.exe")
    set(OPENXRAY_HOST_TCC64
        "${OPENXRAY_ROOT}/../.toolchains/tcc-0.9.27-win64-bin/tcc/tcc.exe" CACHE FILEPATH
        "TinyCC executable used for 64-bit LuaJIT host generators" FORCE)
endif()
if (EXISTS "${OPENXRAY_ROOT}/../.toolchains/tcc-0.9.27-win32-bin/tcc/tcc.exe")
    set(OPENXRAY_HOST_TCC32
        "${OPENXRAY_ROOT}/../.toolchains/tcc-0.9.27-win32-bin/tcc/tcc.exe" CACHE FILEPATH
        "TinyCC executable used for 32-bit LuaJIT host generators" FORCE)
endif()
add_subdirectory("${OPENXRAY_ROOT}/Externals/LuaJIT-proj" "${CMAKE_CURRENT_BINARY_DIR}/LuaJIT")

add_subdirectory("${OPENXRAY_ROOT}/Externals/luabind" "${CMAKE_CURRENT_BINARY_DIR}/luabind")
add_subdirectory("${OPENXRAY_ROOT}/Externals/xrLuaFix" "${CMAKE_CURRENT_BINARY_DIR}/xrLuaFix")
add_subdirectory("${OPENXRAY_ROOT}/Externals/OPCODE" "${CMAKE_CURRENT_BINARY_DIR}/OPCODE")
add_subdirectory("${OPENXRAY_ROOT}/Externals/ode" "${CMAKE_CURRENT_BINARY_DIR}/ODE")
add_subdirectory("${OPENXRAY_ROOT}/Externals/GameSpy" "${CMAKE_CURRENT_BINARY_DIR}/GameSpy")
add_subdirectory("${OPENXRAY_ROOT}/Externals/imgui-proj" "${CMAKE_CURRENT_BINARY_DIR}/ImGui")
add_subdirectory("${OPENXRAY_ROOT}/src/Layers/xrAPI" "${CMAKE_CURRENT_BINARY_DIR}/xrAPI")
add_subdirectory("${OPENXRAY_ROOT}/src/xrScriptEngine" "${CMAKE_CURRENT_BINARY_DIR}/xrScriptEngine")
add_subdirectory("${OPENXRAY_ROOT}/src/xrCDB" "${CMAKE_CURRENT_BINARY_DIR}/xrCDB")
add_subdirectory("${OPENXRAY_ROOT}/src/xrMaterialSystem" "${CMAKE_CURRENT_BINARY_DIR}/xrMaterialSystem")
add_subdirectory("${OPENXRAY_ROOT}/src/xrNetServer" "${CMAKE_CURRENT_BINARY_DIR}/xrNetServer")
add_subdirectory("${OPENXRAY_ROOT}/src/xrSound" "${CMAKE_CURRENT_BINARY_DIR}/xrSound")
add_subdirectory("${OPENXRAY_ROOT}/src/xrEngine" "${CMAKE_CURRENT_BINARY_DIR}/xrEngine")
add_subdirectory("${OPENXRAY_ROOT}/src/xrParticles" "${CMAKE_CURRENT_BINARY_DIR}/xrParticles")
add_subdirectory("${OPENXRAY_ROOT}/src/Layers/xrRenderPC_GL" "${CMAKE_CURRENT_BINARY_DIR}/xrRenderPC_GL")
add_subdirectory("${OPENXRAY_ROOT}/src/xrAICore" "${CMAKE_CURRENT_BINARY_DIR}/xrAICore")
add_subdirectory("${OPENXRAY_ROOT}/src/xrUICore" "${CMAKE_CURRENT_BINARY_DIR}/xrUICore")
add_subdirectory("${OPENXRAY_ROOT}/src/xrGameSpy" "${CMAKE_CURRENT_BINARY_DIR}/xrGameSpy")
add_subdirectory("${OPENXRAY_ROOT}/src/xrPhysics" "${CMAKE_CURRENT_BINARY_DIR}/xrPhysics")
add_subdirectory("${OPENXRAY_ROOT}/src/xrGame" "${CMAKE_CURRENT_BINARY_DIR}/xrGame")

# Compile this small ABI probe as part of xrGame. libmain.so calls it at
# startup, which makes the game archive a real runtime dependency while the
# still-desktop-oriented renderer entry points remain disconnected.
target_sources(xrGame PRIVATE AndroidGameModuleProbe.cpp AndroidGameModuleProbe.hpp)

# Compile the native Vulkan bootstrap shaders with the glslc shipped in the
# pinned NDK, then embed the resulting SPIR-V into libmain.so. This keeps the
# shader sources reviewable and avoids runtime filesystem/asset dependencies.
find_program(OPENXRAY_ANDROID_GLSLC glslc
    HINTS
        "${CMAKE_ANDROID_NDK}/shader-tools/windows-x86_64"
        "${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64"
        "${CMAKE_ANDROID_NDK}/shader-tools/darwin-x86_64"
        "${CMAKE_ANDROID_NDK}/shader-tools/darwin-arm64"
    NO_DEFAULT_PATH
    REQUIRED
)
set(OPENXRAY_ANDROID_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(OPENXRAY_ANDROID_EMBED_SPIRV "${CMAKE_CURRENT_LIST_DIR}/EmbedSpirv.cmake")

function(openxray_embed_android_shader output_header source_file variable_name)
    get_filename_component(shader_name "${source_file}" NAME)
    set(spirv_file "${OPENXRAY_ANDROID_GENERATED_DIR}/${shader_name}.spv")
    set(header_file "${OPENXRAY_ANDROID_GENERATED_DIR}/${output_header}")
    add_custom_command(
        OUTPUT "${spirv_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${OPENXRAY_ANDROID_GENERATED_DIR}"
        COMMAND "${OPENXRAY_ANDROID_GLSLC}" --target-env=vulkan1.0 -O "${source_file}" -o "${spirv_file}"
        DEPENDS "${source_file}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${header_file}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${spirv_file}"
            "-DOUTPUT=${header_file}"
            "-DVARIABLE=${variable_name}"
            -P "${OPENXRAY_ANDROID_EMBED_SPIRV}"
        DEPENDS "${spirv_file}" "${OPENXRAY_ANDROID_EMBED_SPIRV}"
        VERBATIM
    )
    set(${output_header} "${header_file}" PARENT_SCOPE)
endfunction()

openxray_embed_android_shader(AndroidBootstrapVertSpv.hpp
    "${CMAKE_CURRENT_LIST_DIR}/shaders/bootstrap.vert" OpenXRayBootstrapVertSpv)
openxray_embed_android_shader(AndroidBootstrapFragSpv.hpp
    "${CMAKE_CURRENT_LIST_DIR}/shaders/bootstrap.frag" OpenXRayBootstrapFragSpv)
openxray_embed_android_shader(AndroidWorldVertSpv.hpp
    "${CMAKE_CURRENT_LIST_DIR}/shaders/world.vert" OpenXRayWorldVertSpv)
openxray_embed_android_shader(AndroidWorldFragSpv.hpp
    "${CMAKE_CURRENT_LIST_DIR}/shaders/world.frag" OpenXRayWorldFragSpv)

foreach(engine_target IN ITEMS
    xrMiscMath xrCore xrLuaJIT xrLuabind xrLuaFix xrOPCODE xrODE GameSpy-oxr xrImGui xrAPI xrScriptEngine
    xrCDB xrMaterialSystem xrNetServer xrSound xrEngine xrParticles xrRender_GL
    xrAICore xrUICore xrGameSpy xrPhysics xrGame
)
    target_compile_definitions(${engine_target} PRIVATE
        XRAY_STATIC_BUILD
        XRAY_EXCEPTIONS=1
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<CONFIG:Debug>:DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
    )
    set_target_properties(${engine_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

add_library(main SHARED
    android_main.cpp
    AndroidSafBridge.cpp
    AndroidSafBridge.hpp
    AndroidGameModuleProbe.hpp
    AndroidVulkanBootstrap.cpp
    AndroidVulkanBootstrap.hpp
    AndroidVulkanRenderDevice.cpp
    AndroidVulkanRenderDevice.hpp
    AndroidVulkanRenderServices.cpp
    AndroidVulkanRenderServices.hpp
    AndroidVulkanRendererModule.cpp
    AndroidVulkanRendererModule.hpp
    "${AndroidBootstrapVertSpv.hpp}"
    "${AndroidBootstrapFragSpv.hpp}"
    "${AndroidWorldVertSpv.hpp}"
    "${AndroidWorldFragSpv.hpp}"
)
# Keep the desktop GL renderer graph as a separate static milestone until its
# GL-facing entry points have Android-safe runtime implementations. xrGame is
# linked below and contributes the Stage-6 ABI probe to libmain.so.
add_dependencies(main xrRender_GL)
target_compile_features(main PRIVATE cxx_std_17)
target_compile_definitions(main PRIVATE
    XRAY_STATIC_BUILD
    $<$<CONFIG:Debug>:DEBUG>
)
target_include_directories(main PRIVATE
    "${OPENXRAY_ANDROID_GENERATED_DIR}"
    "${OPENXRAY_ROOT}/src"
    "${OPENXRAY_ROOT}/Externals/gli"
    "${OPENXRAY_ROOT}/Externals/gli/external"
    "${OPENXRAY_ROOT}/Externals/OPCODE"
    "${OPENXRAY_ROOT}/Externals/ode/include"
    "${OPENXRAY_ROOT}/Externals/GameSpy/src/GameSpy"
)

find_library(ANDROID_LIBRARY android REQUIRED)
find_library(LOG_LIBRARY log REQUIRED)
find_library(VULKAN_LIBRARY vulkan REQUIRED)

target_link_libraries(main PRIVATE
    xrRender_GL
    xrCore
    SDL2::SDL2
    OpenAL::OpenAL
    Ogg::Ogg
    Vorbis::Vorbis
    Vorbis::VorbisFile
    Theora::Theora
    JPEG::JPEG
    xrScriptEngine
    xrLuaFix
    xrOPCODE
    xrODE
    GameSpy-oxr
    xrImGui
    xrGame
    ${ANDROID_LIBRARY}
    ${LOG_LIBRARY}
    ${VULKAN_LIBRARY}
)
