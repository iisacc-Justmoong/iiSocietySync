cmake_minimum_required(VERSION 3.24)
project(iiSocietySync VERSION 0.3.0 LANGUAGES CXX)

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
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Sql Network)
find_package(iiSocietyContainer 0.10.0 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiSocietyContainer")
find_package(iiServerHost 0.4.1 CONFIG REQUIRED HINTS "$ENV{HOME}/.local/SDK/iiServerHost")
set(CMAKE_AUTOMOC ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(sync_library_type STATIC)
else()
    set(sync_library_type SHARED)
endif()
add_library(iiSocietySync ${sync_library_type} iiSocietySync.cpp iiSocietySync.h iiSocietySyncExport.h
    Replica.cpp Replica.h ConfinedFiles.cpp ConfinedFiles.h Synchronizer.cpp Synchronizer.h
    Controller.cpp Controller.h RemoteFiles.cpp RemoteFiles.h)
add_library(iiSocietySync::iiSocietySync ALIAS iiSocietySync)
if(WIN32)
    target_sources(iiSocietySync PRIVATE ConfinedFiles_win.cpp)
    target_link_libraries(iiSocietySync PRIVATE ntdll)
endif()
target_compile_features(iiSocietySync PUBLIC cxx_std_20)
target_compile_definitions(iiSocietySync PRIVATE IISOCIETYSYNC_BUILDING_LIBRARY)
target_link_libraries(iiSocietySync PUBLIC Qt6::Core Qt6::Sql Qt6::Network iiSocietyContainer::iiSocietyContainer iiServerHost::iiServerHost)
target_include_directories(iiSocietySync PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(iiSocietySync PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE)

if(BUILD_TESTING)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    set(IISOCIETYSYNC_TEST_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" CACHE STRING "Test data directory as seen by the target runtime")
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
install(FILES iiSocietySync.h iiSocietySyncExport.h Replica.h Synchronizer.h Controller.h RemoteFiles.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocietySync)
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
