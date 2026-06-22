##
# @file CMakeLists.txt
# @brief
#/
if (CONFIG_ENABLE_COMP_AI_DISPLAY STREQUAL "y")

set(MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(MODULE_NAME ${MODULE_PATH} NAME)

set(LIB_SRCS)

# Common sources (non-recursive, top-level only)
aux_source_directory(${MODULE_PATH}/src LIB_SRCS)

# UI variant sources based on Kconfig selection.
# Fallback to wechat when the Kconfig choice does not resolve (it does not
# reliably propagate through DuckyClaw's app-local Kconfig include path) --
# matches the #else fallback in ai_main/src/ai_chat_ui.c.
if (CONFIG_ENABLE_AI_CHAT_GUI_CHATBOT STREQUAL "y")
    file(GLOB CHATBOT_SRCS ${MODULE_PATH}/src/chatbot/*.c)
    list(APPEND LIB_SRCS ${CHATBOT_SRCS})
elseif (CONFIG_ENABLE_AI_CHAT_GUI_OLED STREQUAL "y")
    file(GLOB OLED_SRCS ${MODULE_PATH}/src/oled/*.c)
    list(APPEND LIB_SRCS ${OLED_SRCS})
else()
    file(GLOB WECHAT_SRCS ${MODULE_PATH}/src/wechat/*.c)
    list(APPEND LIB_SRCS ${WECHAT_SRCS})
endif()

# Font sources
aux_source_directory(${MODULE_PATH}/font FONT_SRCS)
aux_source_directory(${MODULE_PATH}/font/emoji EMOJI_SRCS)

# Icon sources: select version-specific subdirectory
if (CONFIG_LVGL_VERSION_9 STREQUAL "y")
    aux_source_directory(${MODULE_PATH}/icon/v9 ICON_SRCS)
else()
    aux_source_directory(${MODULE_PATH}/icon/v8 ICON_SRCS)
endif()

list(APPEND LIB_SRCS
    ${FONT_SRCS}
    ${EMOJI_SRCS}
    ${ICON_SRCS}
)

# Include paths
set(LIB_PUBLIC_INC
    ${MODULE_PATH}/include
    ${MODULE_PATH}/font
)

if (CONFIG_ENABLE_AI_CHAT_GUI_CHATBOT STREQUAL "y")
    # no extra public include
elseif (CONFIG_ENABLE_AI_CHAT_GUI_OLED STREQUAL "y")
    # no extra public include
else()
    # wechat fallback -- expose its private headers as public so register hooks find them
    list(APPEND LIB_PUBLIC_INC ${MODULE_PATH}/src/wechat)
    # And force-define the C-side gate that wechat sources use, since the
    # Kconfig choice does not always resolve through the app-local Kconfig path.
    set(_AI_UI_FORCE_WECHAT_DEFINE TRUE)
endif()

########################################
# Target Configure
########################################
add_library(${MODULE_NAME})
target_sources(${MODULE_NAME} PRIVATE ${LIB_SRCS})
target_include_directories(${MODULE_NAME} PUBLIC ${LIB_PUBLIC_INC})
target_compile_options(${MODULE_NAME} PRIVATE "-DLV_LVGL_H_INCLUDE_SIMPLE")
if (_AI_UI_FORCE_WECHAT_DEFINE)
    target_compile_definitions(${MODULE_NAME} PRIVATE ENABLE_AI_CHAT_GUI_WECHAT=1)
endif()

list(APPEND COMPONENT_LIBS ${MODULE_NAME})
set(COMPONENT_LIBS "${COMPONENT_LIBS}" PARENT_SCOPE)
list(APPEND COMPONENT_PUBINC ${LIB_PUBLIC_INC})
set(COMPONENT_PUBINC "${COMPONENT_PUBINC}" PARENT_SCOPE)

endif()
