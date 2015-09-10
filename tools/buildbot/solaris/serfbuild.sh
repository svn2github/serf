#!/usr/bin/bash

#  Licensed to the Apache Software Foundation (ASF) under one
#  or more contributor license agreements.  See the NOTICE file
#  distributed with this work for additional information
#  regarding copyright ownership.  The ASF licenses this file
#  to you under the Apache License, Version 2.0 (the
#  "License"); you may not use this file except in compliance
#  with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing,
#  software distributed under the License is distributed on an
#  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
#  KIND, either express or implied.  See the License for the
#  specific language governing permissions and limitations
#  under the License.

set -e
set -x

scripts=$(cd $(dirname "$0") && pwd)

. ${scripts}/setenv.sh

# An optional parameter tells build scripts which version of APR to use
if [ ! -z "$1" ]; then
    aprconfig="APR=$(eval 'echo $SERFBB_'"$1")"
else
    aprconfig="APR=${SERFBB_APR}"
fi

# An optional parameter tells build scripts which version of APR-Util to use
if [ ! -z "$2" ]; then
    apuconfig="APU=$(eval 'echo $SERFBB_'"$2")"
else
    apuconfig="APU=${SERFBB_APU}"
fi

# Yet another optional parameter tells build scripts to use a different OpenSSL
if [ ! -z "$3" ]; then
    opensslconfig="OPENSSL=$(eval 'echo $SERFBB_'"$3")"
else
    opensslconfig="OPENSSL=${SERFBB_OPENSSL}"
fi

# Build
rm -fr "${absbld}"
mkdir "${absbld}"
cd "${absbld}"
"${SERFBB_SCONS}" -Y "${abssrc}" -j30 \
                  "CC=cc -m64" \
                  "PREFIX=${absbld}/.install-prefix" \
                  "${opensslconfig}" \
                  "${aprconfig}" \
                  "${apuconfig}"
