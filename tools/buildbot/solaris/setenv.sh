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
##     SERFBB_APR               Path to APR
##     SERFBB_APU               Path to APR-Util
##     SERFBB_OPENSSL           OpenSSL installation prefix
##
## The invoking script will set local variable named ${scripts} that
## is the absolute path the parent of this file.

# Modify this to suit your deployment
environment=$(cd "${scripts}/../.." && pwd)/environment.sh

eval $(${environment})

export PATH
export SERFBB_SCONS
export SERFBB_APR
export SERFBB_APU
export SERFBB_OPENSSL


# Set the absolute source and build paths
abssrc=$(pwd)
absbld=$(cd $(dirname ${abssrc}) && pwd)/obj

echo ${abssrc}
echo ${absbld}
