##
# @file CMakeLists.txt
# @brief 
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS
aux_source_directory(${APP_PATH}/src APP_SRCS)
aux_source_directory(${APP_PATH}/tools APP_SRCS)
aux_source_directory(${APP_PATH}/cron_service APP_SRCS)
aux_source_directory(${APP_PATH}/heartbeat APP_SRCS)
aux_source_directory(${APP_PATH}/memory APP_SRCS)
aux_source_directory(${APP_PATH}/gateway APP_SRCS)
aux_source_directory(${APP_PATH}/skills APP_SRCS)
aux_source_directory(${APP_PATH}/agent APP_SRCS)

# APP_INC
set(APP_INC
    ${APP_PATH}
    ${APP_PATH}/include
    ${APP_PATH}/tools
    ${APP_PATH}/cron_service
    ${APP_PATH}/heartbeat
    ${APP_PATH}/memory
    ${APP_PATH}/gateway
    ${APP_PATH}/skills
    ${APP_PATH}/agent
)

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

# Snapshot before subdirectories so we can identify what the app itself adds.
set(_APP_COMP_LIBS_BEFORE "${COMPONENT_LIBS}")
set(_APP_COMP_PUBINC_BEFORE "${COMPONENT_PUBINC}")

add_subdirectory(${APP_PATH}/ai_components ${CMAKE_BINARY_DIR}/ai_components)
add_subdirectory(${APP_PATH}/IM ${CMAKE_BINARY_DIR}/IM)
add_subdirectory(${APP_PATH}/components/lua ${CMAKE_BINARY_DIR}/components/lua)

# Propagate new component libs/includes to EXAMPLE_LIB.
# ai_components uses the self-registering pattern (COMPONENT_LIBS/PUBINC +
# PARENT_SCOPE) that TuyaOpen's top-level deal_with_components/target_link_libraries
# already handles for TuyaOpen's own components but NOT for app-level subdirs
# (HEADER_DIR and COMPONENTS_ALL_LIB are computed before this app's include()).
set(_APP_NEW_LIBS "${COMPONENT_LIBS}")
list(REMOVE_ITEM _APP_NEW_LIBS ${_APP_COMP_LIBS_BEFORE})
set(_APP_NEW_PUBINC "${COMPONENT_PUBINC}")
list(REMOVE_ITEM _APP_NEW_PUBINC ${_APP_COMP_PUBINC_BEFORE})

# Now declare EXAMPLE_LIB's include paths -- APP_INC + the public includes of
# all app-component libs we just added (so app sources can include their headers).
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
        ${_APP_NEW_PUBINC}
    )

if(_APP_NEW_LIBS)
    # Give each new lib the full header set: TuyaOpen SDK + sibling app-component headers.
    set(_APP_FULL_HDIR ${HEADER_DIR} ${_APP_NEW_PUBINC})
    deal_with_components("${_APP_NEW_LIBS}" "${_APP_FULL_HDIR}")
    target_link_libraries(${EXAMPLE_LIB} ${_APP_NEW_LIBS})

    # TuyaOpen's PLATFORM_NEED_LIBS = "${EXAMPLE_LIB} tuyaos".
    # tuyaos is built from TuyaOpen's COMPONENT_LIBS snapshot taken BEFORE this
    # app's CMakeLists ran, so app-local components (ai_components/IM/lua) are
    # NOT packed into it. Fold their .o files into EXAMPLE_LIB (tuyaapp) so the
    # final platform link can resolve them.
    foreach(_lib ${_APP_NEW_LIBS})
        target_sources(${EXAMPLE_LIB} PRIVATE $<TARGET_OBJECTS:${_lib}>)
    endforeach()
endif()