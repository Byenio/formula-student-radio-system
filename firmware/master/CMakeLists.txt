# ==============================================================================
#  SW06 driver's radio -- master (on-car board)
#  STM32G491CET6, Cortex-M4F @ 170 MHz, FreeRTOS / CMSIS-RTOS v2
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

project(master C CXX ASM)

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
#  floating-point arithmetic and abandon IEEE semantics. The audio path is
#  exactly what the campaign measures for quality, and its results must not
#  change between builds because the compiler felt free to reassociate a sum.
#
#  NDEBUG is deliberately NOT defined, unlike CMake's defaults for these
#  profiles. configASSERT does not depend on it, but leaving it out keeps the
#  shipping build's assertion behaviour identical to the one being debugged: a
#  latched assert on the car beats silent corruption.
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

# --- Statistics and instrumentation -------------------------------------------
# ON for bench and measurement builds. OFF removes the whole layer -- perf
# markers become no-ops, statlink/stats compile to empty objects, and on the
# master FreeRTOS run-time stats are off too. Comparing an ON and an OFF build
# of the same commit is how the layer's own cost is measured.
#     cmake --preset RelWithDebInfo -DFW_STATS=OFF
option(FW_STATS "Build the statistics and instrumentation layer" ON)
message(STATUS "Statistics layer: ${FW_STATS}")

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
        # frame size. This is the static half of the stack analysis -- the
        # runtime half is uxTaskGetStackHighWaterMark(). Needed to size the
        # audio task's stack before a codec goes in. Costs build time only.
        $<$<COMPILE_LANGUAGE:C,CXX>:-fstack-usage>
)

# Preprocess .s files, so the startup code can use #ifdef.
add_compile_options($<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>)

# --- Defines ------------------------------------------------------------------
# No DEBUG define: CubeIDE added one, but nothing in this project tests it.
add_compile_definitions(
        FW_STATS_ENABLE=$<BOOL:${FW_STATS}>
        USE_HAL_DRIVER
        STM32G491xx
)

# --- Include paths ------------------------------------------------------------
# ADD NEW INCLUDE PATHS HERE after a CubeMX regeneration adds a middleware.
include_directories(
        Core/Inc

        Drivers/STM32G4xx_HAL_Driver/Inc
        Drivers/STM32G4xx_HAL_Driver/Inc/Legacy
        Drivers/CMSIS/Device/ST/STM32G4xx/Include
        Drivers/CMSIS/Include

        Middlewares/Third_Party/FreeRTOS/Source/include
        Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
        Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
)

# --- Sources ------------------------------------------------------------------
# CONFIGURE_DEPENDS re-runs the glob when directory contents change, so a file
# CubeMX adds is built next time instead of silently missing.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
     "Core/*.c"
     "Core/*.s"
     "Drivers/*.c"
     "Middlewares/*.c"
)

# --- Link ---------------------------------------------------------------------
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/STM32G491CETX_FLASH.ld)

# Target is plain "master"; the toolchain file's suffix makes it master.elf.
add_executable(${PROJECT_NAME} ${SOURCES})

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

# --- Build identity -----------------------------------------------------------
# Regenerates build_info.h (git hash, dirty flag, build type) on every build;
# the stats layer puts it in every schema frame, so each log names the exact
# firmware that produced it. See cmake/build_info.cmake.
set(BUILD_INFO_DIR ${PROJECT_BINARY_DIR}/generated)
add_custom_target(build_info
                  COMMAND ${CMAKE_COMMAND}
                  -DOUT=${BUILD_INFO_DIR}/build_info.h
                  -DSRC_DIR=${CMAKE_SOURCE_DIR}
                  -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
                  -DOPT_OVERRIDE=${FW_OPT_OVERRIDE}
                  -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
                  BYPRODUCTS ${BUILD_INFO_DIR}/build_info.h
                  COMMENT "Stamping build identity"
                  VERBATIM
)
add_dependencies(${PROJECT_NAME} build_info)
target_include_directories(${PROJECT_NAME} PRIVATE ${BUILD_INFO_DIR})

# --- Post-build ---------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                   COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
                   COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
                   COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
                   COMMENT "Generating .hex and .bin, then reporting size"
)