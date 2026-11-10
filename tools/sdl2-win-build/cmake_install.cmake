# Install script for directory: C:/coding/nexus-recomp/tools/SDL2-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/coding/nexus-recomp/tools/sdl2-win")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "CMAKE_OBJDUMP-NOTFOUND")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/libSDL2main.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/libSDL2.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2mainTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2mainTargets.cmake"
         "C:/coding/nexus-recomp/tools/sdl2-win-build/CMakeFiles/Export/f084604df1a27ef5b4fef7c7544737d1/SDL2mainTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2mainTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2mainTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2" TYPE FILE FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/CMakeFiles/Export/f084604df1a27ef5b4fef7c7544737d1/SDL2mainTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2" TYPE FILE FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/CMakeFiles/Export/f084604df1a27ef5b4fef7c7544737d1/SDL2mainTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2staticTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2staticTargets.cmake"
         "C:/coding/nexus-recomp/tools/sdl2-win-build/CMakeFiles/Export/f084604df1a27ef5b4fef7c7544737d1/SDL2staticTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2staticTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2/SDL2staticTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2" TYPE FILE FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/CMakeFiles/Export/f084604df1a27ef5b4fef7c7544737d1/SDL2staticTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2" TYPE FILE FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/CMakeFiles/Export/f084604df1a27ef5b4fef7c7544737d1/SDL2staticTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/SDL2" TYPE FILE FILES
    "C:/coding/nexus-recomp/tools/sdl2-win-build/SDL2Config.cmake"
    "C:/coding/nexus-recomp/tools/sdl2-win-build/SDL2ConfigVersion.cmake"
    "C:/coding/nexus-recomp/tools/SDL2-src/cmake/sdlfind.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/SDL2" TYPE FILE FILES
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_assert.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_atomic.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_audio.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_bits.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_blendmode.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_clipboard.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_copying.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_cpuinfo.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_egl.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_endian.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_error.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_events.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_filesystem.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_gamecontroller.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_gesture.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_guid.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_haptic.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_hidapi.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_hints.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_joystick.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_keyboard.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_keycode.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_loadso.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_locale.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_log.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_main.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_messagebox.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_metal.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_misc.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_mouse.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_mutex.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_name.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengl.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengl_glext.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengles.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengles2.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengles2_gl2.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengles2_gl2ext.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengles2_gl2platform.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_opengles2_khrplatform.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_pixels.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_platform.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_power.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_quit.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_rect.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_render.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_rwops.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_scancode.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_sensor.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_shape.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_stdinc.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_surface.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_system.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_syswm.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_assert.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_common.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_compare.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_crc32.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_font.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_fuzzer.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_harness.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_images.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_log.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_md5.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_memory.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_test_random.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_thread.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_timer.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_touch.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_types.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_version.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_video.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/SDL_vulkan.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/begin_code.h"
    "C:/coding/nexus-recomp/tools/SDL2-src/include/close_code.h"
    "C:/coding/nexus-recomp/tools/sdl2-win-build/include/SDL2/SDL_revision.h"
    "C:/coding/nexus-recomp/tools/sdl2-win-build/include-config-release/SDL2/SDL_config.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/licenses/SDL2" TYPE FILE FILES "C:/coding/nexus-recomp/tools/SDL2-src/LICENSE.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/sdl2.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE PROGRAM FILES "C:/coding/nexus-recomp/tools/sdl2-win-build/sdl2-config")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/aclocal" TYPE FILE FILES "C:/coding/nexus-recomp/tools/SDL2-src/sdl2.m4")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/coding/nexus-recomp/tools/sdl2-win-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/coding/nexus-recomp/tools/sdl2-win-build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
