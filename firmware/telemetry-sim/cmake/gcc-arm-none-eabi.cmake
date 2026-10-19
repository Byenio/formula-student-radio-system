# ==============================================================================
#  Toolchain file -- telemetry simulator (NUCLEO-H755ZI-Q, Cortex-M7 core)
#
#  Same structure as the master's and slave's: owns WHICH compiler and WHAT
#  CPU, nothing else. Everything project-specific is in CMakeLists.txt.
#
#  The CPU line is the only real difference: the H755's M7 has the
#  double-precision FPU (fpv5-d16), unlike the master's M4 (fpv4-sp-d16) or
#  the slave's M33 (fpv5-sp-d16). Kept as a separate file for the same reason
#  as the boards' -- a shared one would silently build for the wrong core.
#
#  Selected by CMakePresets.json; nothing else should reference this file.
# ==============================================================================

set(CMAKE_SYSTEM_NAME           Generic)
set(CMAKE_SYSTEM_PROCESSOR      arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(TOOLCHAIN_PREFIX            arm-none-eabi-)
set(CMAKE_C_COMPILER            ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER          ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER          ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY               ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                  ${TOOLCHAIN_PREFIX}size)

set(CMAKE_EXECUTABLE_SUFFIX_C   .elf)
set(CMAKE_EXECUTABLE_SUFFIX_CXX .elf)
set(CMAKE_EXECUTABLE_SUFFIX_ASM .elf)

# Cortex-M7, double-precision FPU, hard-float ABI -- as CubeIDE built it.
# Reaches the link line through CMAKE_C_FLAGS, which selects the matching
# newlib multilib (thumb/v7e-m+dp/hard).
set(TARGET_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")

set(CMAKE_C_FLAGS_INIT          "${TARGET_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${TARGET_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT        "${TARGET_FLAGS}")
