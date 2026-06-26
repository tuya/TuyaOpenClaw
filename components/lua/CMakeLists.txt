##
# @file CMakeLists.txt
# @brief Embedded Lua 5.5 component for TuyaOpenClaw
#
# Vendored from upstream Lua 5.5 (originally via the esp-idf-component-lua
# managed component). Sources liolib.c / loslib.c / loadlib.c / linit.c /
# lua.c / onelua.c / ltests.c are intentionally excluded — the embedded
# tool only exposes a sandboxed subset of the standard library through
# port/linit_sandbox.c.
#/

if (CONFIG_ENABLE_LUA STREQUAL "y")

set(MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})

# ----------------------------------------------------------------------------
# Lua core sources
# ----------------------------------------------------------------------------
set(LUA_CORE_SRCS
    ${MODULE_PATH}/lua/lapi.c
    ${MODULE_PATH}/lua/lauxlib.c
    ${MODULE_PATH}/lua/lbaselib.c
    ${MODULE_PATH}/lua/lcode.c
    ${MODULE_PATH}/lua/lcorolib.c
    ${MODULE_PATH}/lua/lctype.c
    ${MODULE_PATH}/lua/ldblib.c
    ${MODULE_PATH}/lua/ldebug.c
    ${MODULE_PATH}/lua/ldo.c
    ${MODULE_PATH}/lua/ldump.c
    ${MODULE_PATH}/lua/lfunc.c
    ${MODULE_PATH}/lua/lgc.c
    ${MODULE_PATH}/lua/llex.c
    ${MODULE_PATH}/lua/lmathlib.c
    ${MODULE_PATH}/lua/lmem.c
    ${MODULE_PATH}/lua/lobject.c
    ${MODULE_PATH}/lua/lopcodes.c
    ${MODULE_PATH}/lua/lparser.c
    ${MODULE_PATH}/lua/lstate.c
    ${MODULE_PATH}/lua/lstring.c
    ${MODULE_PATH}/lua/lstrlib.c
    ${MODULE_PATH}/lua/ltable.c
    ${MODULE_PATH}/lua/ltablib.c
    ${MODULE_PATH}/lua/ltm.c
    ${MODULE_PATH}/lua/lundump.c
    ${MODULE_PATH}/lua/lutf8lib.c
    ${MODULE_PATH}/lua/lvm.c
    ${MODULE_PATH}/lua/lzio.c
)

# ----------------------------------------------------------------------------
# TuyaOpenClaw port glue
# ----------------------------------------------------------------------------
set(LUA_PORT_SRCS
    ${MODULE_PATH}/port/linit_sandbox.c
    ${MODULE_PATH}/port/lua_runtime.c
    ${MODULE_PATH}/port/lua_module_registry.c
    ${MODULE_PATH}/port/lua_module_os_safe.c
)

if (CONFIG_ENABLE_LUA_MODULE_GPIO STREQUAL "y")
    list(APPEND LUA_PORT_SRCS
        ${MODULE_PATH}/modules/gpio/lua_module_gpio.c
    )
endif()

if (CONFIG_ENABLE_LUA_MODULE_DELAY STREQUAL "y")
    list(APPEND LUA_PORT_SRCS
        ${MODULE_PATH}/modules/delay/lua_module_delay.c
    )
endif()

# ----------------------------------------------------------------------------
# Attach to the main app library
# ----------------------------------------------------------------------------
target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${LUA_CORE_SRCS}
        ${LUA_PORT_SRCS}
)

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${MODULE_PATH}/lua
        ${MODULE_PATH}/include
        ${MODULE_PATH}/port
        ${MODULE_PATH}/port_include
        ${MODULE_PATH}/modules/gpio
        ${MODULE_PATH}/modules/delay
)

endif()
