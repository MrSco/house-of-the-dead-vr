# The app on Windows (docs/platforms.md, P1 and P4): HOTDVR.exe. The game (arcade_core) with the shared C (src/core),
# the flat shell (src/flat), the VR shell (src/xr, PCVR through OpenXR), online play (src/net), voice chat and its
# libraries, and the Windows platform (src/platform/windows). Needs third_party/quest: the SDL2 headers and the voice
# libraries quest_tools.py fetches, and M2_OPENXR_WIN: Khronos's OpenXR loader for Windows (include/, lib/, bin/;
# python scripts/build_windows.py fetches them all).
set(M2_TP ${CMAKE_SOURCE_DIR}/third_party/quest)
set(M2_OPUS ${M2_TP}/opus-1.5.2)
set(M2_SPEEX ${M2_TP}/speexdsp-1.2.1)
set(M2_RNN ${M2_TP}/rnnoise-0.2)
if(NOT (EXISTS ${M2_OPUS}/CMakeLists.txt AND EXISTS ${M2_SPEEX}/include/speex/speexdsp_config_types.h AND EXISTS ${M2_RNN}/src/denoise.c))
    message(STATUS "HOTDVR.exe: no voice libraries in third_party/quest (python scripts/build_windows.py fetches them)")
    return()
endif()
if(NOT (M2_OPENXR_WIN AND EXISTS ${M2_OPENXR_WIN}/lib/openxr_loader.lib AND EXISTS ${M2_OPENXR_WIN}/include/openxr/openxr.h))
    message(STATUS "HOTDVR.exe: no OpenXR loader (M2_OPENXR_WIN; python scripts/build_windows.py fetches it)")
    return()
endif()

# Opus with its own build (its x86 SIMD checked at run time), quietly
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${M2_OPUS} ${CMAKE_BINARY_DIR}/opus EXCLUDE_FROM_ALL)
if(MSVC)
    # (/fp:strict, the game's, makes MSVC refuse floating-point constants in initializers; voice needs no exact FP)
    target_compile_options(opus PRIVATE /W0 /fp:precise)
endif()
# SpeexDSP: the echo canceller alone, floating point with its own small FFT (as the Quest build)
add_library(m2_speexdsp STATIC ${M2_SPEEX}/libspeexdsp/mdf.c ${M2_SPEEX}/libspeexdsp/fftwrap.c ${M2_SPEEX}/libspeexdsp/smallft.c)
target_compile_definitions(m2_speexdsp PRIVATE FLOATING_POINT USE_SMALLFT EXPORT= _CRT_SECURE_NO_WARNINGS)
target_include_directories(m2_speexdsp PUBLIC ${M2_SPEEX}/include)
# RNNoise with its trained model; its code wants Opus's celt/os_support.h (left out of its archive)
add_library(m2_rnnoise STATIC ${M2_RNN}/src/denoise.c ${M2_RNN}/src/rnn.c ${M2_RNN}/src/pitch.c ${M2_RNN}/src/kiss_fft.c
            ${M2_RNN}/src/celt_lpc.c ${M2_RNN}/src/nnet.c ${M2_RNN}/src/nnet_default.c
            ${M2_RNN}/src/parse_lpcnet_weights.c ${M2_RNN}/src/rnnoise_data.c ${M2_RNN}/src/rnnoise_tables.c)
target_include_directories(m2_rnnoise PUBLIC ${M2_RNN}/include PRIVATE ${M2_RNN}/src ${M2_OPUS}/celt ${M2_OPUS}/include)
target_compile_definitions(m2_rnnoise PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(m2_speexdsp PRIVATE /W0 /fp:precise)
    target_compile_options(m2_rnnoise PRIVATE /W0 /fp:precise)
endif()

set(M2_FLAT_C
    src/flat/win/main_win.c src/flat/win/pad_win.c src/flat/flat.c src/flat/flat_app.c src/flat/flat_menu.c src/flat/flat_touch.c
    src/core/host_sdl.c src/core/voice.c src/core/voice_dsp.c src/core/m2_gl.c
    src/core/quest_menu.c src/core/quest_canvas.c src/core/quest_options.c
    src/platform/windows/platform_win.c src/platform/windows/audio_win.c src/platform/windows/gl_win.c
    src/platform/windows/crash_win.c
    src/xr/xr_shell.c src/xr/xr_win.c src/xr/quest_gun.c src/xr/quest_hand.c src/xr/quest_cabinet.c)
add_executable(hotdvr WIN32 ${M2_FLAT_C} ${M2_NET_C} src/flat/win/hotdvr.rc)   # (.rc: its icon)
set_source_files_properties(src/platform/windows/crash_win.c PROPERTIES COMPILE_DEFINITIONS "${M2_BUILD_INFO_DEFS}")
set_target_properties(hotdvr PROPERTIES OUTPUT_NAME HOTDVR C_STANDARD 11)
target_include_directories(hotdvr PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/xr
                           ${M2_NET_SRC} ${M2_QUEST_SDL2} ${M2_OPENXR_WIN}/include)
target_compile_definitions(hotdvr PRIVATE HOTD_ARCADE HOTD_VOICE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(hotdvr PRIVATE arcade_core runtime opus m2_speexdsp m2_rnnoise ws2_32 opengl32 ole32 winmm iphlpapi
                      dbghelp winhttp ${M2_OPENXR_WIN}/lib/openxr_loader.lib)
if(MSVC)
    # C11 atomics (voice.c); GCC's attributes are hints only (msvc_compat.h); C4244: the shared C converts int to
    # float freely (written for clang -Wall)
    target_compile_options(hotdvr PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11> $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>
                           $<$<COMPILE_LANGUAGE:C>:/FI${CMAKE_SOURCE_DIR}/tools/m2netguest/msvc_compat.h> $<$<COMPILE_LANGUAGE:C>:/wd4244>)
    # a PDB for the crash reports' frames (the code is compiled with /Z7: build_windows.py), as small as /OPT allows
    # (/PDBALTPATH: the exe names its PDB without the build machine's folders)
    target_link_options(hotdvr PRIVATE /STACK:8388608 /DEBUG /OPT:REF /OPT:ICF "/PDBALTPATH:%_PDB%")
    # the OpenXR loader only when VR is tried (xr_win.c loads it first): HOTDVR.exe starts flat without it
    target_link_options(hotdvr PRIVATE /DELAYLOAD:openxr_loader.dll)
    target_link_libraries(hotdvr PRIVATE delayimp)
endif()
