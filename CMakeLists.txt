# ==============================================================================
#  AniDesk-Qt —— AI 动态桌面宠物
#  第一步原型：Qt 动态渲染引擎 + 动画状态机
#
#  构建方式（若 Qt6 不在系统默认路径，请指定 CMAKE_PREFIX_PATH）：
#    cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.7.3/mingw_64"
#    cmake --build build --config Release
#
#  运行：
#    ./build/AniDesk-Qt.exe   （Windows / MinGW 需保证 Qt 的 bin 目录在 PATH 中）
# ==============================================================================
cmake_minimum_required(VERSION 3.16)

project(AniDesk-Qt VERSION 0.1.0 LANGUAGES CXX)

# ---- C++17 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Qt 自动化工具（moc / uic / rcc）----
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# 内置宠物形象素材：assets/pets.qrc 存在时才参与编译。
# 若没有该资源，程序会自动回退到 PetWidget 内置的程序化绘制形象，依然可以运行。
set(ANIDESK_RESOURCES "")
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/pets.qrc")
    list(APPEND ANIDESK_RESOURCES assets/pets.qrc)
endif()

qt_add_executable(AniDesk-Qt
    WIN32
    MACOSX_BUNDLE
    src/main.cpp
    src/PetWidget.h  src/PetWidget.cpp
    src/ChatBubble.h src/ChatBubble.cpp
    # 第二步：AI 资产生成管线
    src/AssetPipelineManager.h  src/AssetPipelineManager.cpp
    src/BackgroundRemover.h     src/BackgroundRemover.cpp
    src/LivePortraitClient.h    src/LivePortraitClient.cpp
    src/PersonaExtractor.h      src/PersonaExtractor.cpp
    # 第三步：LLM 流式对话与情绪驱动
    src/LLMService.h   src/LLMService.cpp
    src/EmotionParser.h src/EmotionParser.cpp
    ${ANIDESK_RESOURCES}
)

target_link_libraries(AniDesk-Qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

# MSVC 下强制按 UTF-8 解析源码，避免中文字符串乱码（MinGW 默认即 UTF-8）
if (MSVC)
    target_compile_options(AniDesk-Qt PRIVATE /utf-8)
endif()

set_target_properties(AniDesk-Qt PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.anidesk.qt"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

install(TARGETS AniDesk-Qt
    BUNDLE  DESTINATION .
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# ---- 管线自检工具（可选，不参与主程序构建）----
# 验证"拖入静态图 → 降级 → 打包 PetAssetPackage"的完整链路：
#   cmake --build build --target anidesk-pipeline-test
#   ./build/anidesk-pipeline-test.exe
add_executable(anidesk-pipeline-test EXCLUDE_FROM_ALL
    tools/pipeline_test.cpp
    src/AssetPipelineManager.h  src/AssetPipelineManager.cpp
    src/BackgroundRemover.h     src/BackgroundRemover.cpp
    src/LivePortraitClient.h    src/LivePortraitClient.cpp
    src/PersonaExtractor.h      src/PersonaExtractor.cpp
)
target_link_libraries(anidesk-pipeline-test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)
target_include_directories(anidesk-pipeline-test PRIVATE src)
if (MSVC)
    target_compile_options(anidesk-pipeline-test PRIVATE /utf-8)
endif()
