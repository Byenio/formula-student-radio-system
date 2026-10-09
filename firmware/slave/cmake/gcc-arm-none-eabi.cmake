# ==============================================================================
#  Toolchain file -- slave board (STM32U375CET6Q, Cortex-M33F)
#
#  Owns exactly two things: WHICH compiler, and WHAT CPU it targets. Both are
#  properties of the hardware, so they are the same for any project built for
#  this chip. Everything that is a property of THIS project -- linker script,
#  libc variant, optimisation level, warnings, sources -- lives in
#  CMakeLists.txt and must not appear here.
#
#  Mirrors the master's toolchain file; the only difference is the CPU line.
#  Kept as a separate file rather than shared, because a shared file with the
#  M4 flags baked in would silently build the slave for the wrong core.
#
#  Selected by CMakePresets.json; nothing else should reference this file.
# ==============================================================================

set(CMAKE_SYSTEM_NAME           Generic)
set(CMAKE_SYSTEM_PROCESSOR      arm)

# A bare-metal cross-compiler cannot link a test executable for the host, so
# CMake's compiler checks must stop at building a static library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# --- Compiler and binutils ----------------------------------------------------
# Resolved from PATH. In CLion that PATH comes from the STM32 CLT toolchain's
# environment file; on the command line, from wherever arm-none-eabi-gcc is
# installed.
set(TOOLCHAIN_PREFIX            arm-none-eabi-)

set(CMAKE_C_COMPILER            ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER          ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER          ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY               ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                  ${TOOLCHAIN_PREFIX}size)

# Executables come out as <target>.elf. CMakeLists therefore names the target
# plainly -- add_executable(master ...) -- and gets master.elf.
set(CMAKE_EXECUTABLE_SUFFIX_C   .elf)
set(CMAKE_EXECUTABLE_SUFFIX_CXX .elf)
set(CMAKE_EXECUTABLE_SUFFIX_ASM .elf)

# --- CPU ----------------------------------------------------------------------
# Cortex-M33 with the single-precision FPU: fpv5-sp-d16, not the master's
# fpv4-sp-d16. No -mcmse -- the firmware is one non-TrustZone image. The slave
# is bare metal and does no floating-point work, but hard-float is kept so the
# HAL and USBX link against the hard-float libc multilib like everything else.
#
# These must also reach the link line, where GCC uses them to select the
# matching multilib -- the build of newlib compiled for this exact FPU and
# float ABI. CMake already passes CMAKE_C_FLAGS to the link driver, so that
# happens without a separate CMAKE_EXE_LINKER_FLAGS_INIT; adding one would put
# every CPU flag on the link line twice.
set(TARGET_FLAGS "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard")

# The *_INIT variables, not CMAKE_C_FLAGS itself. CMake can process a toolchain
# file more than once during configuration, so `set(CMAKE_C_FLAGS
# "${CMAKE_C_FLAGS} ...")` -- which is what the CubeMX version did -- appends
# the flags again on every pass. *_INIT is read once, to seed the cache.
set(CMAKE_C_FLAGS_INIT          "${TARGET_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT        "${TARGET_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT        "${TARGET_FLAGS}")
