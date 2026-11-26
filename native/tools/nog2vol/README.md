# `nog2vol` source and license

`nog2vol.py`, `formats.py`, and `assets.py` are copied from
[`trapexit/3do-bls-nogginknockers2`](https://github.com/trapexit/3do-bls-nogginknockers2),
commit `8fc7f106d5b8f272291eb949d441e858c035b360`. That project supplies
the source-derived parser for the original DOS `.VOL` formats. The copied
files are used without modification. Their license is GNU GPL version 2;
see [LICENSE](LICENSE).

The nearby `../extract_assets.py` converts its PNG output to a simple RGBA
format and maps image records to the port's compositor identifiers. It is
also distributed under GNU GPL version 2.

Original game art and audio come from the `.VOL` files supplied with this
project. The parser license does not grant rights to those assets.
