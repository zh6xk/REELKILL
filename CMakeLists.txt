# Direkonstruksi karena zip tidak berisi .sln/.vcxproj. Sesuaikan jika project Visual Studio lo berbeda.
cmake_minimum_required(VERSION 3.20)
project(REELKILL CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

set(TP "${CMAKE_SOURCE_DIR}/third_party")
file(GLOB RAPID_LIBS "${TP}/rapidocr/*.lib")
file(GLOB MPV_LIBS   "${TP}/mpv/*.lib")
file(GLOB_RECURSE WV2_LIBS "${TP}/webview2/*WebView2Loader.dll.lib")
list(FILTER WV2_LIBS INCLUDE REGEX "/x64/")
if(NOT RAPID_LIBS OR NOT MPV_LIBS OR NOT WV2_LIBS)
  message(FATAL_ERROR "Library kurang. Butuh third_party/rapidocr/*.lib, third_party/mpv/*.lib (+ include/mpv), dan WebView2Loader.dll.lib. Lihat docs/CI_SETUP.md")
endif()

add_executable(REELKILL
  AutoCutCPP.cpp
  src/core/Scanner.cpp
  src/core/Exporter.cpp
  src/bridge/WebHost.cpp)

target_include_directories(REELKILL PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${TP}/mpv/include
  ${OpenCV_INCLUDE_DIRS})
target_compile_definitions(REELKILL PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(REELKILL PRIVATE /EHsc /W3)
target_link_libraries(REELKILL PRIVATE
  ${OpenCV_LIBS} ${RAPID_LIBS} ${MPV_LIBS} ${WV2_LIBS}
  comctl32 user32 gdi32 comdlg32 shell32 ole32 oleaut32 uuid winmm dwmapi uxtheme advapi32 shlwapi version)
# Aplikasi GUI yang memakai main(): subsystem Windows, entry tetap main
set_target_properties(REELKILL PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

# Salin DLL runtime, ui/, models/, app.ico ke folder exe
file(GLOB RT_DLLS
  "${OpenCV_DIR}/x64/*/bin/opencv_world*.dll"
  "${TP}/rapidocr/*.dll" "${TP}/mpv/*.dll")
file(GLOB_RECURSE WV2_DLLS "${TP}/webview2/*WebView2Loader.dll")
list(FILTER WV2_DLLS INCLUDE REGEX "/x64/")
list(APPEND RT_DLLS ${WV2_DLLS})
add_custom_command(TARGET REELKILL POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:REELKILL>/ui"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/models" "$<TARGET_FILE_DIR:REELKILL>/models"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/app.ico" "$<TARGET_FILE_DIR:REELKILL>")
foreach(dll ${RT_DLLS})
  add_custom_command(TARGET REELKILL POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:REELKILL>")
endforeach()
