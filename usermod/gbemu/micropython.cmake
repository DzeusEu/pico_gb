# gbemu user C module for the rp2 port
add_library(usermod_gbemu INTERFACE)

target_sources(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/gbemu.c
)

# peanut_gb.h is downloaded here by the build workflow
target_include_directories(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Optimise only this file (Peanut-GB is not warning-clean under MicroPython's flags)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/gbemu.c PROPERTIES COMPILE_OPTIONS "-O3;-w")

# SPI + DMA are used to stream frames to the display
target_link_libraries(usermod_gbemu INTERFACE hardware_dma hardware_spi hardware_clocks hardware_timer)

target_link_libraries(usermod INTERFACE usermod_gbemu)
