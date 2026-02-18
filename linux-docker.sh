#!/bin/bash
# Deploy this program to a Linux Docker container and run the make and
# make test commands to verify that it works.
#
# Just a make and make test:
#docker run --rm -v "$(pwd)":/src -w /src ubuntu:24.04 bash -c '
#apt-get update -qq && apt-get install -y -qq gcc make binutils >/dev/null 2>&1
#make clean && make && make test 2>&1'

docker build --platform linux/amd64 -t remapper-test . && docker run -v "$(pwd)":/src -w /src --privileged -it --rm remapper-test
