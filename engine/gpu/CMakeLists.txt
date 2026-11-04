# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
# bbport: optional instruction set level (BB_MARCH=x86-64-v2 in build.sh; empty: the compiler's
# default, x86-64). v2 (SSE4.2, POPCNT, SSSE3, CX16) is a subset of what the game's own code
# already requires (probe.c check_cpu: AVX, BMI1, LZCNT, POPCNT, SSE4.2, MOVBE), so it shuts out
# no CPU that can run the game; v3 would (it adds AVX2/BMI2/FMA).
set(BB_MARCH "" CACHE STRING "-march for bbgpu and its static libraries (empty: default)")
if (BB_MARCH)
    add_compile_options(-march=${BB_MARCH})
endif()
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    # GCC writes .gcda files into BB_PGO_DIR, clang default_<pool>.profraw files (build.sh merges
    # them into default.profdata with llvm-profdata).
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # bbport: clang (MSYS2 CLANG64 on Windows) reads one merged profile. It has no
    # -fprofile-partial-training; -pgso=false keeps code the profile never ran from being
    # optimized for size instead (the closest equivalent). Changed functions are compiled
    # without profile data (warnings off).
    add_compile_options(-fprofile-use=${BB_PGO_DIR}/default.profdata
                        -Wno-profile-instr-missing -Wno-profile-instr-out-of-date
                        -Wno-profile-instr-unprofiled -Wno-backend-plugin
                        "SHELL:-mllvm -pgso=false")
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(tsl-robin-map REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
find_package(Zydis REQUIRED)
if (WIN32)
    # bbport: Windows (MSYS2 MINGW64). bbgpu is a static library linked into bb-probe.exe
    # (a DLL cannot leave the runtime_* symbols undefined). Packages MSYS2 does not ship
    # (magic_enum, miniz, xbyak) are fetched; VMA is header-only there.
    include(FetchContent)
    find_package(magic_enum QUIET)
    if (NOT TARGET magic_enum::magic_enum)
        FetchContent_Declare(magic_enum GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
            GIT_TAG v0.9.7 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(magic_enum)
    endif()
    find_package(miniz QUIET)
    if (NOT TARGET miniz::miniz)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
        set(BUILD_NO_STDIO OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(miniz GIT_REPOSITORY https://github.com/richgel999/miniz.git
            GIT_TAG 3.1.0 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(miniz)
        if (NOT TARGET miniz::miniz)
            add_library(miniz::miniz ALIAS miniz)
        endif()
    endif()
    find_package(VulkanMemoryAllocator CONFIG QUIET)
    if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
        find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
        add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
        set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${VMA_INCLUDE_DIR})
    endif()
else()
    find_package(magic_enum REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    find_package(miniz REQUIRED)
endif()
# xbyak (header-only; the shader recompiler's flatten pass), fetched when not installed.
find_package(xbyak QUIET)
if (NOT TARGET xbyak::xbyak)
    include(FetchContent)
    FetchContent_Declare(xbyak GIT_REPOSITORY https://github.com/herumi/xbyak.git
        GIT_TAG v7.24.2 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(xbyak)
endif()
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)
if (WIN32)
    # AMD's FidelityFX sources rely on MSVC's transitive includes (memset, floor, placement new,
    # swprintf_s) on _WIN32.
    foreach(ffx_target IN ITEMS ffx_vulkan_portable ffx_vulkan_fsr3_vk_backend_1_1_4
            ffx_vulkan_fsr3_host_1_1_4 ffx_vulkan_fsr4_v07_vulkan ffx_vulkan_fsr4_v07_assets
            ffx_vulkan_fsr3_vk_framegeneration_3_1_6 ffx_vulkan_fsr3_framegen_3_1_6_scaffold
            ffx_vulkan_fsr3_sdk_core_3_1_5_scaffold)
        if (TARGET ${ffx_target})
            target_compile_options(${ffx_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdio>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cwchar>" "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstring>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cmath>" "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include new>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include mutex>")
        endif()
    endforeach()
endif()

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    set(INCLUDES "")
    if (FILENAME MATCHES "^native_motion_blur")
        list(APPEND INCLUDES ${SHADER_SRC}/native_motion_blur.glsl)
    endif()
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} ${INCLUDES} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(gtao.frag gtao_frag)
add_host_shader(ssr.frag ssr_frag)
add_host_shader(ssr_temporal.comp ssr_temporal_resolve_comp RESOLVE=1)
add_host_shader(ssr_temporal.comp ssr_temporal_copy_comp COPY=1)
add_host_shader(ssr_trace.comp ssr_trace_comp)
add_host_shader(contact_shadow.comp contact_shadow_comp)
add_host_shader(rt_shadow.comp rt_shadow_comp)
add_host_shader(rt_unproject.comp rt_unproject_comp)
add_host_shader(point_shadow_ray.comp point_shadow_ray_comp)
add_host_shader(ssgi_gather_ray.comp ssgi_gather_ray_comp)
add_host_shader(rt_shadow_filter.comp rt_shadow_filter_comp)
add_host_shader(rt_lab.comp rt_lab_comp)
add_host_shader(point_shadow.comp point_shadow_comp)
add_host_shader(point_volumetric.comp point_volumetric_comp)
add_host_shader(native_motion_blur.frag native_motion_blur_frag)
add_host_shader(native_motion_blur_test.comp native_motion_blur_test_comp)
add_host_shader(volumetric_light.comp volumetric_light_integrate_comp INTEGRATE=1)
add_host_shader(volumetric_light.comp volumetric_light_repair_comp REPAIR=1)
add_host_shader(volumetric_light.comp volumetric_light_tint_comp TINT=1)
add_host_shader(volumetric_light.comp volumetric_light_composite_comp COMPOSITE=1)
add_host_shader(volumetric_light.comp volumetric_light_filter_comp FILTER=1)
add_host_shader(ssgi.comp ssgi_prep_comp PREP=1)
add_host_shader(ssgi.comp ssgi_gather_comp GATHER=1)
add_host_shader(ssgi.comp ssgi_denoise_comp DENOISE=1)
add_host_shader(ssgi.comp ssgi_composite_comp COMPOSITE=1)
add_host_shader(hdr_output.frag hdr_output_frag)
add_host_shader(hdr_native.comp hdr_native_luma_comp LUMA=1)
add_host_shader(hdr_native.comp hdr_native_stats_comp STATS=1)
add_host_shader(hdr_native.comp hdr_native_snapshot_comp STATS=1 SNAPSHOT=1)
add_host_shader(hdr_native.comp hdr_native_exposure_comp EXPOSURE=1)
add_host_shader(hdr_native.comp hdr_native_gain_comp GAIN=1)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_debug.comp upscale_debug_comp)
add_host_shader(upscale_debug.comp upscale_debug_ldr_comp LDR_OUTPUT=1)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(frame_gen_compose.comp frame_gen_compose_comp)
add_host_shader(frame_gen_depth.comp frame_gen_depth_comp)
add_host_shader(dlss_ui_alpha.comp dlss_ui_alpha_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(indirect_clamp.comp indirect_clamp_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
if (WIN32)
    # These optional experiments use Linux dma-buf mappings and SIGTRAP hooks (the libGnm entry
    # hooks, bbport_gnm_hooks.cpp, are inline patches and build on Windows too). Keep the
    # section-backed Win32 memory and its vectored crash diagnostics; do not emulate dma-buf.
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "/bbport_(guest_memory|guest_hooks|free_check|heap_sites)\\.cpp$")
    # The PC memory model has Windows files of its own (_win.cpp: host memory import, a vectored
    # exception handler for the guest hooks).
    set(BBGPU_LIBRARY_TYPE STATIC)
else()
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "_win\\.cpp$")
    set(BBGPU_LIBRARY_TYPE SHARED)
endif()
add_library(bbgpu ${BBGPU_LIBRARY_TYPE} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp)
add_dependencies(bbgpu host_shaders)
add_subdirectory(bbnet)
target_link_libraries(bbgpu PUBLIC bbnet)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts); party names in Flesh and Blood by JoannaVu.
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf"
    BB_CJK_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LanternMenuSC.ttf"
    BB_NAME_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/FleshAndBlood.ttf"
    BB_KEYCAP_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/fonts/Roboto-Medium.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LanternMenuSC.ttf;${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/FleshAndBlood.ttf")
# Keyboard prompts' keycaps (shim/engine/keycap_art.cpp): Roboto Medium (Apache License 2.0).
set_source_files_properties(shim/engine/keycap_art.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/misc/fonts/Roboto-Medium.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ../src ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
# bbport: NVIDIA Streamline SDK headers (MIT), for DLSS Frame Generation (vk_streamline.cpp).
target_include_directories(bbgpu PRIVATE third_party/streamline/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# The Linux shared library is loaded at process startup; Windows links it statically.
if(NOT WIN32)
    target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
    # A static FFmpeg (the Linux package's, packaging/linux/setup-ubuntu.sh): its assembly reaches
    # its tables PC-relative, which lld refuses for symbols the library exports. They stay inside.
    target_link_options(bbgpu PRIVATE
        -Wl,--exclude-libs,libavcodec.a:libavformat.a:libavutil.a:libswscale.a:libswresample.a)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan
    ffx-vulkan::fsr3-vk-framegeneration-3.1.6) # bbport: frame generation (vk_frame_generation.cpp)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::OPUS PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit
    xbyak::xbyak)
if (WIN32)
    # Windows headers: no min/max macros, no winsock 1 clash; Windows 10+ APIs (SetThreadDescription).
    target_compile_definitions(bbgpu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(bbgpu PUBLIC setupapi)
    if (BB_PGO STREQUAL "generate")
        # bbport: the instrumented static library needs the profile runtime in bb-probe.exe's
        # link (add_link_options does not reach it): written into bbgpu_link.txt below.
        target_link_options(bbgpu INTERFACE -fprofile-generate=${BB_PGO_DIR})
    endif()
    # bb-probe.exe links libbbgpu.a with these (out/gpu/bbgpu_link.txt, one argument per line,
    # in link order): the static library's transitive dependencies.
    include(cmake/LinkClosure.cmake)
    bb_write_link_closure(bbgpu ${CMAKE_CURRENT_BINARY_DIR}/bbgpu_link.txt)
else()
    # Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(pipeline-cache-blob-test EXCLUDE_FROM_ALL ../tests/test_pipeline_cache_blob.cpp)
target_compile_options(pipeline-cache-blob-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pipeline-cache-blob-test PRIVATE ..)
add_executable(image-overlap-test EXCLUDE_FROM_ALL ../tests/test_image_overlap.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(image-overlap-test PRIVATE -UNDEBUG)
target_include_directories(image-overlap-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(image-overlap-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(image-overlap-test PRIVATE winmm ws2_32 psapi onecore)
endif()
set_source_files_properties(../tests/test_image_overlap.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(pipeline-archive-test EXCLUDE_FROM_ALL ../tests/test_pipeline_archive.cpp)
target_compile_options(pipeline-archive-test PRIVATE -UNDEBUG)
target_include_directories(pipeline-archive-test PRIVATE shadps4)
target_link_libraries(pipeline-archive-test PRIVATE fmt::fmt)
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(lamp-fog-lights-test EXCLUDE_FROM_ALL ../tests/test_lamp_fog_lights.cpp)
target_compile_options(lamp-fog-lights-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(lamp-fog-lights-test PRIVATE ..)
foreach(effect_test
        "motion-blur-shader-test;test_motion_blur.cpp"
        "directional-shadow-test;test_directional_shadows.cpp"
        "rt-shadow-test;test_rt_shadows.cpp"
        "point-volumetric-test;test_point_volumetric.cpp"
        "effect-pipelines-test;test_effect_pipelines.cpp")
    list(GET effect_test 0 effect_test_name)
    list(GET effect_test 1 effect_test_source)
    add_executable(${effect_test_name} EXCLUDE_FROM_ALL ../tests/${effect_test_source})
    target_include_directories(${effect_test_name} PRIVATE ${SHADER_INCLUDE})
    target_compile_options(${effect_test_name} PRIVATE -UNDEBUG -Wall -Werror)
    target_link_libraries(${effect_test_name} PRIVATE Vulkan::Vulkan)
    add_dependencies(${effect_test_name} host_shaders)
endforeach()
add_executable(native-local-shadows-test EXCLUDE_FROM_ALL ../tests/test_native_local_shadows.cpp
    shim/engine/graphics.cpp shim/engine/engine_hooks.cpp shim/bbport_settings.cpp shim/bbport_menu_i18n.cpp)
target_include_directories(native-local-shadows-test PRIVATE shim)
target_compile_options(native-local-shadows-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(occlusion-test EXCLUDE_FROM_ALL ../tests/test_occlusion.cpp)
target_compile_options(occlusion-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(occlusion-test PRIVATE ..)
# Pipeline compiles off the frame path (docs/pipeline_compiles.md); the draw pipe's waits run
# with real threads (bbgpu: the platform and thread helpers).
add_executable(pipeline-compile-test EXCLUDE_FROM_ALL ../tests/test_pipeline_compile.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(pipeline-compile-test PRIVATE -UNDEBUG)
target_include_directories(pipeline-compile-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(pipeline-compile-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(pipeline-compile-test PRIVATE winmm ws2_32 psapi onecore)
endif()
# The shader lab (tests/shader_lab.cpp): shader captures compiled offline, without the game.
add_executable(bb-shader-lab EXCLUDE_FROM_ALL ../tests/shader_lab.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(bb-shader-lab PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(bb-shader-lab PRIVATE bbgpu miniz::miniz)
if (WIN32)
    target_link_libraries(bb-shader-lab PRIVATE winmm ws2_32 psapi onecore)
endif()
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_compile_options(motion-shader-test PRIVATE -UNDEBUG)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
if (WIN32)
    target_link_libraries(motion-shader-test PRIVATE winmm ws2_32 psapi onecore)
endif()
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp
    shim/bbport_menu_i18n.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

# bbport: indirect_clamp.comp as Rasterizer::ClampDispatchArgs records it, then an indirect
# dispatch from its output (run it under the validation layer); plain Vulkan, no bbgpu.
add_executable(indirect-clamp-test EXCLUDE_FROM_ALL ../tests/test_indirect_clamp.cpp)
target_include_directories(indirect-clamp-test PRIVATE ${SHADER_INCLUDE})
target_compile_options(indirect-clamp-test PRIVATE -UNDEBUG -Wall -Werror)
target_link_libraries(indirect-clamp-test PRIVATE Vulkan::Vulkan)
add_dependencies(indirect-clamp-test host_shaders)
# bbport: native HDR output's shaders on a synthetic frame (docs/hdr.md); plain Vulkan, no bbgpu.
add_executable(hdr-native-shader-test EXCLUDE_FROM_ALL ../tests/test_hdr_native.cpp)
target_include_directories(hdr-native-shader-test PRIVATE ${SHADER_INCLUDE})
target_compile_options(hdr-native-shader-test PRIVATE -UNDEBUG -Wall -Werror)
target_link_libraries(hdr-native-shader-test PRIVATE Vulkan::Vulkan)
add_dependencies(hdr-native-shader-test host_shaders)
# The composite tone curve extension on the GPU (docs/hdr-scene-path.md): the ramp modules of
# bb-shader-lab --ramp-modules, the game's curve against the extended one.
add_executable(tone-curve-shader-test EXCLUDE_FROM_ALL ../tests/test_tone_curve.cpp)
target_include_directories(tone-curve-shader-test PRIVATE ${SHADER_INCLUDE})
target_compile_options(tone-curve-shader-test PRIVATE -UNDEBUG -Wall -Werror)
target_link_libraries(tone-curve-shader-test PRIVATE Vulkan::Vulkan)
add_dependencies(tone-curve-shader-test host_shaders)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp shim/bbport_menu_i18n.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

add_executable(engine-hooks-test EXCLUDE_FROM_ALL ../tests/test_engine_hooks.cpp shim/engine/engine_hooks.cpp)
target_compile_options(engine-hooks-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(engine-hooks-test PRIVATE ..)
add_executable(frame-rate-test EXCLUDE_FROM_ALL ../tests/test_frame_rate.cpp shim/engine/engine_hooks.cpp)
target_compile_options(frame-rate-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(frame-rate-test PRIVATE .. shim shim/engine)
# Local game-derived native menu assets. No game data is embedded in this tool.
find_package(ZLIB REQUIRED)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/engine/pc_option_messages.tsv)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../tools/engine/pc_option_messages.tsv BB_ENGINE_MESSAGES)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/include/pc_option_messages.inc "R\"tsv(${BB_ENGINE_MESSAGES})tsv\"\n")
set(BB_MENU_ASSETS ../tools/engine/menu_assets.cpp ../tools/engine/esd.cpp ../tools/engine/rebirth_script.cpp
    ../tools/engine/sha256.cpp)
add_executable(bb-engine-assets ../tools/engine/menu_assets_main.cpp ${BB_MENU_ASSETS})
target_include_directories(bb-engine-assets PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(bb-engine-assets PRIVATE ZLIB::ZLIB)
if (WIN32)
    target_link_libraries(bb-engine-assets PRIVATE shell32)
endif()
add_executable(bb-ui-4k ../tools/engine/ui_4k.cpp shim/hud_gauge.cpp)
target_include_directories(bb-ui-4k PRIVATE shim)
target_link_libraries(bb-ui-4k PRIVATE ZLIB::ZLIB PkgConfig::XXHASH)
if (WIN32)
    target_link_libraries(bb-ui-4k PRIVATE shell32)
endif()
add_executable(menu-assets-test EXCLUDE_FROM_ALL ../tests/test_menu_assets.cpp ${BB_MENU_ASSETS})
target_include_directories(menu-assets-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(menu-assets-test PRIVATE -UNDEBUG -Wall -Wextra)
target_link_libraries(menu-assets-test PRIVATE ZLIB::ZLIB)
add_executable(option-rows-test EXCLUDE_FROM_ALL ../tests/test_option_rows.cpp shim/engine/option_rows.cpp)
target_compile_options(option-rows-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(option-rows-test PRIVATE ..)
add_executable(engine-menu-memory-test EXCLUDE_FROM_ALL ../tests/test_engine_menu_memory.cpp shim/engine/menu_memory.cpp)
target_compile_options(engine-menu-memory-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(engine-menu-memory-test PRIVATE ..)
add_executable(shared-progress-test EXCLUDE_FROM_ALL ../tests/test_shared_progress.cpp)
target_compile_options(shared-progress-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(shared-progress-test PRIVATE ..)
add_executable(party-death-test EXCLUDE_FROM_ALL ../tests/test_party_death.cpp)
target_compile_options(party-death-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(party-death-test PRIVATE ..)
add_executable(anti-lag-pacer-test EXCLUDE_FROM_ALL ../tests/test_anti_lag_pacer.cpp)
target_compile_options(anti-lag-pacer-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(anti-lag-pacer-test PRIVATE .)
add_executable(world-copy-test EXCLUDE_FROM_ALL ../tests/test_world_copy.cpp)
target_compile_options(world-copy-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(world-copy-test PRIVATE ..)
add_executable(kept-travel-test EXCLUDE_FROM_ALL ../tests/test_kept_travel.cpp)
target_compile_options(kept-travel-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(kept-travel-test PRIVATE ..)
add_executable(rebirth-cost-test EXCLUDE_FROM_ALL ../tests/test_rebirth_cost.cpp)
target_compile_options(rebirth-cost-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(rebirth-cost-test PRIVATE ..)
add_executable(pc-input-test EXCLUDE_FROM_ALL ../tests/test_pc_input.cpp shim/input/bindings.cpp)
target_include_directories(pc-input-test PRIVATE shim)
target_link_libraries(pc-input-test PRIVATE PkgConfig::SDL3)
target_compile_options(pc-input-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(widescreen-test EXCLUDE_FROM_ALL ../tests/test_widescreen.cpp
    shim/engine/widescreen.cpp shim/engine/camera.cpp shim/engine/engine_hooks.cpp shim/bbport_settings.cpp
    shim/bbport_menu_i18n.cpp)
target_include_directories(widescreen-test PRIVATE shim)
target_compile_options(widescreen-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
