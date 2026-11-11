# nesemu user C module for the rp2 port (own NES core, no external sources needed)
add_library(usermod_nesemu INTERFACE)

target_sources(usermod_nesemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/nesemu.c
)

target_include_directories(usermod_nesemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/nesemu.c PROPERTIES COMPILE_OPTIONS "-O3;-w")

target_link_libraries(usermod_nesemu INTERFACE hardware_dma hardware_spi hardware_clocks hardware_timer hardware_irq hardware_sync)

target_link_libraries(usermod INTERFACE usermod_nesemu)
