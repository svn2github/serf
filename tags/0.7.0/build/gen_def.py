#!/usr/bin/env python
#
# gen_def.py :  Generate the .DEF file for Windows builds
#
# ====================================================================
#   Copyright 2002-2010 Justin Erenkrantz and Greg Stein
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
# ====================================================================
#
#
# Typically, this script is used like:
#
#    C:\PATH> python build/gen_def.py serf.h serf_bucket_types.h serf_bucket_util.h > serf.def
#

import re
import sys

# This regex parses function declarations that look like:
#
#    return_type serf_func1(...
#    return_type *serf_func2(...
#
# Where return_type is a combination of words and "*" each separated by a
# SINGLE space. If the function returns a pointer type (like serf_func2),
# then a space may exist between the "*" and the function name. Thus,
# a more complicated example might be:
#    const type * const * serf_func3(...
#
_funcs = re.compile(r'^(?:(?:\w+|\*) )+\*?(serf_[a-z][a-z_0-9]*)\(',
                    re.MULTILINE)

def extract_funcs(fname):
  funcs = [ ]
  for name in _funcs.findall(open(fname).read()):
    funcs.append(name)
  return funcs


if __name__ == '__main__':
  # run the extraction over each file mentioned
  import sys
  print("EXPORTS")
  for fname in sys.argv[1:]:
    for func in extract_funcs(fname):
      print(func)
