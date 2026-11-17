# The flat app on Apple's systems (docs/platforms.md, P3), built by scripts/build_apple.py on the Mac; ANGLE
# (OpenGL ES 3 on Metal) comes from ANGLE_DIR (~/angle).
# - iPhone and iPad: everything but the app bundle, as static libraries the Xcode project (src/flat/ios/project.yml,
#   made by xcodegen) links: the game (arcade_core and what it needs), and hotdvr_apple: the shared C (src/core),
#   the flat shell (src/flat) with its iOS front end, online play (src/net), voice chat and its libraries, the Apple
#   platform (src/platform/apple, posix.c).
# - macOS: the same with the Mac front end, as an app (House of the Dead VR.app), ANGLE's dylibs in it.
set(M2_TP ${CMAKE_SOURCE_DIR}/third_party/quest)
set(M2_OPUS ${M2_TP}/opus-1.5.2)
set(M2_SPEEX ${M2_TP}/speexdsp-1.2.1)
set(M2_RNN ${M2_TP}/rnnoise-0.2)
set(ANGLE_DIR "$ENV{HOME}/angle" CACHE PATH "ANGLE's checkout (its include folder and xcframeworks)")
if(NOT (EXISTS ${M2_OPUS}/CMakeLists.txt AND EXISTS ${M2_SPEEX}/include/speex/speexdsp_config_types.h AND EXISTS ${M2_RNN}/src/denoise.c))
    message(STATUS "hotdvr_apple: no voice libraries in third_party/quest (scripts/apple_sync.py sends them)")
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

if(IOS)
    set(M2_FRONT src/flat/ios/main_ios.m)
else()
    set(M2_FRONT src/flat/mac/main_mac.m)
endif()
set(M2_APPLE_C
    ${M2_FRONT} src/flat/flat.c src/flat/flat_app.c src/flat/flat_menu.c src/flat/flat_touch.c
    src/core/host_sdl.c src/core/voice.c src/core/voice_dsp.c src/core/m2_gl.c
    src/core/quest_menu.c src/core/quest_canvas.c src/core/quest_options.c
    src/platform/posix.c src/platform/apple/apple.m)
add_library(hotdvr_apple STATIC ${M2_APPLE_C} ${M2_NET_C})
target_include_directories(hotdvr_apple PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/core ${M2_NET_SRC} ${M2_QUEST_SDL2}
                           ${ANGLE_DIR}/include)
target_compile_definitions(hotdvr_apple PRIVATE HOTD_ARCADE HOTD_VOICE)
target_link_libraries(hotdvr_apple PRIVATE opus m2_speexdsp m2_rnnoise)
set_source_files_properties(${M2_FRONT} src/platform/apple/apple.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# (the shared C: its own warnings, as on the Quest; ObjC from the C99 headers)
target_compile_options(hotdvr_apple PRIVATE -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)

if(NOT IOS)
    # the Mac app: the libraries, Apple's frameworks, ANGLE's dylibs copied into the bundle (their install names are
    # ./libEGL.dylib and ./libGLESv2.dylib: rewritten to @rpath, and the bundle signed again, ad hoc)
    set(M2_ANGLE_MAC ${ANGLE_DIR}/out/mac)
    add_executable(hotdvr_mac MACOSX_BUNDLE src/flat/ios/stub.c)
    set_target_properties(hotdvr_mac PROPERTIES OUTPUT_NAME "House of the Dead VR"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/src/flat/mac/Info.plist.in
        BUILD_RPATH "@executable_path/../Frameworks" INSTALL_RPATH "@executable_path/../Frameworks")
    target_link_libraries(hotdvr_mac PRIVATE -Wl,-force_load,$<TARGET_FILE:hotdvr_apple> hotdvr_apple arcade_core runtime
        opus m2_speexdsp m2_rnnoise ${M2_ANGLE_MAC}/libEGL.dylib ${M2_ANGLE_MAC}/libGLESv2.dylib
        "-framework AppKit" "-framework QuartzCore" "-framework Metal" "-framework AVFoundation" "-framework AudioToolbox"
        "-framework CoreAudio" "-framework GameController" "-framework UniformTypeIdentifiers" -lobjc)
    set(M2_APP "$<TARGET_BUNDLE_DIR:hotdvr_mac>")
    add_custom_command(TARGET hotdvr_mac POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${M2_APP}/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy ${M2_ANGLE_MAC}/libEGL.dylib ${M2_ANGLE_MAC}/libGLESv2.dylib "${M2_APP}/Contents/Frameworks/"
        COMMAND install_name_tool -id @rpath/libEGL.dylib "${M2_APP}/Contents/Frameworks/libEGL.dylib"
        COMMAND install_name_tool -id @rpath/libGLESv2.dylib "${M2_APP}/Contents/Frameworks/libGLESv2.dylib"
        COMMAND install_name_tool -change ./libGLESv2.dylib @rpath/libGLESv2.dylib "${M2_APP}/Contents/Frameworks/libEGL.dylib"
        COMMAND install_name_tool -change ./libEGL.dylib @rpath/libEGL.dylib -change ./libGLESv2.dylib @rpath/libGLESv2.dylib
                "$<TARGET_FILE:hotdvr_mac>"
        COMMAND codesign --force --deep --sign - "${M2_APP}"
        VERBATIM)
endif()
