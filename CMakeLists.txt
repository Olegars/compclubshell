cmake_minimum_required(VERSION 3.16)

project(sector0451 VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Gui Qml Network Multimedia WebView)

qt_standard_project_setup(REQUIRES 6.10)

qt_add_executable(appsector0451
    main.cpp
    src/core/hwidprovider.h src/core/hwidprovider.cpp
    src/core/audiomanager_win.h src/core/audiomanager_win.cpp
    src/core/processmanager.h src/core/processmanager.cpp
    src/core/iplatformauth.h
    src/core/steamauth.h src/core/steamauth.cpp
    src/core/epicauth.h src/core/epicauth.cpp
    src/core/eaauth.h src/core/eaauth.cpp
    src/core/riotauth.h src/core/riotauth.cpp
    src/core/directlaunchauth.h src/core/directlaunchauth.cpp
    src/core/networkmanager.h src/core/networkmanager.cpp
    src/core/thermalmonitor.h src/core/thermalmonitor.cpp
    src/core/fanrelaycontroller.h src/core/fanrelaycontroller.cpp
    src/core/sessionalertmanager.h src/core/sessionalertmanager.cpp
    src/core/hidinputmonitor.h src/core/hidinputmonitor.cpp
    src/core/voicehotkeymonitor.h src/core/voicehotkeymonitor.cpp
    src/core/voiceassistant.h src/core/voiceassistant.cpp
    src/core/lobbyaudiomanager.h src/core/lobbyaudiomanager.cpp
    src/core/ccbootsuperclient.h src/core/ccbootsuperclient.cpp
    src/core/pathresolver.h src/core/pathresolver.cpp
    config.ini
)

set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(appsector0451
    URI sector0451
    VERSION 1.0
    QML_FILES
        Theme.qml
        Main.qml
        Dashboard.qml
        SetupScreen.qml
        OverlayBlock.qml
        LoadingOverlay.qml
        QuickMenu.qml
        VoiceIndicator.qml
        AvatarWatermarkBg.qml
    RESOURCES
        images/hex_bg.png
        images/avatars/avatar_1.png
        images/avatars/avatar_2.png
        images/avatars/avatar_3.png
        images/avatars/avatar_4.png
        images/avatars/avatar_5.png
        images/avatars/avatar_6.png
        images/avatars/avatar_7.png
        images/avatars/avatar_8.png
        images/avatars/avatar_9.png
        images/avatars/avatar_10.png
        fonts/bomber.otf
        images/launchers/steam.png
        images/launchers/epic.png
        images/launchers/ea.png
        images/launchers/riot.png
        images/launchers/battlenet.png
        images/launchers/ubisoft.png
        images/launchers/lesta.png
        images/launchers/vkplay.png
        images/launchers/roblox.png
    SOURCES
        src/models/gamemodel.h src/models/gamemodel.cpp
        src/models/storemodel.h src/models/storemodel.cpp
        src/core/securitymanager.h src/core/securitymanager.cpp
        src/core/steamcrypto.h
)

set_target_properties(appsector0451 PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appsector0451
    PRIVATE
        Qt6::Core
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Gui
        Qt6::Qml
        Qt6::Network
        Qt6::Multimedia
        Qt6::WebView
        crypt32
        iphlpapi
        ws2_32
        ole32
        oleaut32
        wbemuuid
        winmm
        user32
        uiautomationcore
        sapi
        setupapi
)

install(TARGETS appsector0451
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# WebView2 runtime deps (MinGW build of qtwebview plugin)
if(WIN32)
    set(_webview2_loader "$ENV{MSYSTEM_PREFIX}/bin/WebView2Loader.dll")
    if(NOT EXISTS "${_webview2_loader}")
        set(_webview2_loader "C:/msys64/mingw64/bin/WebView2Loader.dll")
    endif()
    if(EXISTS "${_webview2_loader}")
        add_custom_command(TARGET appsector0451 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_webview2_loader}"
                $<TARGET_FILE_DIR:appsector0451>/WebView2Loader.dll
            COMMENT "Copy WebView2Loader.dll next to shell"
        )
    endif()
endif()
