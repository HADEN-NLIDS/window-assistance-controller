# ============================================================
#  Window Assistance Controller - CMake build
#  Generator: Visual Studio (x64) or Ninja with MSVC
#
#  Build:
#     cmake -S . -B out -G "Visual Studio 18 2026" -A x64
#     cmake --build out --config Release
#
#  NOTE: plain C project. Do NOT use a MinGW toolchain - it must be
#        x64 MSVC (see INSTRUCTION_MANUAL.md for why x64 is mandatory).
# ============================================================
cmake_minimum_required(VERSION 3.20)

project(WindowAssistanceController
        VERSION 0.1.0
        DESCRIPTION "Lightweight Windows window workspace controller"
        LANGUAGES C)

# ---- toolchain gates ----------------------------------------
#  Both checks exist because this machine (and many others) has a 32-bit
#  MinGW toolchain (i686-w64-mingw32) sitting earlier on PATH than MSVC.
#  Without these gates the failure is opaque, or worse, silently produces
#  a working-looking 32-bit binary that cannot read 64-bit process paths.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR MINGW OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "This project requires MSVC and refuses MinGW/Clang.\n"
        "  Detected compiler : ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER})\n"
        "  Reason            : 32-bit MinGW (i686-w64-mingw32) is unusable here,\n"
        "                      and mixed toolchains break the /MT + manifest build.\n"
        "  Fix               : configure a Visual Studio generator, e.g.\n"
        "                      cmake -S . -B out -G \"Visual Studio 17 2022\" -A x64\n"
        "                      see INSTRUCTION_MANUAL.md")
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "This project requires MSVC (x64). See INSTRUCTION_MANUAL.md")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "This project must be built as x64 (detected pointer size "
        "${CMAKE_SIZEOF_VOID_P}).\n"
        "  Reason: a 32-bit build suffers WOW64 redirection and cannot read\n"
        "          64-bit process image paths.\n"
        "  Fix   : configure with -A x64 (or -T host=x64).\n"
        "  see INSTRUCTION_MANUAL.md")
endif()

# ---- minimum MSVC version -----------------------------------
#  14.44 is the oldest toolchain this project has been verified against.
if(MSVC_VERSION LESS 1944)
    message(WARNING
        "MSVC ${MSVC_VERSION} is older than the verified minimum (1944 / VS 17.14).\n"
        "  The build may still work, but has not been tested.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---- unicode + lean headers ---------------------------------
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---- warnings: treat as errors ------------------------------
add_compile_options(/W4 /WX)

# ---- size-optimized, statically linked CRT -------------------
#  /MT is mandatory: the shipped exe must run on a clean Windows
#  machine with no VC++ redistributable installed.
add_compile_options(
    /O1        # optimize for size
    /GL        # whole program optimization
    /MT        # static CRT  <-- do not change
    /GS        # stack protection <-- keep enabled
    /Gy /Gw    # function/data level linking
    /GR-       # no RTTI
    /EHs-c-    # no C++ exceptions
)

# ---- encoding + preprocessor ---------------------------------
#  /utf-8            : sources and literals are UTF-8. Without it the Chinese
#                      comments in src/ raise C4819, and /WX makes that fatal.
#  /Zc:preprocessor  : conforming preprocessor. Needed for the variadic
#                      logging macros; see docs/架构与数据结构.md pitfall B.
add_compile_options(/utf-8 /Zc:preprocessor)

# ---- sources -------------------------------------------------
set(WAC_SOURCES
    src/main.c
)

foreach(subdir core model ui util cli)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${subdir}")
        file(GLOB _wac_sub "${CMAKE_CURRENT_SOURCE_DIR}/src/${subdir}/*.c")
        list(APPEND WAC_SOURCES ${_wac_sub})
    endif()
endforeach()

# stage 1: no sources yet is not an error
list(FILTER WAC_SOURCES EXCLUDE REGEX "\\.c$" )
set(WAC_REAL_SOURCES "")
foreach(f ${WAC_SOURCES})
    if(EXISTS "${f}")
        list(APPEND WAC_REAL_SOURCES "${f}")
    endif()
endforeach()

if(NOT WAC_REAL_SOURCES)
    message(STATUS "No sources under src/ yet - nothing to build (expected until stage 2).")
    return()
endif()

add_executable(wac WIN32 ${WAC_REAL_SOURCES})

# ---- resources ----------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/wac.rc")
    target_sources(wac PRIVATE res/wac.rc)
endif()

# ---- linker -------------------------------------------------
target_link_options(wac PRIVATE
    /LTCG
    /OPT:REF
    /OPT:ICF
    /INCREMENTAL:NO
    /MANIFEST:EMBED
    /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/res/wac.manifest
)

# ---- system libraries (all shipped with Windows) -------------
target_link_libraries(wac PRIVATE
    kernel32 user32 gdi32 shell32 shlwapi advapi32 ole32 oleaut32 dwmapi comctl32
)

# ---- keep PDBs for meaningful crash reports -----------------
if(MSVC)
    target_compile_options(wac PRIVATE /Zi)
    target_link_options(wac PRIVATE /DEBUG)
endif()

message(STATUS "WAC: ${PROJECT_VERSION} | sources: ${WAC_REAL_SOURCES}")
