#!/bin/bash

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

${scripts}/mkramdisk.sh ${volume_name} ${ramconf}

# An optional parameter tells build scripts which version of APR to use
if [ ! -z "$1" ]; then
    aprdir=$(eval 'echo $SERFBB_APR_'"$1")
    apudir=$(eval 'echo $SERFBB_APU_'"$1")
    aprconfig="APR=${aprdir}"
    apuconfig="APU=${apudir}"
else
    aprconfig="APR=${SERFBB_APR_DEFAULT}"
    apuconfig="APU=${SERFBB_APU_DEFAULT}"
fi

# Another optional parameter tells build scripts to use a different OpenSSL
if [ ! -z "$2" ]; then
    opensslconfig="OPENSSL=$(eval 'echo $SERFBB_'"$2")"
else
    opensslconfig="OPENSSL=${SERFBB_OPENSSL_DEFAULT}"
fi

# Build
cd "${absbld}"
"${SERFBB_SCONS}" -Y "${abssrc}" \
                  "CC=clang" \
                  "PREFIX=${absbld}/.install-prefix" \
                  "GSSAPI=/usr" \
                  "ZLIB=/usr" \
                  "${opensslconfig}" \
                  "${aprconfig}" \
                  "${apuconfig}"
