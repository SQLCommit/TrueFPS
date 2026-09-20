####################################################################################################
# Ashita - Copyright (c) 2025 Ashita Development Team
# Contact: https://www.ashitaxi.com/
# Contact: https://discord.gg/Ashita
#
# This file is part of Ashita.
#
# Ashita is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Ashita is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Ashita.  If not, see <https://www.gnu.org/licenses/>.
####################################################################################################
#
# Locates and prepares the Ashita SDK for usage with CMAKE.
#
# This module creates the following defines:
#
#   AshitaSDK_FOUND
#       Defines if the Ashita SDK package was properly found or not.
#
#   ASHITA_SDK_INCLUDE_DIRS
#       Defines the paths to the various Ashita SDK folders that contain include headers.
#
#   ASHITA_SDK_LIBRARY_DIRS
#       Defines the paths to the various Ashita SDK folders that contain lib files.
#
#   ASHITA_SDK_LIBRARY_LIBS
#       Defines the paths to the various Ashita SDK library files.
#
####################################################################################################
#
# Usage
#
#   find_package(AshitaSDK REQUIRED)
#
# Required Environment Variable: 'ASHITA4_SDK_PATH'
#
#   This module requires the usage of an environment variable 'ASHITA4_SDK_PATH' to properly
#   function and locate the Ashits v4 SDK. This can be defined user or system level of the
#   global environment variables or it can be defined within the local build environment when
#   executing CMAKE.
#
####################################################################################################

# Ashita plugins must be compiled as 32bit (x86) modules. 64bit is not supported.
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Ashita plugins must be compiled as 32bit (x86) modules!")
endif()

# Locate the main SDK path via the expected environment variable..
find_path(ASHITA_SDK_PATH
    NAMES
        "Ashita.h"
    PATHS
        ENV ASHITA4_SDK_PATH
    )

if (NOT ASHITA_SDK_PATH)
    message(FATAL_ERROR "Failed to locate the Ashita SDK! Missing or invalid expected environment variable: ASHITA4_SDK_PATH")
endif()

# Prepare the expected SDK paths..
set(ASHITA_SDK_PATH_D3D8       ${ASHITA_SDK_PATH}/d3d8)
set(ASHITA_SDK_PATH_D3D8_INC   ${ASHITA_SDK_PATH_D3D8}/includes/)
set(ASHITA_SDK_PATH_D3D8_LIB   ${ASHITA_SDK_PATH_D3D8}/lib/)
set(ASHITA_SDK_PATH_FFXI       ${ASHITA_SDK_PATH}/ffxi/)

# Prepare the Direct3D library files..
find_library(ASHITA_SDK_LIB_D3D8
    NAMES
        d3d8
    PATHS
        ${ASHITA_SDK_PATH_D3D8_LIB}
    NO_DEFAULT_PATH
    )
find_library(ASHITA_SDK_LIB_D3DX8
    NAMES
        d3dx8
    PATHS
        ${ASHITA_SDK_PATH_D3D8_LIB}
    NO_DEFAULT_PATH
    )
find_library(ASHITA_SDK_LIB_DINPUT8
    NAMES
        dinput8
    PATHS
        ${ASHITA_SDK_PATH_D3D8_LIB}
    NO_DEFAULT_PATH
    )
find_library(ASHITA_SDK_LIB_DXERR8
    NAMES
        dxerr8
    PATHS
        ${ASHITA_SDK_PATH_D3D8_LIB}
    NO_DEFAULT_PATH
    )
find_library(ASHITA_SDK_LIB_DXGUID
    NAMES
        dxguid
    PATHS
        ${ASHITA_SDK_PATH_D3D8_LIB}
    NO_DEFAULT_PATH
    )

# Validate everything required was found..
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AshitaSDK
    DEFAULT_MSG
        # Validate Folder Paths
        ASHITA_SDK_PATH
        ASHITA_SDK_PATH_D3D8
        ASHITA_SDK_PATH_D3D8_INC
        ASHITA_SDK_PATH_D3D8_LIB
        ASHITA_SDK_PATH_FFXI
        # Validate Library Paths
        ASHITA_SDK_LIB_D3D8
        ASHITA_SDK_LIB_D3DX8
        ASHITA_SDK_LIB_DINPUT8
        ASHITA_SDK_LIB_DXERR8
        ASHITA_SDK_LIB_DXGUID
    )

