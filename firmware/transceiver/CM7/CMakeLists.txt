cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard " )

#Linker options
set(STM32_LINKER_SCRIPT stm32h755xx_flash_CM7.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME transceiver_CM7)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Include mx-generated
include("mx-generated.cmake")

# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# OPUS configuration
set(OPUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
set(OPUS_ENABLE_FLOAT_API OFF CACHE BOOL "" FORCE)
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)

# Bare-metal specific Opus flags
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
set(OPUS_HARDENING OFF CACHE BOOL "" FORCE)
set(OPUS_USE_NEON OFF CACHE BOOL "" FORCE)

# Prevent CMake from trying to install Opus when building the main project
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/../Middlewares/Third_Party/opus ${CMAKE_BINARY_DIR}/opus EXCLUDE_FROM_ALL)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/../Middlewares/Third_Party/opus/include
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Core/Src/main.c
        Core/Src/stm32h7xx_it.c
        Core/Src/stm32h7xx_hal_msp.c
        Core/Src/adpcm.c
        # Add user sources here
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    opus
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
