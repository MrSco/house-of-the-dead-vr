# No build machine's folders in the binaries (the downloads are public): the checkout's folder becomes . and the home
# folder ~ in every __FILE__ (assert messages: Opus's, SDL's, the runtime's), and with clang and GCC in the debug
# information too (the crash symbols: paths relative to the repository, as the triage's digest wants them). Included
# by CMakeLists.txt, and as CMAKE_PROJECT_INCLUDE by the other projects built for the app (build_windows.py's
# SDL3.dll). scripts/check_paths.py checks the downloads; scripts/build_info.py's prefix_map_flags is the same for the
# builds without CMake (build_quest.py). No floating-point or code-generation flag here: only file names change.
get_filename_component(M2_PM_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(WIN32 AND DEFINED ENV{USERPROFILE})
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" M2_PM_HOME)
elseif(DEFINED ENV{HOME})
    file(TO_CMAKE_PATH "$ENV{HOME}" M2_PM_HOME)
endif()
if(MSVC)
    # MSVC's __FILE__ without the folder (/d1trimfile: the first prefix that matches is cut, so the checkout's first;
    # it takes / and \ alike, and a trailing \ would escape CMake's list separator). The PDB keeps its paths (it never
    # ships: the crash symbols' private repository).
    foreach(p "${M2_PM_ROOT}" "${M2_PM_HOME}")
        if(p)
            add_compile_options("/d1trimfile:${p}/")
        endif()
    endforeach()
else()
    # (clang 17+ and GCC: the last that matches wins; older clang: the longest. The checkout's last either way.)
    if(M2_PM_HOME)
        add_compile_options("-ffile-prefix-map=${M2_PM_HOME}=~")
    endif()
    add_compile_options("-ffile-prefix-map=${M2_PM_ROOT}=.")
    # (and the debug information's compilation folder as . too: the file names are ./src/... now, which a symbolizer
    # would otherwise put after the build folder's name, ./build_houseofthedead/<platform>/hotd/./src/...)
    add_compile_options("-fdebug-compilation-dir=.")
endif()
