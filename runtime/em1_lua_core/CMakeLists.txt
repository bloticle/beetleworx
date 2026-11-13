find_program(EM1_CARGO cargo HINTS "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EM1_LUA_TARGET "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(EM1_LUA_LIBRARY "${EM1_LUA_TARGET}/release/em1_lua_core.lib")
set(EM1_LUA_C_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/lua5.1.lib")
add_custom_command(OUTPUT "${EM1_LUA_LIBRARY}" "${EM1_LUA_C_LIBRARY}"
  COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${EM1_LUA_TARGET}"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_native.py"
    "${EM1_CARGO}" "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" "${EM1_LUA_C_LIBRARY}"
  DEPENDS Cargo.toml Cargo.lock src/chd_images.rs src/disc_images.rs src/lib.rs src/entities.rs src/primary_player.rs src/source_modules.rs src/animation_events.rs src/engine_bridge.rs src/scheduler.rs src/global_state.rs src/global_bridge.rs src/callback_preflight.rs src/callback_arity.rs src/callback_calls.rs src/prefab_data.rs src/source_vector.rs src/source_map.rs build_native.py
  COMMENT "Building embedded native EM1 Lua library" VERBATIM)
add_custom_target(em1_lua_build DEPENDS "${EM1_LUA_LIBRARY}" "${EM1_LUA_C_LIBRARY}")
add_library(em1_lua_native STATIC IMPORTED GLOBAL)
set_target_properties(em1_lua_native PROPERTIES IMPORTED_LOCATION "${EM1_LUA_LIBRARY}")
add_dependencies(em1_lua_native em1_lua_build)
add_library(em1_lua_c STATIC IMPORTED GLOBAL)
set_target_properties(em1_lua_c PROPERTIES IMPORTED_LOCATION "${EM1_LUA_C_LIBRARY}")
add_dependencies(em1_lua_c em1_lua_build)
target_link_libraries(em1_lua_native INTERFACE em1_lua_c userenv ntdll bcrypt ws2_32)
