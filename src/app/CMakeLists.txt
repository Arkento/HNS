# ---------------------------------------------------------------------------
# 桌面程序（阶段 1：Qt WebEngine 浏览器外壳 + 主窗口）
#
# 只有本目标依赖 Qt WebEngine / Widgets。核心库（njulms_core）刻意保持
# 与 GUI 无关，因此即使没有安装 Qt WebEngine，解析器、仓储、同步与调度
# 依然可以构建并通过全部单元测试（见 -DNJULMS_BUILD_GUI=OFF）。
# ---------------------------------------------------------------------------
add_executable(njulms_app
    main.cpp

    # 注意：这些路径相对于 src/app/（本文件所在目录），
    # 因为 src/CMakeLists.txt 用 add_subdirectory(app) 引入本文件。
    ../ui/MainWindow.cpp
    ../ui/MainWindow.h
    ../ui/UiTheme.cpp
    ../ui/UiTheme.h

    # iPad 前端：只读 HTTP 服务（只监听回环地址，对外由 tailscale serve 提供 HTTPS）
    ../web/WebApi.cpp
    ../web/WebApi.h
    ../web/WebServer.cpp
    ../web/WebServer.h

    ../browser/WebEnginePageBridge.cpp
    ../browser/WebEnginePageBridge.h
)

target_include_directories(njulms_app PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    # cpp-httplib：单头文件第三方库，只被 src/web/WebServer.cpp 引用
    ${CMAKE_SOURCE_DIR}/third_party
)

# 前端静态页面（HTML/CSS/JS）编进 exe 的 Qt 资源，分发时不需要额外目录：
#   :/web/index.html  :/web/app.js  :/web/style.css
qt_add_resources(njulms_app "web_assets"
    PREFIX "/web"
    BASE "${CMAKE_SOURCE_DIR}/resources/web"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/web/index.html"
        "${CMAKE_SOURCE_DIR}/resources/web/app.js"
        "${CMAKE_SOURCE_DIR}/resources/web/style.css"
        "${CMAKE_SOURCE_DIR}/resources/web/apple-touch-icon.png"
)

target_link_libraries(njulms_app
    PRIVATE
        njulms_core
        Qt6::Widgets
        Qt6::WebEngineWidgets
        Qt6::WebChannel
        Qt6::Network
        njulms_build_options
)

set_target_properties(njulms_app PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    # 真正的 GUI 程序：不要弹出控制台窗口。
    # 但 Debug 构建保留控制台便于看 qDebug 输出。
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
)

# ---------------------------------------------------------------------------
# 真机验证工具（可选，默认关闭）
#
# 它不是产品的一部分，而是用来回答「站点真实行为是什么」的一次性装置：
# 复用真实的 WebEnginePageBridge + NjuLmsAdapter，对已登录的持久化 Profile
# 做最小请求，回答 V1（runJavaScript+fetch 是否可用）、
# V2（会话失效的响应形态）、V3（page_size 上限）。
#
# 开启方式：-DNJULMS_BUILD_VERIFY_TOOL=ON
# ---------------------------------------------------------------------------
option(NJULMS_BUILD_VERIFY_TOOL "Build the live-site verification tool" OFF)

if(NJULMS_BUILD_VERIFY_TOOL)
    add_executable(njulms_verify
        ${CMAKE_SOURCE_DIR}/tools/verify_live.cpp
        ../browser/WebEnginePageBridge.cpp
        ../browser/WebEnginePageBridge.h
    )
    target_include_directories(njulms_verify PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(njulms_verify
        PRIVATE
            njulms_core
            Qt6::Widgets
            Qt6::WebEngineWidgets
            Qt6::WebChannel
            njulms_build_options
    )
    set_target_properties(njulms_verify PROPERTIES AUTOMOC ON)
endif()
