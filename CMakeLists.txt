cmake_minimum_required(VERSION 3.16)

project(sector0451 VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Поиск необходимых зависимостей Qt6
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Gui Qml Network Multimedia)

qt_standard_project_setup(REQUIRES 6.10)

# 2. Объявление исполняемого файла и перечисление базовых исходников ядра
qt_add_executable(appsector0451
    main.cpp
    src/core/audiomanager_win.h src/core/audiomanager_win.cpp  # <-- ДОБАВИЛИ СЮДА
    src/core/processmanager.h src/core/processmanager.cpp
    src/core/networkmanager.h src/core/networkmanager.cpp
    config.ini
)

# 3. Настройка QML-модуля (Сектор 0451)
# Файлы интерфейса берутся напрямую из корня проекта согласно исходной структуре.
qt_add_qml_module(appsector0451
    URI sector0451
    VERSION 1.0
    QML_FILES
        Main.qml
        Dashboard.qml
        SetupScreen.qml
    RESOURCES
        fonts/bomber.otf
        images/hex_bg.png
    SOURCES
        src/models/gamemodel.h src/models/gamemodel.cpp
        src/models/storemodel.h src/models/storemodel.cpp
        src/core/securitymanager.h
        src/core/securitymanager.cpp
        SOURCES src/core/audiomanager_win.h
        SOURCES src/core/audiomanager_win.cpp
        QML_FILES OverlayBlock.qml
        SOURCES src/core/steamcrypto.h
        QML_FILES LoadingOverlay.qml
)

# 4. Свойства сборки под целевые платформы
set_target_properties(appsector0451 PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# 5. Линковка системных библиотек и модулей Qt
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
)

# 6. Секция инсталляции сборки
install(TARGETS appsector0451
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)