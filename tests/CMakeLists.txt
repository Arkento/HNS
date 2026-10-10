# ---------------------------------------------------------------------------
# 测试
#
# 全部测试只依赖 njulms_core（Qt Core + Qt Sql），**不需要 Qt WebEngine**。
# 这正是把网站专属逻辑与浏览器会话解耦之后的直接收益：
# 解析器、仓储、断点恢复、会话判定都可以在没有网络、没有真实账号的
# 条件下反复验证。
# ---------------------------------------------------------------------------

# 共享的测试支撑代码
add_library(njulms_test_support INTERFACE)
target_include_directories(njulms_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(njulms_test_support INTERFACE njulms_core)

# 辅助函数：注册一个基于 Qt Test 的测试可执行文件
# 额外参数（可选）：该测试专属的支撑源文件（例如含多行 HTML 样本的夹具 .cpp，
# 它们必须放在不被 moc 处理的 .cpp 里，见 docs/01-实现决策.md）
function(njulms_add_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name}
        PRIVATE
            njulms_test_support
            Qt6::Test
            njulms_build_options
    )
    set_target_properties(${name} PROPERTIES
        AUTOMOC ON
        AUTOUIC ON
    )
    add_test(NAME ${name} COMMAND ${name})
    # 测试使用临时目录，不污染源码树
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
endfunction()

njulms_add_test(AssignmentParserTests)
njulms_add_test(ChaoxingParserTests support/ChaoxingFixtures.cpp)
njulms_add_test(AuthManagerTests)
njulms_add_test(PaginationTests)
njulms_add_test(RepositoryTests)
njulms_add_test(ResumeTests)
njulms_add_test(SessionConsistencyTests)
njulms_add_test(SyncManagerTests)
njulms_add_test(SchedulerTests)
njulms_add_test(TimeUtilsTests)
njulms_add_test(RedactTests)
njulms_add_test(SingleInstanceLockTests)

# ---------------------------------------------------------------------------
# iPad 前端（只读 HTTP 服务）
#
# 只依赖 Qt Core / Network / Test（不需要 WebEngine）：用假数据源起一个真实的
# HTTP 服务，验证路由、JSON 形状，以及「所有回调都在主线程执行」这条线程约束。
# ---------------------------------------------------------------------------
add_executable(WebServerTests
    WebServerTests.cpp
    ${CMAKE_SOURCE_DIR}/src/web/WebApi.cpp
    ${CMAKE_SOURCE_DIR}/src/web/WebApi.h
    ${CMAKE_SOURCE_DIR}/src/web/WebServer.cpp
    ${CMAKE_SOURCE_DIR}/src/web/WebServer.h
)
target_include_directories(WebServerTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(WebServerTests
    PRIVATE
        njulms_test_support
        Qt6::Test
        Qt6::Network
        njulms_build_options
)
# 静态页面同样编进测试目标：这样「资源前缀写错 / 页面取不到」会在测试里暴露
qt_add_resources(WebServerTests "web_assets"
    PREFIX "/web"
    BASE "${CMAKE_SOURCE_DIR}/resources/web"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/web/index.html"
        "${CMAKE_SOURCE_DIR}/resources/web/app.js"
        "${CMAKE_SOURCE_DIR}/resources/web/style.css"
        "${CMAKE_SOURCE_DIR}/resources/web/apple-touch-icon.png"
        "${CMAKE_SOURCE_DIR}/resources/web/service-worker.js"
)
set_target_properties(WebServerTests PROPERTIES AUTOMOC ON)
add_test(NAME WebServerTests COMMAND WebServerTests)
set_tests_properties(WebServerTests PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# GUI 冒烟测试
#
# 唯一需要 Qt Widgets / WebEngine 的测试：它构造真实的 MainWindow，
# 因此只在 GUI 打开时才有意义。用来覆盖此前完全没有测试的 UI 层
# （建界面 → 接信号 → 开数据库 → 建 Profile → 从数据库填充表格）。
#
# 注意：它需要离屏平台（QT_QPA_PLATFORM=offscreen），下面已设置。
# ---------------------------------------------------------------------------
if(NJULMS_BUILD_GUI)
    add_executable(MainWindowSmokeTests
        MainWindowSmokeTests.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/MainWindow.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/MainWindow.h
        ${CMAKE_SOURCE_DIR}/src/ui/UiTheme.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/UiTheme.h
        ${CMAKE_SOURCE_DIR}/src/web/WebApi.cpp
        ${CMAKE_SOURCE_DIR}/src/web/WebApi.h
        ${CMAKE_SOURCE_DIR}/src/web/WebServer.cpp
        ${CMAKE_SOURCE_DIR}/src/web/WebServer.h
        ${CMAKE_SOURCE_DIR}/src/browser/WebEnginePageBridge.cpp
        ${CMAKE_SOURCE_DIR}/src/browser/WebEnginePageBridge.h
        ${CMAKE_SOURCE_DIR}/src/adapters/ChaoxingAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/adapters/ChaoxingAdapter.h
        ${CMAKE_SOURCE_DIR}/src/sync/ChaoxingSyncRunner.cpp
        ${CMAKE_SOURCE_DIR}/src/sync/ChaoxingSyncRunner.h
    )
    target_include_directories(MainWindowSmokeTests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
    )
    target_link_libraries(MainWindowSmokeTests
        PRIVATE
            njulms_test_support
            Qt6::Test
            Qt6::Widgets
            Qt6::WebEngineWidgets
            Qt6::WebChannel
            Qt6::Network
            njulms_build_options
    )
    set_target_properties(MainWindowSmokeTests PROPERTIES AUTOMOC ON)
    add_test(NAME MainWindowSmokeTests COMMAND MainWindowSmokeTests)
    set_tests_properties(MainWindowSmokeTests PROPERTIES
        # NJULMS_DISABLE_AUTO_OPEN：跳过「启动自动打开站点」，让 UI 测试保持离线、确定性
        # NJULMS_WEB_PORT=0：测试不起前端 HTTP 服务（不占用端口、不受其它实例影响）
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;NJULMS_DISABLE_AUTO_OPEN=1;NJULMS_WEB_PORT=0"
        TIMEOUT 180
    )

    # ------------------------------------------------------------------
    # 把 offscreen 平台插件部署到测试可执行文件旁边（实测必需）
    #
    # GUI 冒烟测试用 QT_QPA_PLATFORM=offscreen 运行。测试可执行文件位于
    # build/bin，旁边已有 windeployqt 生成的 platforms/（只含 qwindows）。
    # 实测：当 platforms/ 存在但缺少 offscreen 插件时，Qt 的平台插件查找
    # 会卡住（进程停在 QApplication 构造，180s 超时）；把插件就地放好则
    # 立即恢复正常（同一台机器、同一批二进制反复验证）。
    # 因此这里显式随测试部署一份，不再依赖插件搜索路径的回退行为。
    # ------------------------------------------------------------------
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        set(njulms_offscreen_plugin "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>")
    else()
        file(GLOB njulms_offscreen_candidates
             "${Qt6_DIR}/../../../plugins/platforms/qoffscreen*.dll")
        list(GET njulms_offscreen_candidates 0 njulms_offscreen_plugin)
    endif()

    add_custom_command(TARGET MainWindowSmokeTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${njulms_offscreen_plugin}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms"
        COMMENT "Deploying the offscreen platform plugin for GUI tests"
        VERBATIM
    )
endif()
