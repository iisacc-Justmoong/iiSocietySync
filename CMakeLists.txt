cmake_minimum_required(VERSION 3.24)
project(iiSocietySync VERSION 0.8.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(DEFINED ENV{HOME})
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local/SDK/iiSocietySync" CACHE PATH "Installation prefix" FORCE)
    elseif(DEFINED ENV{USERPROFILE})
        set(CMAKE_INSTALL_PREFIX "$ENV{USERPROFILE}/.local/SDK/iiSocietySync" CACHE PATH "Installation prefix" FORCE)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Sql Network)

if(APPLE AND NOT IOS AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_society_gl WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_society_gl)
        list(FILTER _society_gl EXCLUDE REGEX "AGL")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${_society_gl}")
    endif()
endif()

option(IISOCIETYSYNC_WITH_BLE "Discover nearby peers and exchange LAN bootstrap metadata using BLE" ON)
if(IISOCIETYSYNC_WITH_BLE)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Bluetooth)
endif()
find_package(iiSocietyContainer 0.14.0 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiSocietyContainer")
find_package(iiServerHost 0.6.0 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiServerHost")
set(CMAKE_AUTOMOC ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(sync_library_type STATIC)
else()
    set(sync_library_type SHARED)
endif()
add_library(iiSocietySync ${sync_library_type} iiSocietySync.cpp iiSocietySync.h iiSocietySyncExport.h
    Replica.cpp Replica.h ConfinedFiles.cpp ConfinedFiles.h Synchronizer.cpp Synchronizer.h
    NamespaceJournal.cpp NamespaceJournal.h
    ObjectProvider.cpp ObjectProvider.h
    Controller.cpp Controller.h RemoteFiles.cpp RemoteFiles.h
    NearbyBootstrap.cpp NearbyBootstrap.h BleDiscovery.cpp BleDiscovery.h)
add_library(iiSocietySync::iiSocietySync ALIAS iiSocietySync)
if(WIN32)
    target_sources(iiSocietySync PRIVATE ConfinedFiles_win.cpp)
    target_link_libraries(iiSocietySync PRIVATE ntdll)
endif()
target_compile_features(iiSocietySync PUBLIC cxx_std_23)
if(IISOCIETYSYNC_WITH_BLE)
    target_link_libraries(iiSocietySync PRIVATE Qt6::Bluetooth)
    target_compile_definitions(iiSocietySync PRIVATE IISOCIETYSYNC_BLE)
endif()
target_compile_definitions(iiSocietySync PRIVATE IISOCIETYSYNC_BUILDING_LIBRARY)
target_link_libraries(iiSocietySync PUBLIC Qt6::Core Qt6::Gui Qt6::Sql Qt6::Network iiSocietyContainer::iiSocietyContainer iiServerHost::iiServerHost)
target_include_directories(iiSocietySync PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(iiSocietySync PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE)

if(BUILD_TESTING)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    set(IISOCIETYSYNC_TEST_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" CACHE STRING "Test data directory as seen by the target runtime")
    add_executable(iiSocietySync_indexing_tests tests/indexing.cpp)
    target_link_libraries(iiSocietySync_indexing_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
    target_compile_definitions(iiSocietySync_indexing_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
    add_test(NAME iiSocietySync.Indexing COMMAND iiSocietySync_indexing_tests)
    set_tests_properties(iiSocietySync.Indexing PROPERTIES TIMEOUT 60)
    add_executable(iiSocietySync_on_demand_tests tests/on_demand.cpp)
    target_link_libraries(iiSocietySync_on_demand_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
    target_compile_definitions(iiSocietySync_on_demand_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
    add_test(NAME iiSocietySync.OnDemand COMMAND iiSocietySync_on_demand_tests)
    set_tests_properties(iiSocietySync.OnDemand PROPERTIES TIMEOUT 90)
    add_executable(iiSocietySync_namespace_tests tests/namespace.cpp)
    target_link_libraries(iiSocietySync_namespace_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
    target_compile_definitions(iiSocietySync_namespace_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
    add_test(NAME iiSocietySync.Namespace COMMAND iiSocietySync_namespace_tests)
    set_tests_properties(iiSocietySync.Namespace PROPERTIES TIMEOUT 90)
    add_executable(iiSocietySync_bootstrap_tests tests/bootstrap.cpp)
    target_link_libraries(iiSocietySync_bootstrap_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
    add_test(NAME iiSocietySync.Bootstrap COMMAND iiSocietySync_bootstrap_tests)
    if(NOT IOS AND NOT ANDROID)
        add_executable(iiSocietySync_socket_tests tests/socket_transfer.cpp)
        target_link_libraries(iiSocietySync_socket_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
        target_compile_definitions(iiSocietySync_socket_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
        add_test(NAME iiSocietySync.SocketTransfer COMMAND iiSocietySync_socket_tests)
        set_tests_properties(iiSocietySync.SocketTransfer PROPERTIES TIMEOUT 90)
    endif()
    add_executable(iiSocietySync_files_tests tests/confined_files.cpp ConfinedFiles.cpp)
    if(WIN32)
        target_sources(iiSocietySync_files_tests PRIVATE ConfinedFiles_win.cpp)
        target_link_libraries(iiSocietySync_files_tests PRIVATE ntdll)
    endif()
    target_compile_features(iiSocietySync_files_tests PRIVATE cxx_std_20)
    target_include_directories(iiSocietySync_files_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(iiSocietySync_files_tests PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(iiSocietySync_files_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
    add_test(NAME iiSocietySync.ConfinedFiles COMMAND iiSocietySync_files_tests)
    set_tests_properties(iiSocietySync.ConfinedFiles PROPERTIES TIMEOUT 60)
    add_executable(iiSocietySync_replica_tests tests/replica.cpp)
    target_link_libraries(iiSocietySync_replica_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
    target_compile_definitions(iiSocietySync_replica_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
    add_test(NAME iiSocietySync.Replica COMMAND iiSocietySync_replica_tests)
    set_tests_properties(iiSocietySync.Replica PROPERTIES TIMEOUT 60)
    add_executable(iiSocietySync_sync_tests tests/synchronizer.cpp)
    target_link_libraries(iiSocietySync_sync_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
    target_compile_definitions(iiSocietySync_sync_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
    add_test(NAME iiSocietySync.Synchronizer COMMAND iiSocietySync_sync_tests)
    set_tests_properties(iiSocietySync.Synchronizer PROPERTIES TIMEOUT 90)
    add_executable(iiSocietySync_controller_tests tests/controller.cpp)
    target_link_libraries(iiSocietySync_controller_tests PRIVATE iiSocietySync::iiSocietySync Qt6::Test)
    target_compile_definitions(iiSocietySync_controller_tests PRIVATE SYNC_TEST_DIRECTORY="${IISOCIETYSYNC_TEST_DIRECTORY}")
    add_test(NAME iiSocietySync.Controller COMMAND iiSocietySync_controller_tests)
    set_tests_properties(iiSocietySync.Controller PROPERTIES TIMEOUT 60)
    add_executable(iiSocietySync_hello_world tests/hello_world.cpp)
    target_link_libraries(iiSocietySync_hello_world PRIVATE iiSocietySync::iiSocietySync)
    set_target_properties(iiSocietySync_hello_world PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietySync.hello_world COMMAND iiSocietySync_hello_world)
endif()

install(TARGETS iiSocietySync EXPORT iiSocietySyncTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES iiSocietySync.h iiSocietySyncExport.h Replica.h Synchronizer.h Controller.h RemoteFiles.h
    NearbyBootstrap.h BleDiscovery.h ObjectProvider.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md NearbySync.md Namespace.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietySync)
set(iiSocietySync_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/iiSocietySync")
configure_package_config_file(cmake/iiSocietySyncConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietySyncConfig.cmake"
    INSTALL_DESTINATION "${iiSocietySync_PACKAGE_DIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietySyncConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT iiSocietySyncTargets
    NAMESPACE iiSocietySync::
    DESTINATION "${iiSocietySync_PACKAGE_DIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietySyncConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocietySyncConfigVersion.cmake"
    DESTINATION "${iiSocietySync_PACKAGE_DIR}")
