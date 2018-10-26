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

## This script calls a helper that provides the folloing environemnt
## variables:
##
##     PATH                     The search path
##     SERFBB_SCONS             Path to the scons binary
##     SERFBB_APR_DEFAULT       Path of default APR
##     SERFBB_APU_DEFAULT       Path of default APR-Util
##     SERFBB_APR_OPENSSL_11    Path of default APR
##     SERFBB_APU_OPENSSL_11    Path of APR-Util to use with OpenSSL 1.1
##     SERFBB_APR_20_DEV        Path of APR-2.0
##     SERFBB_APU_20_DEV        Also path of APR-2.0
##     SERFBB_OPENSSL_DEFAULT   Default OpenSSL installation prefix
##     SERFBB_OPENSSL_11        OpenSSL 1.1.x installation prefix
##
## The invoking script will set local variable named ${scripts} that
## is the absolute path the parent of this file.

# Modify this to suit your deployment
environment=$(cd "${scripts}/../.." && pwd)/environment.sh

eval $(${environment})

export PATH
export SERFBB_SCONS
export SERFBB_APR_DEFAULT
export SERFBB_APU_DEFAULT
export SERFBB_APR_OPENSSL_11
export SERFBB_APU_OPENSSL_11 
export SERFBB_APR_20_DEV
export SERFBB_APU_20_DEV
export SERFBB_OPENSSL_DEFAULT
export SERFBB_OPENSSL_11


# Set the absolute source path
abssrc=$(pwd)

# Set the path to the RAMdisk device name file
ramconf=$(dirname "${abssrc}")/ramdisk.conf

# The RAMdisk volume name is the same as the name of the builder
volume_name=$(basename $(dirname "${abssrc}"))
if [ -z "${volume_name}" ]; then
    echo "Missing config parameter: RAMdisk volume name"
    exit 1
fi

# Set the absolute build path
absbld="/Volumes/${volume_name}"
