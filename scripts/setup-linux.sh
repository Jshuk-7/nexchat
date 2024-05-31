#!/bin/bash

pushd ..
vendor/bin/premake/linux/premake5 --cc=gcc --file=build.lua gmake2
popd