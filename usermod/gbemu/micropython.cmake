# gbemu user C module for the rp2 port
add_library(usermod_gbemu INTERFACE)

target_sources(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/gbemu.c
)

# peanut_gb.h is downloaded here by the build workflow
target_include_directories(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Peanut-GB is not warning-clean under MicroPython's -Werror; speed matters more than size here
target_compile_options(usermod_gbemu INTERFACE -O3 -w)

target_link_libraries(usermod INTERFACE usermod_gbemu)
