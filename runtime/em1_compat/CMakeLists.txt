cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EM1Adapters LANGUAGES C CXX)
else()
  enable_language(C)
endif()

# Official Zstandard decoder only, BSD license. Never builds a CLI/shared DLL.
set(EM1_ZSTD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib")
add_library(em1_zstd STATIC
  ${EM1_ZSTD_ROOT}/common/debug.c
  ${EM1_ZSTD_ROOT}/common/entropy_common.c
  ${EM1_ZSTD_ROOT}/common/error_private.c
  ${EM1_ZSTD_ROOT}/common/fse_decompress.c
  ${EM1_ZSTD_ROOT}/common/xxhash.c
  ${EM1_ZSTD_ROOT}/common/zstd_common.c
  ${EM1_ZSTD_ROOT}/decompress/huf_decompress.c
  ${EM1_ZSTD_ROOT}/decompress/zstd_ddict.c
  ${EM1_ZSTD_ROOT}/decompress/zstd_decompress.c
  ${EM1_ZSTD_ROOT}/decompress/zstd_decompress_block.c)
target_compile_definitions(em1_zstd PRIVATE HUF_DISABLE_ASM ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
add_library(em1_zlib STATIC zlib/adler32.c zlib/deflate.c zlib/trees.c zlib/inflate.c zlib/inftrees.c
  zlib/inffast.c zlib/zutil.c)
target_compile_definitions(em1_zlib PRIVATE Z_SOLO Z_PREFIX NO_GZIP)
add_library(em1_compat STATIC ../em1_scene_adapter.cpp ../em1_rvz.cpp
  ../em1_pack_source.cpp ../em1_inflate.cpp ../em1_level_source.cpp ../em1_kfm.cpp ../em1_keyframe.cpp ../em1_kfm_closure.cpp ../em1_resource_catalog.cpp
  ../em1_nif_textures.cpp ../em1_gx_mesh.cpp ../em1_nif_meshes.cpp
  ../em1_model_source.cpp ../em1_model_closure.cpp ../em1_havok_collision.cpp ../em1_preview_scene.cpp ../em1_jigsaw_havok.cpp ../em1_behavior_upgrade.cpp)
target_sources(em1_compat PRIVATE ../em1_ammo_change.cpp ../em1_ammo_recharge.cpp ../em1_deflate.cpp ../em1_level_collision_plan.cpp)
target_sources(em1_compat PRIVATE ../em1_ink_restoration.cpp)
target_sources(em1_compat PRIVATE ../em1_navigation_data.cpp ../em1_navigation_mesh.cpp ../em1_navigation_query.cpp)
target_compile_features(em1_compat PUBLIC cxx_std_20)
target_include_directories(em1_compat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(em1_compat PRIVATE em1_zstd em1_zlib bcrypt)

# The game already provides ReXGlue. Developer validators can explicitly load
# its installed package; the pure format adapters remain SDK-independent.
option(EM1_ADAPTER_WITH_REX_VFS "Enable the existing ReXGlue VFS integration in a standalone developer build" OFF)
if(EM1_ADAPTER_WITH_REX_VFS AND NOT TARGET rex::filesystem)
  find_package(rexglue CONFIG REQUIRED)
endif()
if(TARGET rex::filesystem)
  target_compile_features(em1_compat PUBLIC cxx_std_23)
  target_sources(em1_compat PRIVATE ../em1_vfs.cpp ../em1_vfs_copy.cpp)
  set_property(SOURCE ../em1_vfs_copy.cpp PROPERTY SKIP_PRECOMPILE_HEADERS ON)
  set_property(SOURCE ../em1_vfs_copy.cpp APPEND PROPERTY COMPILE_OPTIONS /EHa /clang:-fno-lto)
  target_link_libraries(em1_compat PRIVATE rex::filesystem)
endif()

# These are developer validation tools, never installed or required at runtime.
option(EM1_ADAPTER_BUILD_TESTS "Build developer-only original-format validators" OFF)
if(EM1_ADAPTER_BUILD_TESTS)
  add_executable(em1_scene_adapter_test ../../scripts/em1_scene_adapter_test.cpp)
  target_link_libraries(em1_scene_adapter_test PRIVATE em1_compat)
  add_executable(em1_rvz_test ../../scripts/em1_rvz_test.cpp)
  target_link_libraries(em1_rvz_test PRIVATE em1_compat)
  add_executable(em1_resource_test ../../scripts/em1_resource_test.cpp)
  target_link_libraries(em1_resource_test PRIVATE em1_compat)
  add_executable(em1_nif_texture_test ../../scripts/em1_nif_texture_test.cpp)
  target_link_libraries(em1_nif_texture_test PRIVATE em1_compat)
  if(TARGET rex::filesystem)
    add_executable(em1_vfs_test ../../scripts/em1_vfs_test.cpp)
    target_link_libraries(em1_vfs_test PRIVATE em1_compat rex::filesystem)
  endif()
endif()
