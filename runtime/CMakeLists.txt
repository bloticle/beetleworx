cmake_minimum_required(VERSION 3.25)
project(EM2RecompRuntime LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(EM2_OPTIMIZE "Enable optimization of translated game code" ON)
option(EM2_THIN_LTO "Experiment with link-time optimization of translated code and native overrides" OFF)
set(EM2_THIN_LTO_JOBS "2" CACHE STRING "Maximum concurrent ThinLTO backend jobs")
if(EM2_OPTIMIZE)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "/Od /Ob0 /DNDEBUG")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(EM2_GAME_DIRECTORY "D:/Modding/Builds/EM2X360" CACHE PATH "Extracted Xbox 360 game directory")
set(REXSDK "D:/Repos/rexglue-sdk" CACHE PATH "ReXGlue 0.1 SDK")
list(PREPEND CMAKE_PREFIX_PATH "${REXSDK}")
find_package(rexglue CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(EM2_LLVM_AR llvm-ar REQUIRED)
find_program(EM2_LLVM_OBJCOPY llvm-objcopy REQUIRED)
find_program(EM2_LLVM_NM llvm-nm REQUIRED)
set(EM2_RENDERER_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/renderer/rexgraphics.lib")
set(EM2_UI_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/renderer/rexui.lib")
add_custom_command(OUTPUT "${EM2_RENDERER_LIBRARY}" "${EM2_UI_LIBRARY}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-renderer-library.py"
        "${REXSDK}/lib/rexgraphics.lib" "${EM2_RENDERER_LIBRARY}"
        --ui-source "${REXSDK}/lib/rexui.lib" --ui-output "${EM2_UI_LIBRARY}"
        --ar "${EM2_LLVM_AR}" --objcopy "${EM2_LLVM_OBJCOPY}" --nm "${EM2_LLVM_NM}"
    DEPENDS "${REXSDK}/lib/rexgraphics.lib" "${REXSDK}/lib/rexui.lib" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-renderer-library.py" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/coff_bridge.py"
    VERBATIM)
add_custom_target(em2_renderer_library DEPENDS "${EM2_RENDERER_LIBRARY}" "${EM2_UI_LIBRARY}")
set_property(TARGET rex::graphics PROPERTY IMPORTED_LOCATION_RELEASE "${EM2_RENDERER_LIBRARY}")
set_property(TARGET rex::ui PROPERTY IMPORTED_LOCATION_RELEASE "${EM2_UI_LIBRARY}")
set(EM2_CORE_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/core/rexcore.lib")
add_custom_command(OUTPUT "${EM2_CORE_LIBRARY}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-core-library.py"
        "${REXSDK}/lib/rexcore.lib" "${EM2_CORE_LIBRARY}"
        --ar "${EM2_LLVM_AR}" --objcopy "${EM2_LLVM_OBJCOPY}" --nm "${EM2_LLVM_NM}"
    DEPENDS "${REXSDK}/lib/rexcore.lib" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-core-library.py"
    VERBATIM)
add_custom_target(em2_core_library DEPENDS "${EM2_CORE_LIBRARY}")
set_property(TARGET rex::core PROPERTY IMPORTED_LOCATION_RELEASE "${EM2_CORE_LIBRARY}")
set(EM2_KERNEL_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/core/rexkernel.lib")
add_custom_command(OUTPUT "${EM2_KERNEL_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/core"
    COMMAND "${CMAKE_COMMAND}" -E copy "${REXSDK}/lib/rexkernel.lib" "${EM2_KERNEL_LIBRARY}"
    COMMAND "${EM2_LLVM_AR}" d "${EM2_KERNEL_LIBRARY}" content_manager.cpp.obj
    DEPENDS "${REXSDK}/lib/rexkernel.lib"
    VERBATIM)
add_custom_target(em2_kernel_library DEPENDS "${EM2_KERNEL_LIBRARY}")
set_property(TARGET rex::kernel PROPERTY IMPORTED_LOCATION_RELEASE "${EM2_KERNEL_LIBRARY}")
set(EM2_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/../generated/ppc")
file(GLOB EM2_PPC_SOURCES CONFIGURE_DEPENDS "${EM2_GENERATED}/*.cpp")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 23 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 24)
    # LLVM 23.1.4 crashes in "Merge disjoint stack slots" for the generated
    # __imp__sub_827B3298 when rebuilding the shared throwing-call context.
    # Preserve O2 and guest semantics; disable only stack-slot coloring in its TU.
    set_property(SOURCE "${EM2_GENERATED}/ppc_recomp.147.cpp" APPEND PROPERTY COMPILE_OPTIONS /clang:-mllvm /clang:-no-stack-coloring)
endif()
if(NOT EM2_PPC_SOURCES)
    message(FATAL_ERROR "Run scripts/recompile.ps1 before configuring the runtime")
endif()
option(EM2_STARTUP_DIAGNOSTICS "Enable expensive guest allocation and startup probes" OFF)
add_executable(EM2Recomp WIN32 main.cpp imports.cpp crash_handler.cpp entrypoints.cpp frame_metrics.cpp pc_settings.cpp simulation.cpp skinning.cpp controls.cpp render_options.cpp render_logging.cpp players.cpp em2_windowed_app_main.cpp ${EM2_PPC_SOURCES})
set_target_properties(EM2Recomp PROPERTIES OUTPUT_NAME beetleworx PDB_NAME beetleworx COMPILE_PDB_NAME EM2Recomp)
set_property(SOURCE em2_windowed_app_main.cpp PROPERTY INCLUDE_DIRECTORIES "${REXGLUE_SHARE_DIR}")
add_dependencies(EM2Recomp em2_renderer_library)
target_sources(EM2Recomp PRIVATE game_console.cpp)
target_sources(EM2Recomp PRIVATE content_patch.cpp)
target_sources(EM2Recomp PRIVATE shadow_distance.cpp)
target_sources(EM2Recomp PRIVATE shadow_cast_probe.cpp)
target_sources(EM2Recomp PRIVATE shadow_readback.cpp)
add_dependencies(EM2Recomp em2_core_library)
file(GLOB EM2_NATIVE_SHADER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hlsl")
file(GLOB_RECURSE EM2_NATIVE_SHADER_INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/rex_render_target/thirdparty/direct_resolve/*.xesli")
set(EM2_BUILT_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/built_shaders_data.h")
add_custom_command(OUTPUT "${EM2_BUILT_SHADERS}" "${CMAKE_CURRENT_BINARY_DIR}/built_shaders_data.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-native-shaders.py"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${EM2_BUILT_SHADERS}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-native-shaders.py" ${EM2_NATIVE_SHADER_SOURCES} ${EM2_NATIVE_SHADER_INCLUDES}
    VERBATIM)
add_custom_target(em2_native_shaders DEPENDS "${EM2_BUILT_SHADERS}")
add_dependencies(EM2Recomp em2_native_shaders)
target_sources(EM2Recomp PRIVATE built_shaders.cpp built_shaders_test.cpp)
target_sources(EM2Recomp PRIVATE direct_color_resolve.cpp direct_color_resolve_test.cpp)
target_sources(EM2Recomp PRIVATE engine_workbench.cpp console_tools.cpp gameplay_mods.cpp native_engine.cpp gpu_interrupt_lifetime.cpp audio_lifetime.cpp)
target_sources(EM2Recomp PRIVATE platform_allocations.cpp movie_texture_memory.cpp)
set(EM2_NATIVE_VIDEO_BLOCKS "${CMAKE_CURRENT_BINARY_DIR}/native_video_blocks_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_VIDEO_BLOCKS}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-video-blocks.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_VIDEO_BLOCKS}"
    DEPENDS "${EM2_GENERATED}/ppc_recomp.544.cpp" "${EM2_GENERATED}/ppc_recomp.545.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-video-blocks.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE video_blocks.cpp "${EM2_NATIVE_VIDEO_BLOCKS}")
set_property(SOURCE "${EM2_NATIVE_VIDEO_BLOCKS}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(SOURCE audio_lifetime.cpp PROPERTY INCLUDE_DIRECTORIES "${REXSDK}/include/SDL2")
set_property(SOURCE built_shaders.cpp built_shaders_test.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
set(EM2_RENDER_TARGET_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/render-target")
add_custom_command(OUTPUT "${EM2_RENDER_TARGET_GENERATED}/ready.stamp"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-render-target-source.py"
        "${REXSDK}" "${EM2_RENDER_TARGET_GENERATED}" --ar "${EM2_LLVM_AR}"
    DEPENDS "${REXSDK}/lib/rexgraphics.lib" "${REXSDK}/include/rex/graphics/d3d12/render_target_cache.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-render-target-source.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/rex_render_target/render_target_cache.cpp"
    VERBATIM)
add_custom_target(em2_render_target_shaders DEPENDS "${EM2_RENDER_TARGET_GENERATED}/ready.stamp")
add_dependencies(EM2Recomp em2_render_target_shaders)
target_sources(EM2Recomp PRIVATE rex_render_target/render_target_cache.cpp)
set_property(SOURCE rex_render_target/render_target_cache.cpp PROPERTY INCLUDE_DIRECTORIES "${EM2_RENDER_TARGET_GENERATED}")
set_property(SOURCE rex_render_target/render_target_cache.cpp PROPERTY SKIP_PRECOMPILE_HEADERS ON)
set(EM2_PRESENTER_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/presenter")
set(EM2_PRESENTER_SHADER_HEADERS)
foreach(shader IN ITEMS guest_output_triangle_strip_rect_vs guest_output_bilinear_ps
    guest_output_bilinear_dither_ps guest_output_ffx_cas_resample_ps
    guest_output_ffx_cas_resample_dither_ps guest_output_ffx_cas_sharpen_ps
    guest_output_ffx_cas_sharpen_dither_ps guest_output_ffx_cas_sharpen_hdr_ps
    guest_output_ffx_cas_resample_hdr_ps guest_output_hdr_cas_bilinear_ps
    guest_output_ffx_fsr_easu_hdr_ps guest_output_ffx_fsr_rcas_hdr_ps guest_output_ffx_fsr_easu_ps
    guest_output_ffx_fsr_rcas_ps guest_output_ffx_fsr_rcas_dither_ps)
    list(APPEND EM2_PRESENTER_SHADER_HEADERS "${EM2_PRESENTER_GENERATED}/ui/shaders/bytecode/d3d12_5_1/${shader}.h")
endforeach()
add_custom_command(OUTPUT "${EM2_PRESENTER_GENERATED}/ready.stamp"
    BYPRODUCTS ${EM2_PRESENTER_SHADER_HEADERS} "${EM2_PRESENTER_GENERATED}/shader-manifest.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-presenter-source.py"
        "${REXSDK}" "${EM2_PRESENTER_GENERATED}" --ar "${EM2_LLVM_AR}"
    DEPENDS "${REXSDK}/lib/rexui.lib" "${REXSDK}/include/rex/ui/d3d12/d3d12_presenter.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-presenter-source.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/cas_shader_compiler.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/clamped_cas.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/hdr_cas_colour.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/hdr_cas_bilinear.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/hdr_fsr.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/ffx_fsr1.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/fsr_ffx_a.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/ffx_a.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/ffx_cas.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/noise.xesli"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cas/upstream.json"
    VERBATIM)
add_custom_target(em2_presenter_shaders DEPENDS "${EM2_PRESENTER_GENERATED}/ready.stamp")
add_dependencies(EM2Recomp em2_presenter_shaders)
target_sources(EM2Recomp PRIVATE rex_presenter/d3d12_presenter.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hdr_output.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/hdr_output.hlsl" EM2_HDR_SHADER_TEXT)
configure_file(hdr_output_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/hdr_output_shader.h" @ONLY)
target_sources(EM2Recomp PRIVATE hdr_output.cpp hdr_output_test.cpp)
target_sources(EM2Recomp PRIVATE hdr_composition_test.cpp)
target_sources(EM2Recomp PRIVATE hdr_presenter.cpp hdr_presenter_test.cpp)
target_sources(EM2Recomp PRIVATE hdr_swap.cpp)
target_sources(EM2Recomp PRIVATE hdr_native_swap_test.cpp)
target_sources(EM2Recomp PRIVATE renderer_colour_mode.cpp)
target_sources(EM2Recomp PRIVATE presenter_ui_exchange.cpp presenter_ui_exchange_test.cpp)
  target_sources(EM2Recomp PRIVATE hdr_settings.cpp scene_lighting_settings.cpp)
target_sources(EM2Recomp PRIVATE hdr_shader_export.cpp hdr_shader_export_test.cpp)
target_sources(EM2Recomp PRIVATE hdr_scene_target.cpp)
target_sources(EM2Recomp PRIVATE hdr_scene_cache.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hdr_scene_resolve.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/hdr_scene_resolve.hlsl" EM2_HDR_RESOLVE_SHADER_TEXT)
configure_file(hdr_scene_resolve_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/hdr_scene_resolve_shader.h" @ONLY)
target_sources(EM2Recomp PRIVATE hdr_scene_resolve.cpp)
target_sources(EM2Recomp PRIVATE hdr_texture_binding.cpp)
set_property(SOURCE hdr_scene_resolve.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
set_property(SOURCE hdr_output.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
set_property(SOURCE rex_presenter/d3d12_presenter.cpp PROPERTY INCLUDE_DIRECTORIES "${EM2_PRESENTER_GENERATED}")
set_property(SOURCE rex_presenter/d3d12_presenter.cpp PROPERTY SKIP_PRECOMPILE_HEADERS ON)
target_sources(EM2Recomp PRIVATE pack_reads.cpp pack_read_copy.cpp)
# A mapped page or destination can fault asynchronously inside memmove. Keep
# this tiny SEH boundary separate from the ordinary C++ exception model/PCH.
set_property(SOURCE pack_read_copy.cpp PROPERTY SKIP_PRECOMPILE_HEADERS ON)
set_property(SOURCE pack_read_copy.cpp APPEND PROPERTY COMPILE_OPTIONS /EHa /clang:-fno-lto)
target_sources(EM2Recomp PRIVATE online_frontend.cpp)
target_sources(EM2Recomp PRIVATE online_router.cpp online_router_test.cpp)
target_sources(EM2Recomp PRIVATE online_upnp.cpp online_upnp_test.cpp)
target_sources(EM2Recomp PRIVATE online_discovery.cpp)
target_sources(EM2Recomp PRIVATE shadow_kernel.cpp)
target_sources(EM2Recomp PRIVATE shadow_pixel_test.cpp)
target_sources(EM2Recomp PRIVATE scene_shader_audit.cpp)
target_sources(EM2Recomp PRIVATE material_shader_audit.cpp)
target_sources(EM2Recomp PRIVATE material_lighting_test.cpp)
target_sources(EM2Recomp PRIVATE em1_blend_pixel_test.cpp)
target_sources(EM2Recomp PRIVATE em1_native_effect_trace.cpp)
target_sources(EM2Recomp PRIVATE material_full_lighting_test.cpp material_ambient_clear_test.cpp material_ambient_export.cpp material_capture.cpp material_capture_test.cpp)
target_sources(EM2Recomp PRIVATE scene_vertex_test.cpp)
target_sources(EM2Recomp PRIVATE scene_replay.cpp)
target_sources(EM2Recomp PRIVATE scene_raster_geometry.cpp)
target_sources(EM2Recomp PRIVATE scene_raster_pass.cpp)
target_sources(EM2Recomp PRIVATE scene_replay_gpu_test.cpp)
target_sources(EM2Recomp PRIVATE scene_lighting.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scene_raster_geometry.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene_raster_geometry.hlsl" EM2_SCENE_RASTER_GEOMETRY_TEXT)
configure_file(scene_raster_geometry_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/scene_raster_geometry_shader.h" @ONLY)
set_property(SOURCE scene_raster_geometry.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
target_sources(EM2Recomp PRIVATE scene_draw_inputs.cpp)
target_sources(EM2Recomp PRIVATE scene_raster.cpp)
target_sources(EM2Recomp PRIVATE scene_geometry.cpp)
target_sources(EM2Recomp PRIVATE scene_occlusion.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scene_occlusion.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene_occlusion.hlsl" EM2_SCENE_OCCLUSION_TEXT)
configure_file(scene_occlusion_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/scene_occlusion_shader.h" @ONLY)
set_property(SOURCE scene_occlusion.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scene_geometry.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene_geometry.hlsl" EM2_SCENE_GEOMETRY_TEXT)
configure_file(scene_geometry_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/scene_geometry_shader.h" @ONLY)
set_property(SOURCE scene_geometry.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scene_unproject.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene_unproject.hlsl" EM2_SCENE_UNPROJECT_TEXT)
configure_file(scene_unproject_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/scene_unproject_shader.h" @ONLY)
target_sources(EM2Recomp PRIVATE scene_unproject_test.cpp)
set_property(SOURCE scene_unproject_test.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
target_sources(EM2Recomp PRIVATE bloom_pixel_test.cpp)
target_sources(EM2Recomp PRIVATE bloom_kernel.cpp)
target_sources(EM2Recomp PRIVATE bloom_composite_test.cpp)
target_sources(EM2Recomp PRIVATE online_continue.cpp)
target_sources(EM2Recomp PRIVATE online_notice.cpp)
target_sources(EM2Recomp PRIVATE content_setup.cpp)
target_sources(EM2Recomp PRIVATE disc_image.cpp)
target_sources(EM2Recomp PRIVATE online_lobby.cpp)
target_sources(EM2Recomp PRIVATE online_content_root.cpp rex_content/content_manager.cpp)
add_dependencies(EM2Recomp em2_kernel_library)
target_sources(EM2Recomp PRIVATE native_random.cpp)
target_sources(EM2Recomp PRIVATE physics_timing.cpp)
target_sources(EM2Recomp PRIVATE character_timing.cpp)
target_sources(EM2Recomp PRIVATE camera_projection.cpp camera_targets.cpp camera_cadence.cpp camera_look.cpp)
target_sources(EM2Recomp PRIVATE register_bank.cpp)

set(EM2_NATIVE_TYPE_CHECK_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_type_check_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_TYPE_CHECK_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-type-check.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_TYPE_CHECK_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-type-check.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE type_check.cpp "${EM2_NATIVE_TYPE_CHECK_SOURCE}")

set(EM2_NATIVE_MATRIX_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_matrix_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_MATRIX_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-matrix.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_MATRIX_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-matrix.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE matrix_math.cpp "${EM2_NATIVE_MATRIX_SOURCE}")
set_property(SOURCE "${EM2_NATIVE_MATRIX_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")

set(EM2_NATIVE_MATERIAL_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_material_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_MATERIAL_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-material.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_MATERIAL_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-material.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE material_binding.cpp "${EM2_NATIVE_MATERIAL_SOURCE}")
target_sources(EM2Recomp PRIVATE mickey_ears.cpp)
target_sources(EM2Recomp PRIVATE native_player_spawn.cpp)
target_sources(EM2Recomp PRIVATE demo_setup.cpp)
set_property(SOURCE "${EM2_NATIVE_MATERIAL_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
  target_sources(EM2Recomp PRIVATE shader_storage.cpp shader_storage_test.cpp pc_shader_library.cpp pc_shader_library_test.cpp pc_shader_native_cache_test.cpp)
target_sources(EM2Recomp PRIVATE pso_binary_cache.cpp gpu_constants.cpp gpu_constants_test.cpp gpu_command_profile.cpp gpu_execution_profile.cpp gpu_execution_profile_test.cpp gpu_draw_profile.cpp)
target_sources(EM2Recomp PRIVATE transfer_scissor_test.cpp transfer_scissor_gpu_test.cpp stencil_zero_proof_test.cpp native_stencil_zero_test.cpp bounded_transfer_division_test.cpp)
target_sources(EM2Recomp PRIVATE transfer_benchmark.cpp stencil_transfer.cpp fenced_scratch_cache_test.cpp)
target_sources(EM2Recomp PRIVATE stencil_plane_copy_probe.cpp stencil_plane_copy_pixels.cpp)
target_sources(EM2Recomp PRIVATE lto_dispatch_fixture.cpp lto_dispatch_test.cpp)
file(SHA256 "${REXSDK}/lib/rexgraphics.lib" EM2_RENDERER_CACHE_SHA)
string(SUBSTRING "${EM2_RENDERER_CACHE_SHA}" 0 16 EM2_RENDERER_CACHE_KEY)
set_property(SOURCE shader_storage.cpp APPEND PROPERTY COMPILE_DEFINITIONS "EM2_RENDERER_CACHE_ABI=\"rex-${EM2_RENDERER_CACHE_KEY}\"")
set_property(SOURCE shader_storage.cpp PROPERTY SKIP_PRECOMPILE_HEADERS ON)
set(EM2_NATIVE_SHADER_UPLOAD_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_shader_upload_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_SHADER_UPLOAD_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-shader-upload.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_SHADER_UPLOAD_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-shader-upload.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE shader_upload.cpp "${EM2_NATIVE_SHADER_UPLOAD_SOURCE}")
set_property(SOURCE "${EM2_NATIVE_SHADER_UPLOAD_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
set(EM2_NATIVE_VISIBILITY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_visibility_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_VISIBILITY_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-visibility.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_VISIBILITY_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-visibility.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE visibility_scan.cpp "${EM2_NATIVE_VISIBILITY_SOURCE}")
set_property(SOURCE "${EM2_NATIVE_VISIBILITY_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
set(EM2_NATIVE_MEMORY_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_memory_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_MEMORY_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-memory.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_MEMORY_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-memory.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE native_sampler.cpp binding_cache.cpp property_lookup.cpp memory_copy.cpp allocation_trace.cpp heap_budget.cpp camera_timing.cpp online.cpp online_gameplay.cpp online_identity.cpp online_invite.cpp online_checkpoint.cpp online_boot.cpp online_campaigns.cpp online_runtime.cpp online_qa.cpp world_observation.cpp "${EM2_NATIVE_MEMORY_SOURCE}")
set_property(SOURCE "${EM2_NATIVE_MEMORY_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
if(EM2_STARTUP_DIAGNOSTICS)
    target_sources(EM2Recomp PRIVATE startup_diagnostics.cpp)
endif()
target_include_directories(EM2Recomp PRIVATE "${EM2_GENERATED}")
target_compile_definitions(EM2Recomp PRIVATE EM2_DEFAULT_GAME_DIRECTORY="${EM2_GAME_DIRECTORY}")
target_precompile_headers(EM2Recomp PRIVATE "${EM2_GENERATED}/ppc_context.h")
target_compile_options(EM2Recomp PRIVATE /bigobj /clang:-mavx2 /clang:-Wno-unused-value /clang:-Wno-invalid-source-encoding)

if(EM2_THIN_LTO AND EM2_OPTIMIZE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT MSVC)
        message(FATAL_ERROR "EM2 ThinLTO requires the configured clang-cl/lld-link toolchain")
    endif()
    get_filename_component(EM2_LINKER_NAME "${CMAKE_LINKER}" NAME_WE)
    if(NOT EM2_LINKER_NAME STREQUAL "lld-link")
        message(FATAL_ERROR "EM2 ThinLTO requires lld-link; use scripts/enter-toolchain.ps1")
    endif()
    if(NOT EM2_THIN_LTO_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "EM2_THIN_LTO_JOBS must be a positive integer")
    endif()
    # Keep weak symbol interposition and existing floating-point rules intact.
    # Native SDK archives remain machine code; only this target emits bitcode.
    target_compile_options(EM2Recomp PRIVATE "$<$<CONFIG:Release>:-flto=thin>")
    target_compile_definitions(EM2Recomp PRIVATE "$<$<CONFIG:Release>:EM2_THIN_LTO_DISPATCH=1>")
    target_precompile_headers(EM2Recomp PRIVATE "$<$<CONFIG:Release>:${CMAKE_CURRENT_SOURCE_DIR}/thinlto_dispatch.h>")
    # Matrix NaN payload selection was reviewed against ordinary per-source
    # code generation. Keep both that reference and its ordered-assembly
    # replacement outside LTO; do not weaken their exact-bit differential.
    set_property(SOURCE "${EM2_NATIVE_MATRIX_SOURCE}"
        APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Release>:-fno-lto>")
    # Repeated whole-build comparisons found regressions in already tuned
    # upload, skinning and material kernels. Retain their per-source codegen
    # while optimizing the remaining translated/native call graph with LTO.
    set_property(SOURCE shader_upload.cpp skinning.cpp "${EM2_NATIVE_MATERIAL_SOURCE}" "${EM2_NATIVE_MEMORY_SOURCE}"
        APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Release>:-fno-lto>")
    # Keep the corresponding weak fallbacks in the same COFF linkage mode as
    # their native overrides; mixed weak-IR/strong-COFF definitions otherwise
    # produce a duplicate-symbol error in the installed LLVM linker.
    # Resolve ownership by function, not bundle number: recompilation may move
    # a function to another generated source file.
    set(EM2_SOURCE_FINDER "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/find-translated-sources.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EM2_SOURCE_FINDER}" ${EM2_PPC_SOURCES})
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${EM2_SOURCE_FINDER}" "${EM2_GENERATED}"
        __imp__sub_82B9D350 __imp__sub_82B6A550 __imp__sub_826BF668 __imp__sub_826BF2D0 __imp__sub_826BF510 __imp__sub_82551560
        RESULT_VARIABLE EM2_SOURCE_FINDER_RESULT OUTPUT_VARIABLE EM2_ORDERED_REFERENCE_SOURCES
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT EM2_SOURCE_FINDER_RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to identify numerical/override sources for the ThinLTO boundary")
    endif()
    set_property(SOURCE ${EM2_ORDERED_REFERENCE_SOURCES}
        APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Release>:-fno-lto>")
    target_link_options(EM2Recomp PRIVATE
        "$<$<CONFIG:Release>:/opt:lldltojobs=${EM2_THIN_LTO_JOBS}>"
        "$<$<CONFIG:Release>:/lldltocache:${CMAKE_CURRENT_BINARY_DIR}/thinlto-cache>"
        "$<$<CONFIG:Release>:/lldltocachepolicy:cache_size_bytes=2147483648>")
endif()
target_link_libraries(EM2Recomp PRIVATE rex::core rex::runtime rex::kernel rex::graphics rex::ui)
# Match the MSVC-style C++ driver when enabling C for the decoder. Selecting
# clang.exe here would feed GNU-style Clang the runtime's existing /D flags.
if(MSVC AND NOT CMAKE_C_COMPILER_LOADED)
    set(CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH "C compiler for native decoders" FORCE)
endif()
add_subdirectory(em1_compat)
add_subdirectory(em1_lua_core)
target_sources(EM2Recomp PRIVATE em1_script_source.cpp em1_script_audio.cpp em1_native_globals.cpp em1_target_image.cpp)
target_link_libraries(EM2Recomp PRIVATE "$<LINK_ONLY:em1_lua_native>")
target_sources(EM2Recomp PRIVATE em1_level_collision.cpp em1_level_entry.cpp em1_native_item_manager.cpp em1_native_component.cpp em1_native_properties.cpp em1_runtime_validation.cpp em1_parser_probe.cpp em1_runtime_validation_hook.cpp em1_model_validation.cpp em1_native_resources.cpp em1_native_animations.cpp em1_havok_collision_probe.cpp em1_havok_collision_world.cpp em1_native_preview.cpp em1_preview_runtime.cpp em1_single_gsa.cpp em1_single_gsa_pak.cpp em1_player_bootstrap.cpp em1_loader_diagnostics.cpp em1_preview_capture.cpp)
target_sources(EM2Recomp PRIVATE em1_native_navigation.cpp)
# The game's sources already include sibling runtime headers directly. Keep
# adapter include paths off the translated code's compile command so this new
# static library doesn't force a full recompilation of every PPC function.
target_link_libraries(EM2Recomp PRIVATE "$<LINK_ONLY:em1_compat>")
target_sources(EM2Recomp PRIVATE mouse_look.cpp)
target_sources(EM2Recomp PRIVATE restored_gremlin_names.cpp)
target_sources(EM2Recomp PRIVATE em1_ink_visibility.cpp)
target_sources(EM2Recomp PRIVATE ink_shader_test.cpp)
target_sources(EM2Recomp PRIVATE emr_ability_animation_probe.cpp)
target_sources(EM2Recomp PRIVATE demo_support.cpp game_build.cpp)
target_sources(EM2Recomp PRIVATE live_graphics.cpp)
target_sources(EM2Recomp PRIVATE physical_arena.cpp)
target_sources(EM2Recomp PRIVATE gpu_sync.cpp)
target_sources(EM2Recomp PRIVATE scaled_resolve_migration.cpp)
target_sources(EM2Recomp PRIVATE edram_scale_migration_test.cpp)
target_sources(EM2Recomp PRIVATE edram_capture.cpp)
target_sources(EM2Recomp PRIVATE auxiliary_depth_resample.cpp)
target_sources(EM2Recomp PRIVATE resolve_write_observer.cpp)
target_sources(EM2Recomp PRIVATE resolve_coverage.cpp)
target_sources(EM2Recomp PRIVATE resolved_storage_capture.cpp)
target_sources(EM2Recomp PRIVATE resolved_storage_conversion.cpp)
target_sources(EM2Recomp PRIVATE shadow_resources.cpp)
target_sources(EM2Recomp PRIVATE shadow_texture.cpp)
target_sources(EM2Recomp PRIVATE native_texture_retirement.cpp)
set(EM2_NATIVE_SHADOW_TEXTURE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_shadow_texture_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_SHADOW_TEXTURE_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-shadow-texture.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_SHADOW_TEXTURE_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-shadow-texture.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE "${EM2_NATIVE_SHADOW_TEXTURE_SOURCE}")
set_property(SOURCE "${EM2_NATIVE_SHADOW_TEXTURE_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(EM2Recomp PRIVATE shadow_resize.cpp)
set(EM2_NATIVE_SHADOW_REFRESH_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_shadow_refresh_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_SHADOW_REFRESH_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-shadow-refresh.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_SHADOW_REFRESH_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-shadow-refresh.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE "${EM2_NATIVE_SHADOW_REFRESH_SOURCE}")
set_property(SOURCE "${EM2_NATIVE_SHADOW_REFRESH_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
set(EM2_NATIVE_MOUSE_PITCH_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/native_mouse_pitch_generated.cpp")
add_custom_command(OUTPUT "${EM2_NATIVE_MOUSE_PITCH_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-mouse-pitch.py"
        "${EM2_GENERATED}" "${EM2_NATIVE_MOUSE_PITCH_SOURCE}"
    DEPENDS ${EM2_PPC_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare-native-mouse-pitch.py"
    VERBATIM)
target_sources(EM2Recomp PRIVATE "${EM2_NATIVE_MOUSE_PITCH_SOURCE}")
set_property(SOURCE "${EM2_NATIVE_MOUSE_PITCH_SOURCE}" PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EM2Recomp PRIVATE winmm ws2_32 bcrypt iphlpapi shell32 ole32 oleaut32 comctl32 uuid winhttp)
target_link_options(EM2Recomp PRIVATE /MANIFEST:EMBED
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'")
target_link_options(EM2Recomp PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:rex::kernel>")
target_link_options(EM2Recomp PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF "/MAP:${CMAKE_CURRENT_BINARY_DIR}/beetleworx.map")
if(EXISTS "${EM2_GENERATED}/../tests/instructions.cpp")
    enable_testing()
    target_sources(EM2Recomp PRIVATE lowering_tests.cpp "${EM2_GENERATED}/../tests/instructions.cpp")
    add_test(NAME runtime-regressions COMMAND EM2Recomp --em2_test_pc_settings=true
        "--log_file=${CMAKE_CURRENT_BINARY_DIR}/runtime-tests.log")
    add_test(NAME ground-pound-retail-stimuli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/audit-ground-pound-stimuli.py")
    add_test(NAME demo-support-transactions COMMAND EM2Recomp --em2_test_demo_support=true
        "--log_file=${CMAKE_CURRENT_BINARY_DIR}/demo-support-tests.log")
endif()
target_sources(EM2Recomp PRIVATE renderer_storage.cpp)

target_sources(EM2Recomp PRIVATE renderer_bindings.cpp)

target_sources(EM2Recomp PRIVATE renderer_graph.cpp)

target_sources(EM2Recomp PRIVATE renderer_graph_test.cpp)

target_sources(EM2Recomp PRIVATE verify_packed.cpp)


target_sources(EM2Recomp PRIVATE scene_ambient.cpp scene_ambient_test.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scene_ambient.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene_ambient.hlsl" EM2_SCENE_AMBIENT_TEXT)
configure_file(scene_ambient_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/scene_ambient_shader.h" @ONLY)
set_property(SOURCE scene_ambient.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")

target_sources(EM2Recomp PRIVATE hdr_encoding_test.cpp)

target_sources(EM2Recomp PRIVATE scene_ambient_publish_test.cpp)

target_sources(EM2Recomp PRIVATE hdr_guest_view.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hdr_guest_view.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/hdr_guest_view.hlsl" EM2_HDR_GUEST_SHADER_TEXT)
configure_file(hdr_guest_view_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/hdr_guest_view_shader.h" @ONLY)
set_property(SOURCE hdr_guest_view.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")

target_sources(EM2Recomp PRIVATE hdr_guest_view_test.cpp)

target_sources(EM2Recomp PRIVATE world_material_test.cpp)

target_sources(EM2Recomp PRIVATE material_surface_export.cpp world_surface_test.cpp)

target_sources(EM2Recomp PRIVATE scene_indirect.cpp scene_indirect_test.cpp)
target_sources(EM2Recomp PRIVATE scene_indirect_benchmark.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scene_indirect.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scene_indirect.hlsl" EM2_SCENE_INDIRECT_TEXT)
configure_file(scene_indirect_shader.h.in "${CMAKE_CURRENT_BINARY_DIR}/scene_indirect_shader.h" @ONLY)
set_property(SOURCE scene_indirect.cpp PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")

target_sources(EM2Recomp PRIVATE online_perception.cpp online_selector.cpp online_uuid.cpp online_world_random.cpp)
target_sources(EM2Recomp PRIVATE online_native_menu.cpp)
target_sources(EM2Recomp PRIVATE online_native_handoff.cpp)
target_sources(EM2Recomp PRIVATE stencil_consumer_survey.cpp)

target_sources(EM2Recomp PRIVATE screen_upscale_test.cpp)

target_sources(EM2Recomp PRIVATE guest_read_batch_test.cpp)

target_sources(EM2Recomp PRIVATE native_entity_probe.cpp)

target_sources(EM2Recomp PRIVATE em1_audio_output.cpp)
target_sources(EM2Recomp PRIVATE em1_asset_preflight.cpp)
target_sources(EM2Recomp PRIVATE em1_ai_compat.cpp)
target_sources(EM2Recomp PRIVATE em1_script_events.cpp)
target_sources(EM2Recomp PRIVATE em1_jigsaw_havok_probe.cpp)
target_sources(EM2Recomp PRIVATE em1_behavior_schemas.cpp em1_behavior_probe.cpp)
target_sources(EM2Recomp PRIVATE debug_level_browser.cpp)
set_property(SOURCE em1_audio_output.cpp PROPERTY INCLUDE_DIRECTORIES "${REXSDK}/include/SDL2")

target_sources(EM2Recomp PRIVATE online_actor_input.cpp)

target_sources(EM2Recomp PRIVATE online_name_input.cpp)
