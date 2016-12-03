#!/bin/bash

# Copyright (c) 2016      Cisco Systems, Inc.  All rights reserved.
#
# This software is available to you under a choice of one of two
# licenses.  You may choose to be licensed under the terms of the GNU
# General Public License (GPL) Version 2, available from the file
# COPYING in the main directory of this source tree, or the
# BSD license below:
#
#     Redistribution and use in source and binary forms, with or
#     without modification, are permitted provided that the following
#     conditions are met:
#
#      - Redistributions of source code must retain the above
#        copyright notice, this list of conditions and the following
#        disclaimer.
#
#      - Redistributions in binary form must reproduce the above
#        copyright notice, this list of conditions and the following
#        disclaimer in the documentation and/or other materials
#        provided with the distribution.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

####################################################################
#                                                                  #
#                  Libfabric RPM build script                      #
#                                                                  #
####################################################################

####################
# initialized values
####################
specfile="libfabric.spec"
rpmbuilddir="$PWD/rpmbuild"
prefix="/usr"
noop=""
install_in_opt=""
unpack_spec=""
verbose=""
st=""

##################################
# If you need custom configure and
# rpmbuild options, put them here
# or use "-c" and "-r" parameters
##################################
configure_options=""
rpmbuild_options=""

###########
# functions
###########
verbose()
{
  if [[ -n "$verbose" ]]; then
    echo "$0: --> $*"
  fi
}

runcmd()
{
  if [[ -z "$noop" ]]; then
    verbose "Executing: $*"
  else
    verbose "Would have executed: $*"
  fi

  if [[ -z "$noop" ]]; then
    eval "$*"
    st=$?
    if [[ "$st" != "0" ]]; then
      echo "$0: FATAL: command failed with status $st: $*" 1>&2
      exit 9
    fi
  fi
}

error()
{
  st=$1
  shift
  echo "$0: FATAL: $*" 1>&2
  if [[ -z "$noop" ]]; then
    exit $st
  fi
}

###################
# usage information
###################
usage="Usage: $0 [-i provider_name] [-e provider_name]
       [-n] [-o] [-d] [-s] [-c] [-r] [-v] [-h] tarball

 Provider options:

  -i    provider_name
             include 'provider_name' provider support to the build

  -e    provider_name
             exclude 'provider_name' provider support from the build

 General options:
  -n         no op, do nothing (useful with -v option)

  -o         install under /opt/libfabric/_VERSION_
               {default: install under /usr/ }

  -d         build with Debugging support
              {default: without debugging support}

  -s         try to unpack libfabric.spec file from tarball
               {default: look for libfabric.spec file in current directory}

  -c    parameter
             add custom configure parameter

  -r    parameter
             add custom RPM build parameter

  -v         be verbose

  -h         print this message and exit

  tarball    path to Libfabric source tarball
  "

############
# parse args
############
export arguments="$@"
while getopts noi:e:dc:r:svh flag; do
    case "$flag" in
      n) noop="true"
         ;;
      o) install_in_opt="true"
         ;;
      i) configure_options="$configure_options --enable-$OPTARG"
         ;;
      e) configure_options="$configure_options --disable-$OPTARG"
         ;;
      d) configure_options="$configure_options --enable-debug"
         ;;
      c) configure_options="$configure_options $OPTARG"
         ;;
      r) configure_options="$rpmbuild_options $OPTARG"
         ;;
      s) unpack_spec="true"
         ;;
      v) verbose="true"
         ;;
      h) echo "$usage"
         exit 0
         ;;
    esac
done
shift $(( OPTIND - 1 ));

