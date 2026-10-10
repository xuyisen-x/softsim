add_library(ram_wrapper src/ramulator_wrapper.cpp)

target_include_directories(ram_wrapper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ram_wrapper PRIVATE ramulator)
