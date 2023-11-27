
How to launch meson test:

```
cd build
meson test --wrapper="gdb -x <path_to_txproto>/test/gdbinit --args" --repeat=100 -j 10 --print-errorlogs
```

<path_to_txproto> needs to be absolute
