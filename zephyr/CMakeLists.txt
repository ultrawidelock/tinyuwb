# SPDX-License-Identifier: Apache-2.0

if(CONFIG_TINYUWB)
  zephyr_include_directories(../include)
  zephyr_library()
  zephyr_library_include_directories(../src)
  zephyr_library_sources(
    ../src/kdf.c ../src/mac.c ../src/range.c ../src/round.c ../src/session.c
    ../src/setup.c ../src/sts.c ../src/twr.c ../src/dw3000.c
    ../port/zephyr/bus_zephyr.c ../port/zephyr/port_zephyr.c)
  zephyr_library_compile_definitions(
    TU_TX_POWER=${CONFIG_TINYUWB_TX_POWER}
    TU_TX_ANTD=${CONFIG_TINYUWB_TX_ANTD}
    TU_RX_ANTD=${CONFIG_TINYUWB_RX_ANTD})
  if(CONFIG_TINYUWB_RANGE_BIAS_MM)
    zephyr_library_compile_definitions(TU_RANGE_BIAS_MM=${CONFIG_TINYUWB_RANGE_BIAS_MM})
  endif()
endif()
