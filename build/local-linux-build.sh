#!/bin/bash
# This script should be run from the root of the repo to locally update the
# linux/android golang wrappers for the libraries

docker build . -t go-libtor
docker run --rm -v "$PWD":/usr/src/go-libtor go-libtor cp -a /go/src/app/libtor/. /usr/src/go-libtor/libtor/
