add_library(bta_platform STATIC
  bt_discovery.cpp
  kernel_l2cap.cpp
  wasapi_source.cpp
  ipc.cpp
  service_core.cpp)
target_include_directories(bta_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bta_platform PUBLIC bta_core bthprops ws2_32 cfgmgr32 ole32 advapi32)
