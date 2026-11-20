add_library(openscope openscope/instrument.cpp)
target_include_directories(openscope PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openscope PRIVATE openscope_warnings)
target_compile_definitions(openscope PRIVATE OPENSCOPE_VERSION="${PROJECT_VERSION}")
