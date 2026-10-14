add_executable(bta2dp-switch main.cpp)
target_link_libraries(bta2dp-switch PRIVATE bta_core setupapi newdev)
