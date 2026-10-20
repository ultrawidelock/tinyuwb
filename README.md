# tinyuwb

A DW3000 UWB driver and FiRa/CCC responder ranging engine. Portable C11.

8,680 B flash, 958 B RAM, LTO release build. The Qorvo driver and engine it
replaces take 35,094 B flash and 4,211 B RAM.

## Use

```conf
CONFIG_TINYUWB=y
```

Plus a `dw3000` devicetree node.

## Test

```sh
make test   # engine and driver, against a fake chip
make asan
make size
```

Apache-2.0
