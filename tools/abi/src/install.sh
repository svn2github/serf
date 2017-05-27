#!/bin/sh
case "`basename $INSTALL_TO`" in
  0.6.1)
    patch -p0 < $(dirname "$0")/../patches/0.6.1-build-problem.patch
  ;&
  0.*|1.0*|1.1*|1.2*)
    ./configure
    make install DESTDIR=$INSTALL_TO CFLAGS="-g -Og"
  ;;
  *)
    scons install --install-sandbox=$INSTALL_TO DEBUG=yes CFLAGS="-Og"
  ;;
esac