mark_as_advanced(
    ASHITA_SDK_PATH_D3D8
    ASHITA_SDK_PATH_D3D8_INC
    ASHITA_SDK_PATH_D3D8_LIB
    ASHITA_SDK_PATH_FFXI
    ASHITA_SDK_LIB_D3D8
    ASHITA_SDK_LIB_D3DX8
    ASHITA_SDK_LIB_DINPUT8
    ASHITA_SDK_LIB_DXERR8
    ASHITA_SDK_LIB_DXGUID
    )

# Expose SDK related paths for external use..
set(ASHITA_SDK_INCLUDE_DIRS
    ${ASHITA_SDK_PATH}
    ${ASHITA_SDK_PATH_D3D8_INC}
    ${ASHITA_SDK_PATH_FFXI}
    )

set(ASHITA_SDK_LIBRARY_DIRS
    ${ASHITA_SDK_PATH_D3D8_LIB}
    )

set(ASHITA_SDK_LIBRARY_LIBS
    ${ASHITA_SDK_LIB_D3D8}
    ${ASHITA_SDK_LIB_D3DX8}
    ${ASHITA_SDK_LIB_DINPUT8}
    ${ASHITA_SDK_LIB_DXERR8}
    ${ASHITA_SDK_LIB_DXGUID}
    )

mark_as_advanced(
    ASHITA_SDK_INCLUDE_DIRS
    ASHITA_SDK_LIBRARY_DIRS
    ASHITA_SDK_LIBRARY_LIBS
    )

####################################################################################################
#
# Helper Functions
#
# General helper functions that can be used with this module. These can be used to help define
# recommended configurations for compiling Ashita plugins.
#
####################################################################################################

