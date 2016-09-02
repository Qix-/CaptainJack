#!/usr/bin/env bash
set -e

cd "$(dirname "${0}")/ext/jack"


(find . -type f -name "*.h"; find . -type f -name "*.c") | LC_ALL=C LANG=C xargs -L1 sed -i '' -e 's/CoreServices\/\.\.\/Frameworks\/CarbonCore\.framework\/Headers\/MacTypes\.h/MacTypes.h/'

./autogen.sh
./configure
