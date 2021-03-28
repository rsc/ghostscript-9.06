Ghostscript 9.06 was [released on August 8, 2012](https://code.google.com/archive/p/ghostscript/downloads?page=2). The next release, Ghostscript 9.07, changed the license from the GPL to the AGPL.

This is a copy of Ghostscript 9.06, the final GPL'ed Ghostscript.

It has been lightly edited to silence warnings from modern compilers.

To build:

```
./configure CFLAGS=-funsigned-char
make
sudo make install
```

Note the flags to set char to default to unsigned.
