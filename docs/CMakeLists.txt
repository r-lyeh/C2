cmake_minimum_required(VERSION 3.15)

# MSVC: static C runtime everywhere (/MT, /MTd in Debug), MIR included, so c2.exe needs no
# VCRUNTIME/UCRT DLLs and scripts, C² and MIR share one CRT (one heap, one stdio).
# Set before project().
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(c2 C)
set(CMAKE_C_STANDARD 11)

# This file lives in docs/: `cmake -S docs -B .build-cmake && cmake --build .build-cmake --config Release`
# from the repository root.  ROOT is the repository (c2.c, mir/, tests/).
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# CMake is optional: `cl c2.c 3rd\mir.c 3rd\mir-gen.c` builds the same compiler (see c2.c).
option(NO_MIR "Build C² without MIR (no JIT: only -S and -E)" OFF)

if(NOT NO_MIR)
    # MIR, vendored in 3rd/: only mir.c and mir-gen.c are compiled (they #include the interpreter
    # and the back end of the target).  Flags as in MIR's own build.
    add_library(mir STATIC ${ROOT}/3rd/mir.c ${ROOT}/3rd/mir-gen.c)
    find_package(Threads REQUIRED)
    target_link_libraries(mir PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    if(MSVC)
        target_compile_options(mir PRIVATE /MP)   # compile both files in parallel
    else()
        target_compile_options(mir PRIVATE -Wno-abi -fsigned-char -fPIC $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            target_compile_options(mir PRIVATE -fno-tree-sra -fno-ipa-cp-clone)
        endif()
        target_link_libraries(mir PUBLIC m)
    endif()
endif()

# c2(.exe) goes straight into the repository root, next to tests/ (no copy step).  The generator
# expression stops Visual Studio and other multi-config generators from adding a Release/ subfolder.
add_executable(c2 ${ROOT}/c2.c)
set_target_properties(c2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<1:${ROOT}>)
if(NO_MIR)
    target_compile_definitions(c2 PRIVATE NO_MIR)
    if(NOT WIN32)
        target_link_libraries(c2 PRIVATE m ${CMAKE_DL_LIBS})
    endif()
else()
    target_link_libraries(c2 PRIVATE mir)

    # C² as a library (c2.h) and the embedding example
    add_library(c2_lib STATIC ${ROOT}/c2.c)
    target_compile_definitions(c2_lib PRIVATE C2_NO_MAIN)
    target_include_directories(c2_lib PUBLIC ${ROOT})
    target_link_libraries(c2_lib PUBLIC mir)

    add_executable(c2_example ${ROOT}/example.c)
    target_link_libraries(c2_example PRIVATE c2_lib)
endif()
