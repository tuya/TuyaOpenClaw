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

########################################
# DuckyClaw ai_components overlay
########################################
# Surgically replace individual translation units inside the upstream
# TuyaOpen ai_components library targets (ai_mcp, ai_agent) with
# DuckyClaw-side patches kept under overlay/. The SDK submodule stays
# pristine; only this app's CMake graph diverges. TuyaOpen processes
# src/ai_components/ before this file is `include`d, so the upstream targets
# already exist when we reach here.
#
# Note: ai_main and ai_audio used to be overlayed too, but those diffs have
# been folded back into upstream / proven trivial — DuckyClaw now consumes
# them straight from TuyaOpen/src/ai_components/ai_{main,audio}/.
set(_DC_OVERLAY ${APP_PATH}/overlay/ai_components)

function(_dc_overlay_source target rel_path)
    if(NOT TARGET ${target})
        message(WARNING "[DC overlay] target '${target}' not defined; skip ${rel_path}")
        return()
    endif()
    set(_overlay_file ${_DC_OVERLAY}/${rel_path})
    if(NOT EXISTS ${_overlay_file})
        message(FATAL_ERROR "[DC overlay] missing overlay file: ${_overlay_file}")
    endif()
    get_target_property(_srcs ${target} SOURCES)
    get_filename_component(_basename ${rel_path} NAME)
    set(_new_srcs)
    set(_found_upstream FALSE)
    foreach(_s ${_srcs})
        get_filename_component(_n ${_s} NAME)
        if(_n STREQUAL "${_basename}")
            set(_found_upstream TRUE)
        else()
            list(APPEND _new_srcs ${_s})
        endif()
    endforeach()
    list(APPEND _new_srcs ${_overlay_file})
    set_target_properties(${target} PROPERTIES SOURCES "${_new_srcs}")
    if(_found_upstream)
        message(STATUS "[DC overlay] ${target} <- ${_overlay_file} (replaced upstream ${_basename})")
    else()
        message(WARNING "[DC overlay] ${target}: upstream ${_basename} not found in SOURCES; overlay appended anyway")
    endif()
endfunction()

# Header overlays (BEFORE PUBLIC on the lib + BEFORE PRIVATE on EXAMPLE_LIB)
# The lib-level prepend covers the lib's own .c files and anything that does
# target_link_libraries(... <lib>). But EXAMPLE_LIB (tuyaapp) gets its
# include paths from the COMPONENT_PUBINC variable that was snapshotted in
# the lib's own CMakeLists BEFORE our overlay ran, so EXAMPLE_LIB also needs
# the overlay path explicitly — otherwise app sources that compile into
# EXAMPLE_LIB (agent_loop.c, ducky_claw_chat.c, app_im.c, …) keep seeing the
# upstream headers without DuckyClaw's added declarations.
foreach(_lib_hdir ai_mcp ai_agent)
    if(TARGET ${_lib_hdir})
        target_include_directories(${_lib_hdir} BEFORE PUBLIC ${_DC_OVERLAY}/${_lib_hdir}/include)
        message(STATUS "[DC overlay] ${_lib_hdir} include path prepended: ${_DC_OVERLAY}/${_lib_hdir}/include")
    endif()
    target_include_directories(${EXAMPLE_LIB} BEFORE PRIVATE ${_DC_OVERLAY}/${_lib_hdir}/include)
    message(STATUS "[DC overlay] ${EXAMPLE_LIB} include path prepended: ${_DC_OVERLAY}/${_lib_hdir}/include")
endforeach()

_dc_overlay_source(ai_mcp   ai_mcp/src/ai_mcp_server.c)
_dc_overlay_source(ai_agent ai_agent/src/ai_agent.c)