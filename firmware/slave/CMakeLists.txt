# ==============================================================================
#  SW06 driver's radio -- slave (pit-wall base station)
#  STM32U375CET6Q, Cortex-M33F @ 96 MHz, bare metal + USBX device CDC-ACM
#
#  Owns everything that is a property of THIS project: optimisation, warnings,
#  defines, include paths, sources, linker script and libc variant. Compiler
#  selection and CPU flags belong to cmake/gcc-arm-none-eabi.cmake, which
#  CMakePresets.json passes in. Neither file sets anything the other does --
#  see the toolchain file header for what went wrong when they overlapped.
#
#  Build with a preset, never by hand:
#      cmake --preset RelWithDebInfo
#      cmake --build --preset RelWithDebInfo
#
#  CubeMX does not touch this file (the .ioc targets STM32CubeIDE). A
#  regeneration that adds a NEW directory needs its include path added below
#  by hand; new files in existing directories are picked up automatically.
# ==============================================================================

cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
            "No toolchain file. Configure through a preset, e.g. "
            "`cmake --preset Debug`, rather than calling cmake directly.")
endif()

project(slave C CXX ASM)

set(CMAKE_C_STANDARD    11)
set(CMAKE_CXX_STANDARD  17)

# --- Build types --------------------------------------------------------------
#
#   Debug           -Og  -g3   Day-to-day development. -Og is the level GCC
#                              designs for debugging: breakpoints land where
#                              expected and variables stay readable, unlike
#                              -O2. Also enables the dataflow analysis several
#                              -Wall checks depend on, which -O0 skips.
#
#   RelWithDebInfo  -O2  -g3   THE MEASUREMENT AND SHIPPING BUILD. Every figure
#                              in the test campaign is taken on this profile,
#                              because it is the binary that runs on the car.
#                              -g3 costs no flash: debug info lives in ELF
#                              sections that are never loaded onto the chip.
#
#   MinSizeRel      -Os  -g3   Kept for the optimisation comparison, and in
#                              case flash ever becomes the constraint.
#
#  No plain "Release". With -g free, it would produce a binary identical to
#  RelWithDebInfo but impossible to debug.
#
#  Never -Ofast. It implies -ffast-math, which lets the compiler reorder
#  floating-point arithmetic and abandon IEEE semantics. The slave does no
#  float work today, so this costs nothing -- it is here so the two boards'
#  build policies stay identical and nobody has to remember which one differs.
#
#  NDEBUG is deliberately NOT defined, unlike CMake's defaults for these
#  profiles, so assertion behaviour is identical between the build being
#  debugged and the build being shipped.
#
#  Plain set() after project(), rather than the cache, so that editing a value
#  here takes effect on the next configure without a cache reset.
set(CMAKE_C_FLAGS_DEBUG             "-Og -g3")
set(CMAKE_C_FLAGS_RELWITHDEBINFO    "-O2 -g3")
set(CMAKE_C_FLAGS_MINSIZEREL        "-Os -g3")

set(CMAKE_CXX_FLAGS_DEBUG           "${CMAKE_C_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO  "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
set(CMAKE_CXX_FLAGS_MINSIZEREL      "${CMAKE_C_FLAGS_MINSIZEREL}")

set(CMAKE_ASM_FLAGS_DEBUG           "-g3")
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO  "-g3")
set(CMAKE_ASM_FLAGS_MINSIZEREL      "-g3")

# Optimisation sweep hook. Set to e.g. -O3 to override the profile's level
# without editing this file:
#     cmake --preset RelWithDebInfo -DFW_OPT_OVERRIDE=-O3
# Appended after the profile flags; GCC honours the last -O it sees. Empty by
# default, and reported at configure time when set, so a sweep build is never
# mistaken for the real one.
set(FW_OPT_OVERRIDE "" CACHE STRING "Override optimisation level, e.g. -O3")
if(FW_OPT_OVERRIDE)
  message(WARNING "Optimisation overridden to ${FW_OPT_OVERRIDE} -- not a release build")
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:${FW_OPT_OVERRIDE}>)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# --- Compile options ----------------------------------------------------------
add_compile_options(
        # One section per function and per variable, so --gc-sections at link
        # time can discard everything unreferenced.
        -ffunction-sections
        -fdata-sections

        -fno-common
        -fmessage-length=0
        -Wall

        # Writes a .su file beside every object, giving each function's stack
        # frame size. The slave has no RTOS and so no per-task high-water
        # mark; this static figure is the main stack evidence it has. Costs
        # build time only.
        $<$<COMPILE_LANGUAGE:C,CXX>:-fstack-usage>
)

# Preprocess .s files, so the startup code can use #ifdef.
add_compile_options($<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>)

# --- Defines ------------------------------------------------------------------
# No DEBUG define: CubeIDE added one, but nothing in this project tests it.
#
# UX_INCLUDE_USER_DEFINE_FILE makes USBX read USBX/App/ux_user.h. Drop it and
# the stack silently falls back to its own defaults -- including losing
# UX_STANDALONE, which is what lets USBX run without ThreadX on this board.
add_compile_definitions(
        USE_HAL_DRIVER
        STM32U375xx
        UX_INCLUDE_USER_DEFINE_FILE
)

# --- Include paths ------------------------------------------------------------
# ADD NEW INCLUDE PATHS HERE after a CubeMX regeneration adds a middleware.
include_directories(
        Core/Inc

        USBX/App
        USBX/Target

        Drivers/STM32U3xx_HAL_Driver/Inc
        Drivers/STM32U3xx_HAL_Driver/Inc/Legacy
        Drivers/CMSIS/Device/ST/STM32U3xx/Include
        Drivers/CMSIS/Include

        Middlewares/ST/usbx/common/core/inc
        Middlewares/ST/usbx/common/usbx_device_classes/inc
        Middlewares/ST/usbx/common/usbx_stm32_device_controllers
        Middlewares/ST/usbx/ports/generic/inc
)

# --- Sources ------------------------------------------------------------------
# CONFIGURE_DEPENDS re-runs the glob when directory contents change, so a file
# CubeMX adds is built next time instead of silently missing.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
        "Core/*.c"
        "Core/*.s"
        "USBX/*.c"
        "Drivers/*.c"
        "Middlewares/*.c"
)

# --- Link ---------------------------------------------------------------------
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/STM32U375CETXQ_FLASH.ld)

# Target is plain "slave"; the toolchain file's suffix makes it slave.elf.
add_executable(${PROJECT_NAME} ${SOURCES}
               Core/Inc/safety.h
               Core/Src/safety.c)

# Relink when the linker script changes, not only when sources do.
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

target_link_options(${PROJECT_NAME} PRIVATE
        -T${LINKER_SCRIPT}

        # newlib-nano: the reduced C library. Full newlib adds ~1.2 kB .data
        # and ~0.9 kB .bss of printf machinery this firmware never calls.
        # Core/Src/syscalls.c and sysmem.c provide the OS stubs, so do NOT add
        # --specs=nosys.specs as well; the two collide on _write and _sbrk.
        --specs=nano.specs

        -Wl,--gc-sections
        -Wl,--print-memory-usage
        -Wl,-Map=${PROJECT_BINARY_DIR}/${PROJECT_NAME}.map
)

target_link_libraries(${PROJECT_NAME} PRIVATE m)

# --- Post-build ---------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
        COMMENT "Generating .hex and .bin, then reporting size"
)
