# O-RAN-SC
#
# Copyright (C) 2019 AT&T Intellectual Property and Nokia
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# CI to verify the RMR library and build run-time and dev packages
# Inherits C toolchain from buildpack-deps:stretch
# Adds cmake ksh and alien for RMR

FROM buildpack-deps:stretch
RUN apt-get update && apt-get -q -y install cmake ksh alien
ADD . /tmp
WORKDIR /tmp

# build RMr, run unit tests, and generate packages and package lists
RUN ksh ci/ci_build.ksh

# Executing the container "as a binary" will cause the CI publish
# script to execute.  This will take the simple package list generated
# by the ci_build script and copy the list of packages to the target
# directory.  The target directory is /export by default, but can be
# overridden from the docker run command line. In either case, the 
# assumption is that the target directory is mounted as a volume.
#
ENTRYPOINT [ "ci/publish.sh" ]

