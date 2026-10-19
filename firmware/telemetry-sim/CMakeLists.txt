# ==============================================================================
#  SW06 driver's radio -- telemetry simulator (VCU stand-in)
#  NUCLEO-H755ZI-Q, Cortex-M7 core only
#
#  Bench fixture, not shipped firmware. It generates RS-485 telemetry frames
#  in the VCU format and drives them into the master's J101 with a bit-banged
#  complementary transmitter (bb_diff.c), so the master's receive path can be
#  loaded without the real VCU.
#
#  WHY ONLY THE M7: the H755 is dual-core, and CubeMX generated a CM4 project
#  too, but its main() is an empty loop and the boot synchronisation between
#  the cores is disabled in CM7/Core/Src/main.c. Everything the simulator does
#  runs on the M7. The CM4/ directory is left untouched and is not built here.
#  Whatever is in the M4's flash bank (empty, or an old CubeIDE image) runs or
#  faults on its own core without affecting the M7.
#
#  Same three-file structure as the boards: CMakePresets.json picks the
#  profile and the toolchain file, cmake/gcc-arm-none-eabi.cmake owns compiler
#  and CPU, this file owns everything else.
#
#      cmake --preset RelWithDebInfo
#      cmake --build --preset RelWithDebInfo
# ==============================================================================

cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
            "No toolchain file. Configure through a preset, e.g. "
            "`cmake --preset Debug`, rather than calling cmake directly.")
endif()

project(telemetry-sim C CXX ASM)

set(CMAKE_C_STANDARD    11)
set(CMAKE_CXX_STANDARD  17)

# --- Build types --------------------------------------------------------------
# Same profiles and flags as the boards, so one CLion setup fits all three.
# For a fixture, which profile it runs barely matters -- with one exception:
# bb_diff.c times its bits from the DWT cycle counter, not from instruction
# counts, so the transmitted baud rate is the same in every profile.
set(CMAKE_C_FLAGS_DEBUG             "-Og -g3")
set(CMAKE_C_FLAGS_RELWITHDEBINFO    "-O2 -g3")
set(CMAKE_C_FLAGS_MINSIZEREL        "-Os -g3")

set(CMAKE_CXX_FLAGS_DEBUG           "${CMAKE_C_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO  "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
set(CMAKE_CXX_FLAGS_MINSIZEREL      "${CMAKE_C_FLAGS_MINSIZEREL}")

set(CMAKE_ASM_FLAGS_DEBUG           "-g3")
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO  "-g3")
set(CMAKE_ASM_FLAGS_MINSIZEREL      "-g3")

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# --- Compile options ----------------------------------------------------------
add_compile_options(
        -ffunction-sections
        -fdata-sections
        -fno-common
        -fmessage-length=0
        -Wall
        $<$<COMPILE_LANGUAGE:C,CXX>:-fstack-usage>
)
add_compile_options($<$<COMPILE_LANGUAGE:ASM>:-x$<SEMICOLON>assembler-with-cpp>)

# --- Defines ------------------------------------------------------------------
# Taken from the CM7 project's .cproject.
#   CORE_CM7                    the shared system and HAL files compile
#                               differently per core
#   USE_PWR_DIRECT_SMPS_SUPPLY  the Nucleo-H755ZI-Q powers the core from its
#                               SMPS. A wrong supply setting here makes the
#                               chip lock up in SystemClock_Config, and on
#                               this board family can need a recovery boot
#                               to get it back -- do not change it.
#
#  Not USE_NUCLEO_144, although CubeIDE passed it too: the CubeMX-generated
#  CM7/Core/Inc/stm32h7xx_nucleo_conf.h already defines it, and defining it
#  in both places is a redefinition warning in every file that includes it.
add_compile_definitions(
        CORE_CM7
        STM32H755xx
        USE_HAL_DRIVER
        USE_PWR_DIRECT_SMPS_SUPPLY
)

# --- Include paths ------------------------------------------------------------
include_directories(
        CM7/Core/Inc
        Drivers/STM32H7xx_HAL_Driver/Inc
        Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
        Drivers/CMSIS/Device/ST/STM32H7xx/Include
        Drivers/CMSIS/Include
        Drivers/BSP/STM32H7xx_Nucleo
)

# --- Sources ------------------------------------------------------------------
# CM7 application, the dual-core SystemInit shared by both cores, the HAL
# (CubeMX copied only the modules in use) and the Nucleo BSP for LEDs, the
# user button and the ST-LINK virtual COM port.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
        "CM7/Core/*.c"
        "CM7/Core/*.s"
        "Common/Src/*.c"
        "Drivers/STM32H7xx_HAL_Driver/Src/*.c"
        "Drivers/BSP/STM32H7xx_Nucleo/*.c"
)

# --- Link ---------------------------------------------------------------------
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/CM7/STM32H755ZITX_FLASH.ld)

add_executable(${PROJECT_NAME} ${SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

target_link_options(${PROJECT_NAME} PRIVATE
        -T${LINKER_SCRIPT}
        --specs=nano.specs
        -Wl,--gc-sections
        -Wl,--print-memory-usage
        -Wl,-Map=${PROJECT_BINARY_DIR}/${PROJECT_NAME}.map
)

target_link_libraries(${PROJECT_NAME} PRIVATE m)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
        COMMENT "Generating .hex and .bin, then reporting size"
)
