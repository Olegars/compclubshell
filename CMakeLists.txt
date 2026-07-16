cmake_minimum_required(VERSION 3.16)

project(sector0451 VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Gui Qml Network Multimedia)

qt_standard_project_setup(REQUIRES 6.10)

qt_add_executable(appsector0451
    main.cpp
    src/core/hwidprovider.h src/core/hwidprovider.cpp
    src/core/audiomanager_win.h src/core/audiomanager_win.cpp
    src/core/processmanager.h src/core/processmanager.cpp
    src/core/iplatformauth.h
    src/core/steamauth.h src/core/steamauth.cpp
    src/core/epicauth.h src/core/epicauth.cpp
    src/core/directlaunchauth.h src/core/directlaunchauth.cpp
    src/core/networkmanager.h src/core/networkmanager.cpp
    config.ini
)

qt_add_qml_module(appsector0451
    URI sector0451
    VERSION 1.0
    QML_FILES
        Main.qml
        Dashboard.qml
        SetupScreen.qml
        OverlayBlock.qml
        LoadingOverlay.qml
    RESOURCES
        images/hex_bg.png
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
        crypt32
        iphlpapi
        ws2_32
        ole32
        oleaut32
        uiautomationcore
)

install(TARGETS appsector0451
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
