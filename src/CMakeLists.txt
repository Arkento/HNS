# ---------------------------------------------------------------------------
# njulms_core —— 与 Qt WebEngine / GUI 完全无关的核心库
#
# 只依赖 Qt Core + Qt Sql。这样即使 Qt WebEngine 组件尚未安装，
# 解析器、仓储、同步与调度模块仍然可以独立构建、运行与单元测试。
# 这是「网站专属逻辑与数据库/调度解耦」在构建层面的体现。
# ---------------------------------------------------------------------------
add_library(njulms_core STATIC
    models/Assignment.cpp
    models/Assignment.h
    models/Course.h
    models/FieldState.h

    parser/AssignmentParser.cpp
    parser/AssignmentParser.h
    parser/ChaoxingParser.cpp
    parser/ChaoxingParser.h
    parser/YuketangParser.cpp
    parser/YuketangParser.h

    repository/SqliteRepository.cpp
    repository/SqliteRepository.h

    sync/SyncManager.cpp
    sync/SyncManager.h
    sync/YuketangSyncRunner.cpp
    sync/YuketangSyncRunner.h

    scheduler/Scheduler.cpp
    scheduler/Scheduler.h

    auth/AuthManager.cpp
    auth/AuthManager.h

    adapters/IPageBridge.h
    adapters/NjuLmsAdapter.cpp
    adapters/NjuLmsAdapter.h
    adapters/YuketangAdapter.cpp
    adapters/YuketangAdapter.h

    utils/Logger.cpp
    utils/Logger.h
    utils/Redact.cpp
    utils/Redact.h
    utils/TimeUtils.cpp
    utils/TimeUtils.h
    utils/SingleInstanceLock.cpp
    utils/SingleInstanceLock.h
    utils/ChromiumLogging.cpp
    utils/ChromiumLogging.h
)

target_include_directories(njulms_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(njulms_core
    PUBLIC
        Qt6::Core
        Qt6::Sql
    PRIVATE
        njulms_build_options
)

set_target_properties(njulms_core PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# ---------------------------------------------------------------------------
# 主程序与 GUI（阶段 1，需要 Qt WebEngine）
# ---------------------------------------------------------------------------
if(NJULMS_BUILD_GUI)
    add_subdirectory(app)
endif()
