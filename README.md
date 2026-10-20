# tinyuwb

A DW3000 UWB driver and FiRa/CCC responder ranging engine. Portable C11.

8.5 KB flash, 0.9 KB RAM, LTO release build. The Qorvo driver and engine it
replaces take 34.3 KB flash and 4.1 KB RAM.

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