if (MSVC)

    # Initializes the CMAKE environment for building Ashita plugins.
    function (ashita_sdk_initialize)
        # Configure CMAKE options..
        set(CMAKE_EXPORT_COMPILE_COMMANDS ON)           # Enable compiler command output.
        set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF) # Disable PATH environment variable usage.
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)         # Enable PIC for all targets.

        # Clear the default C++ flags, we override these..
        unset(CMAKE_CXX_FLAGS CACHE)
        unset(CMAKE_CXX_FLAGS_DEBUG CACHE)
        unset(CMAKE_CXX_FLAGS_MINSIZEREL CACHE)
        unset(CMAKE_CXX_FLAGS_RELEASE CACHE)
        unset(CMAKE_CXX_FLAGS_RELWITHDEBINFO CACHE)

        # Set the C++ standard level if not already set..
        if (NOT "${CMAKE_CXX_STANDARD}")
            set(CMAKE_CXX_STANDARD 23 CACHE STRING "Default C++ standard to be used.")
        endif()
    endfunction()

    # Attaches the SDK header and libraries to the given target.
    function (ashita_sdk_attach target)
        target_include_directories(${target}
            PRIVATE
                ${ASHITA_SDK_INCLUDE_DIRS}
            )
        target_link_directories(${target}
            PRIVATE
                ${ASHITA_SDK_LIBRARY_LIBS}
            )
        target_link_libraries(${target}
            PRIVATE
                ${ASHITA_SDK_LIBRARY_LIBS}

                # Ashita plugins can make use of Direct3D8 libraries which requires a legacy
                # lib on newer Visual Studio versions..
                "legacy_stdio_definitions.lib"
            )
    endfunction()

    # Sets the recommended compiler configurations for compiling Ashita plugins.
    function (ashita_sdk_set_compiler_defaults target)
        # Set general configuration independent options..
        target_compile_options(${target} PUBLIC
            /analyze-               # Disable code analysis..
            /bigobj                 # Increase number of sections object files can hold..
            /D_MBCS                 # Define: _MBCS
            /DWIN32                 # Define: WIN32
            /D_WINDOWS              # Define: _WINDOWS
            /diagnostics:column     # Enable rich diagnostics..
            /EHa                    # Enable C++ exceptions with SEH..
            /errorReport:prompt     # Sets the internal compile error reporting method..
            /FC                     # Use full paths in diagnostic messages..
            /fp:precise             # Sets the floating point model..
            /Gd                     # Default calling convention (__cdecl)
            /GF                     # Enable string pooling..
            /Gm-                    # Disable minimal rebuild..
            /GS                     # Enable securiy checks..
            /MP                     # Enable multi-processor compiling..
            /nologo                 # Disable tooling logo..
            /Oy-                    # Don't omit frame pointers..
            /permissive-            # Sets the conformance mode..
            /sdl                    # Enable SDL checks..
            /Zc:forScope            # Force comformance in for loop scope..
            /Zc:inline              # Remove unreferenced code..
            /Zc:wchar_t             # Treat wchar_t as built-in type..
            )

        # Set debug configuration options..
        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            target_compile_options(${target} PUBLIC
                /DDEBUG             # Define: DEBUG
                /MDd                # Use debug mode runtime library..
                /Ob0                # Default inline function expansion..
                /Od                 # Disable optimizations..
                /RTC1               # Enable basic runtime checks..
                /Zi                 # Enable program database..
                )
        endif()

        # Set release configuration options..
        if (CMAKE_BUILD_TYPE STREQUAL "Release")
            target_compile_options(${target} PUBLIC
                /DNDEBUG            # Define: NDEBUG
                /GL                 # Enable whole program optimization..
                /Gy-                # Disable function-level linking..
                /MD                 # Use release mode runtime library..
                /Ob2                # Default inline function expansion..
                /Oi                 # Generate intrinsic functions..
                /O2                 # Use maximum optimizations..
                )
        endif()
    endfunction()

    # Sets the recommended linker configurations for compiling Ashita plugins.
    function (ashita_sdk_set_linker_defaults target)
        # Set general configuration independent options..
        target_link_options(${target} PUBLIC
            /ERRORREPORT:PROMPT     # Sets the internal linker error reporting method..
            /MACHINE:X86            # Sets the target platform..
            /NODEFAULTLIB:libci     # Remove default library 'libci' to avoid Direct3D library issues..
            /NOLOGO                 # Suppress the startup banner..
            /NXCOMPAT               # Marks the DLL as DEP compatible..
            /TLBID:1                # Sets the default type library id..
            )

        # Set debug configuration options..
        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            target_link_options(${target} PUBLIC
                /INCREMENTAL        # Enable incremental building..
                )
        endif()

        # Set release configuration options..
        if (CMAKE_BUILD_TYPE STREQUAL "Release")
            target_link_options(${target} PUBLIC
                /INCREMENTAL:NO     # Disable incremental building..
                /LTCG:incremental   # Sets the linker to optimize only changed files..
                /NOCOFFGRPINFO      # Removes the debug directory from the resulting binary..
                /OPT:ICF            # Enables COMDAT folding..
                /OPT:REF            # Removes unused code from the resulting binary..
                )
        endif()
    endfunction()

    # Sets the compiler warning level for the given target.
    function (ashita_sdk_set_compiler_warn_level target level)
        if (level STREQUAL "")
            set(level 3)
        endif()
        target_compile_options(${target} PUBLIC "/W${level}")
    endfunction()

    # Sets the compiler to treat warnings as errors for the given target.
    function (ashita_sdk_set_compiler_warnings_as_errors target)
        target_compile_options(${target} PUBLIC "/WX")
    endfunction()

    # Sets the linker to generate debug information for the given target.
    function (ashita_sdk_set_linker_enable_debug_linking target)
        target_link_options(${target} PUBLIC /DEBUG)
    endfunction()

    # Sets the linker to disable generating debug information for the given target.
    function (ashita_sdk_set_linker_disable_debug_linking target)
        target_link_options(${target} PUBLIC /DEBUG:NONE)
    endfunction()

    # Sets the linker to enable safe exception handlers for the given target.
    function (ashita_sdk_set_linker_enable_safe_seh target)
        target_link_options(${target} PUBLIC /SAFESEH)
    endfunction()

endif()
