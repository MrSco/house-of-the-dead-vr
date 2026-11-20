# The VR app on Apple Vision Pro (docs/platforms.md, P5), built by scripts/build_visionos.py on the Mac; ANGLE (OpenGL
# ES 3 on Metal) comes from ANGLE_DIR (~/angle, with its visionOS slices). Everything but the app bundle, as static
# libraries the Xcode project (src/visionos/project.yml, made by xcodegen) links: the game (arcade_core and what it
# needs), and hotdvr_visionos: the VR shell (src/xr: every headset's, built without OpenXR, XR_OPENXR=0) on its
# Vision Pro host (src/visionos/xr_vos.m: Compositor Services, ARKit), the guns, hands and cabinet models' code
# (src/xr), the shared C (src/core), online play (src/net), voice chat and its libraries, the Apple platform
# (src/platform/apple, posix.c).
set(M2_TP ${CMAKE_SOURCE_DIR}/third_party/quest)
set(M2_OPUS ${M2_TP}/opus-1.5.2)
set(M2_SPEEX ${M2_TP}/speexdsp-1.2.1)
set(M2_RNN ${M2_TP}/rnnoise-0.2)
set(ANGLE_DIR "$ENV{HOME}/angle" CACHE PATH "ANGLE's checkout (its include folder and xcframeworks)")
if(NOT (EXISTS ${M2_OPUS}/CMakeLists.txt AND EXISTS ${M2_SPEEX}/include/speex/speexdsp_config_types.h AND EXISTS ${M2_RNN}/src/denoise.c))
    message(STATUS "hotdvr_visionos: no voice libraries in third_party/quest (scripts/apple_sync.py sends them)")
    return()
endif()

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${M2_OPUS} ${CMAKE_BINARY_DIR}/opus EXCLUDE_FROM_ALL)
target_compile_options(opus PRIVATE -w)
add_library(m2_speexdsp STATIC ${M2_SPEEX}/libspeexdsp/mdf.c ${M2_SPEEX}/libspeexdsp/fftwrap.c ${M2_SPEEX}/libspeexdsp/smallft.c)
target_compile_definitions(m2_speexdsp PRIVATE FLOATING_POINT USE_SMALLFT EXPORT=)
target_include_directories(m2_speexdsp PUBLIC ${M2_SPEEX}/include)
target_compile_options(m2_speexdsp PRIVATE -w)
add_library(m2_rnnoise STATIC ${M2_RNN}/src/denoise.c ${M2_RNN}/src/rnn.c ${M2_RNN}/src/pitch.c ${M2_RNN}/src/kiss_fft.c
            ${M2_RNN}/src/celt_lpc.c ${M2_RNN}/src/nnet.c ${M2_RNN}/src/nnet_default.c
            ${M2_RNN}/src/parse_lpcnet_weights.c ${M2_RNN}/src/rnnoise_data.c ${M2_RNN}/src/rnnoise_tables.c)
target_include_directories(m2_rnnoise PUBLIC ${M2_RNN}/include PRIVATE ${M2_RNN}/src ${M2_OPUS}/celt ${M2_OPUS}/include)
target_compile_options(m2_rnnoise PRIVATE -w)

set(M2_VOS_C
    src/xr/xr_shell.c src/visionos/xr_vos.m
    src/xr/quest_gun.c src/xr/quest_hand.c src/xr/quest_cabinet.c
    src/core/host_sdl.c src/core/voice.c src/core/voice_dsp.c src/core/m2_gl.c
    src/core/quest_menu.c src/core/quest_canvas.c src/core/quest_options.c
    src/platform/posix.c src/platform/apple/apple.m)
add_library(hotdvr_visionos STATIC ${M2_VOS_C} ${M2_NET_C})
target_include_directories(hotdvr_visionos PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/xr
                           ${CMAKE_SOURCE_DIR}/src/visionos ${M2_NET_SRC} ${M2_QUEST_SDL2} ${ANGLE_DIR}/include)
# (XR_OPENXR=0: the shell without OpenXR, its frames and input from the host: src/xr/xr.h)
target_compile_definitions(hotdvr_visionos PRIVATE HOTD_ARCADE HOTD_VOICE XR_OPENXR=0)
target_link_libraries(hotdvr_visionos PRIVATE opus m2_speexdsp m2_rnnoise)
set_source_files_properties(src/visionos/xr_vos.m src/platform/apple/apple.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# (the shared C: its own warnings, as on the Quest)
target_compile_options(hotdvr_visionos PRIVATE -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
                       -Wno-unused-function)
