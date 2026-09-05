cmake_minimum_required(VERSION 3.24)
project(iiSocietySync VERSION 0.1.0 LANGUAGES CXX)

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
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core)

add_library(iiSocietySync SHARED iiSocietySync.cpp iiSocietySync.h)
add_library(iiSocietySync::iiSocietySync ALIAS iiSocietySync)
target_compile_features(iiSocietySync PUBLIC cxx_std_20)
target_compile_definitions(iiSocietySync PRIVATE IISOCIETYSYNC_BUILDING_LIBRARY)
target_link_libraries(iiSocietySync PUBLIC Qt6::Core)
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
    add_executable(iiSocietySync_hello_world tests/hello_world.cpp)
    target_link_libraries(iiSocietySync_hello_world PRIVATE iiSocietySync::iiSocietySync)
    set_target_properties(iiSocietySync_hello_world PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiSocietySync.hello_world COMMAND iiSocietySync_hello_world)
endif()

install(TARGETS iiSocietySync EXPORT iiSocietySyncTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES iiSocietySync.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
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
