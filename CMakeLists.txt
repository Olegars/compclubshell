cmake_minimum_required(VERSION 3.16)

project(sector0451 VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Ищем все нужные пакеты за один раз (ДОБАВЛЕН QuickControls2 для работы стилей)
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Gui Qml Network Multimedia)

qt_standard_project_setup(REQUIRES 6.10)

# 2. Перечисляем все C++ исходники здесь + config.ini для отображения в IDE
qt_add_executable(appsector0451
    main.cpp
    src/models/gamemodel.h src/models/gamemodel.cpp
    src/core/processmanager.h src/core/processmanager.cpp
    src/core/networkmanager.h src/core/networkmanager.cpp
    config.ini
)

# 3. Настраиваем QML-модуль
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
        src/models/storemodel.h src/models/storemodel.cpp
)

# 4. Системные свойства (Windows/macOS)
set_target_properties(appsector0451 PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# 5. Линковка библиотек (ДОБАВЛЕН Qt6::QuickControls2)
target_link_libraries(appsector0451
    PRIVATE
        Qt6::Core
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Gui
        Qt6::Qml
        Qt6::Network
        Qt6::Multimedia
        iphlpapi
        ws2_32
)

# 6. Инсталляция (опционально)
install(TARGETS appsector0451
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)