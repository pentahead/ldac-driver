# Core libraries (no OS device dependencies beyond Win32 basics) -----------------------------
add_library(bta_core STATIC
  common/status.cpp
  common/log.cpp
  common/json.cpp
  common/types.cpp
  common/settings.cpp
  a2dp/codec_caps.cpp
  a2dp/avdtp.cpp
  a2dp/avdtp_source.cpp
  a2dp/media.cpp
  codec/pcm.cpp
  codec/sbc_encoder.cpp
  codec/ldac_encoder.cpp
  transport/loopback.cpp
  pipeline/pcm_ring.cpp
  pipeline/source.cpp
  pipeline/stream_engine.cpp
  core/session_controller.cpp
  sim/sim_sink.cpp)
target_include_directories(bta_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(bta_core PUBLIC tp_sbc tp_ldac tp_ldac_abr avrt shell32 ole32)

# Platform integration (Bluetooth discovery, WASAPI, kernel transport, IPC) ------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/CMakeLists.txt)
  add_subdirectory(platform)
endif()
foreach(app service ctl config_app installer)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${app}/CMakeLists.txt)
    add_subdirectory(${app})
  endif()
endforeach()
