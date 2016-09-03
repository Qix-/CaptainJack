#!/usr/bin/env bash
cd "$(dirname "${0}")"
LC_ALL=C sed -i '' -e 's/CoreServices\/\.\.\/Frameworks\/CarbonCore\.framework\/Headers\/MacTypes\.h/MacTypes.h/' jack/**/*.c jack/**/*.h
