# msvc-wine cross toolchain for CP2077 RED4ext DLLs
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_CROSSCOMPILING ON)

set(MSVC_TOOLS /mnt/sata/msvc/bin/x64)

set(CMAKE_C_COMPILER   ${MSVC_TOOLS}/cl)
set(CMAKE_CXX_COMPILER ${MSVC_TOOLS}/cl)
set(CMAKE_RC_COMPILER  ${MSVC_TOOLS}/rc)
set(CMAKE_MT           ${MSVC_TOOLS}/mt)
set(CMAKE_LINKER       ${MSVC_TOOLS}/link)
set(CMAKE_AR           ${MSVC_TOOLS}/lib)

# Compiler id: build a static lib so no full link/run is attempted for detection.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Search host tools on host, libs/headers only in the MSVC/SDK env (via INCLUDE/LIB).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
