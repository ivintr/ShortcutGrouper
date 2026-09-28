cmake_minimum_required(VERSION 3.20)
project(Win11ShortcutGrouper LANGUAGES CXX)

# Версия пакета: единый источник для MSIX (build-sparse.ps1 -Version).
# Бампить при каждом релизе, иначе апгрейды пакета не детектятся.
set(SHORTCUT_GROUPER_VERSION "1.0.1.0" CACHE STRING "MSIX package version N.N.N.N")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Межпроцедурная оптимизация в Release (меньше бинарь, быстрее код)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

# Исходники в UTF-8 (русские строковые литералы): без этого MSVC читает
# файлы в системной ANSI-кодировке и L"..." превращается в кракозябры.
if (MSVC)
    add_compile_options(/utf-8)
endif()

# nlohmann/json (header-only, bundled)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/vendor/include)

# --- Common (header-only + compiled helpers, shared) ---
add_library(Common STATIC
    src/Common/Lang.cpp
    src/Common/DpiHelper.cpp
    src/Common/ColorUtils.cpp
    src/Common/Logger.cpp
)
target_include_directories(Common PUBLIC src/Common)
target_compile_definitions(Common PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(Common PUBLIC shlwapi)

# --- ShellExtension.dll ---
add_library(ShellExtension SHARED
    src/ShellExtension/dllmain.cpp
    src/ShellExtension/GroupCommand.cpp
    src/ShellExtension/Registration.cpp
    src/ShellExtension/DesktopOpenWithFilter.cpp
    src/ShellExtension/LogHelper.cpp
)
target_include_directories(ShellExtension PRIVATE src/ShellExtension src/Common)
target_compile_definitions(ShellExtension PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(ShellExtension PRIVATE shlwapi ole32 uuid Common)
target_link_options(ShellExtension PRIVATE /DEF:${CMAKE_SOURCE_DIR}/src/ShellExtension/ShellExtension.def)

set_target_properties(ShellExtension PROPERTIES
    OUTPUT_NAME "ShellExtension"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# --- GroupManager.exe ---
add_executable(GroupManager WIN32
    src/GroupManager/main.cpp
    src/GroupManager/BlurHelper.cpp
    src/GroupManager/IconHelper.cpp
    src/GroupManager/Renderer.cpp
    src/GroupManager/ModernMenu.cpp
    src/GroupManager/DesktopWidget.cpp
    src/GroupManager/PopupWindow.cpp
    src/GroupManager/Marquee.cpp
    src/GroupManager/DesktopGrid.cpp
    src/GroupManager/WidgetManager.cpp
    src/GroupManager/TrayIcon.cpp
    src/GroupManager/UninstallHelper.cpp
    src/GroupManager/SettingsDialog.cpp
    src/GroupManager/SearchWindow.cpp
    src/GroupManager/ColorDialog.cpp
    src/GroupManager/OpenWithDetector.cpp
    resources/app.rc
)
target_include_directories(GroupManager PRIVATE src/GroupManager src/Common)
target_compile_definitions(GroupManager PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(GroupManager PRIVATE
    d2d1 dwrite dwmapi windowscodecs
    shlwapi ole32 oleaut32 oleacc shell32 comctl32 msimg32
    nlohmann_json Common
)
set_target_properties(GroupManager PROPERTIES
    OUTPUT_NAME "GroupManager"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Копия exe в корень проекта (только exe, без pdb): удобно запускать/проверять.
add_custom_command(TARGET GroupManager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GroupManager>
        ${CMAKE_SOURCE_DIR}/GroupManager.exe
    COMMENT "Copying GroupManager.exe to project root")

# install()-правила: один источник правды для раскладки вместо ручного
# копирования bin\Release скриптами (дрейф версий exe/dll/msix).
install(TARGETS ShellExtension GroupManager
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION lib)
install(FILES "${CMAKE_SOURCE_DIR}/src/AppxManifest.xml" DESTINATION msix)