########################################
# Check if there is at least 1 parameter
# specified (tarball is mandatory)
########################################
if [[ $# -lt 1 ]]; then
  echo "$usage" 1>&2
  exit 1
fi

#############
# Print hello
#############
if [[ -n "$verbose" ]]; then
cat <<EOF

------------------------------------------------------------------------------
====              Welcome to the Libfabric RPM build script               ====
------------------------------------------------------------------------------
You have specified these options:
 $arguments
------------------------------------------------------------------------------

EOF
  if [[ -n "$noop" ]]; then
    echo "$0: --> Option -n was specified. I won't do anything."
  fi
fi

################################
# Check if tarball was specified
################################
tarball="$1"

if [[ ! -r "$tarball" ]]; then
  error 2 "Can't find $tarball"
fi
verbose "Found tarball: $tarball"

##############################
# Get tarball name and version
##############################
tardirname=$(basename "$tarball" | awk '{print substr ($0, 0, index($0, ".tar")-1)}')
if [[ -z "$tardirname" ]]; then
  error 3 "Cannot determine name from $tarball"
fi
verbose "Name of package: $tardirname"

version=$(basename "$tardirname" | cut -d- -f2)
if [[ -z "$tardirname" ]]; then
  error 4 "Cannot determine version from $tarball"
fi
verbose "Version: $version"

if [[ -n "$install_in_opt" ]]; then
  prefix="/opt/libfabric/$version"
  rpmbuild_options="$rpmbuild_options --define '_prefix $prefix'"
  verbose "Setting RPM install path to: $prefix"
fi

######################################
# Try to unpack spec file from tarball
######################################
if [[ -f "$specfile" ]]; then
  if [[ -n "$verbose" ]]; then
    echo "$0: --> WARNING: $specfile already exists and will be overwritten" 1>&2
  fi
fi

if [[ -n "$unpack_spec" ]]; then
  if [[ -z "$verbose" ]]; then
    runcmd "tar -xf \"$tarball\""
    runcmd "cp -fp \"$tardirname/$specfile\" ."
    runcmd "rm -rf \"$tardirname\""
  else
    verbose "Extracting tarball"
    runcmd "tar -xvf \"$tarball\""
    verbose "Copying specfile"
    runcmd "cp -fpv \"$tardirname/$specfile\" ."
    verbose "Cleanup after extraction"
    runcmd "rm -rfv \"$tardirname\""
  fi
fi

##############################
# Check if specfile is present
##############################
if [[ ! -r $specfile ]]; then
  if [[ -z "$noop" ]]; then
    error 5 "Cannot find $specfile"
  else
    verbose "WARNING: Cannot find $specfile"
  fi
else
  if [[ -n "$verbose" ]]; then
    echo "$0: --> Found specfile: $specfile"
  fi
fi

#############################
# Prepare directory for build
#############################
if [[ -z "$verbose" ]]; then
  runcmd "rm -rf \"$rpmbuilddir\""
  runcmd "mkdir -p \"$rpmbuilddir/SOURCES/\""
  runcmd "cp -fp \"$tarball\" \"$rpmbuilddir/SOURCES/\""
else
  verbose "Cleanup old rpmbuild directory"
  runcmd "rm -rfv \"$rpmbuilddir\""
  verbose "Create rpmbuild SOURCES directory"
  runcmd "mkdir -pv \"$rpmbuilddir/SOURCES/\""
  verbose "Copy tarball to rpmbuild directory"
  runcmd "cp -fpv \"$tarball\" \"$rpmbuilddir/SOURCES/\""
fi

###########
# Build RPM
###########
cmd=""
build_opt=""

if [[ -z "$verbose" ]]; then
  build_opt="--quiet"
else
  build_opt="-v"
fi
cmd="rpmbuild $build_opt -bb $specfile $rpmbuild_options \
  --define '_topdir $rpmbuilddir' \
  --define '_sourcedir $rpmbuilddir/SOURCES' \
  --define '_rpmdir $rpmbuilddir/RPMS' \
  --define '_specdir $rpmbuilddir/SPECS' \
  --define '_tmppath $rpmbuilddir/tmp'"

if [[ -n "$configure_options" ]]; then
  cmd="$cmd --define 'configopts $configure_options'"
fi

verbose "Build command used:"
verbose "$cmd"
verbose "RPM build will start"

if [[ -z "$noop" ]]; then
  eval "$cmd"

  status="$?"
  if [[ "$status" != "0" ]]; then
    echo "$0: FATAL: *** FAILURE BUILDING RPM! ERROR CODE: $status" 1>&2
    error 6 "Aborting"
  fi
fi
verbose "Done building the RPM"

##############
# Print result
##############
if [[ -n "$verbose" ]]; then
  cat <<EOF

------------------------------------------------------------------------------
====                FINISHED BUILDING Libfabric RPM                       ====
------------------------------------------------------------------------------
A copy of the tarball is located in: $rpmbuilddir/SOURCES/
The completed RPMs are located in:   $rpmbuilddir/RPMS/
------------------------------------------------------------------------------

EOF
fi
