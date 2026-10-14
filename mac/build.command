#!/bin/sh
# Runs build.sh inside the desktop session, where the signing key in the login keychain is usable
# (over SSH, codesign fails with errSecInternalComponent): `open -a Terminal mac/build.command`,
# then wait for build.status (the exit code; output in build.log).
#
# Signs with the first "Developer ID Application" identity, or MACGLASS_SIGN_IDENTITY. A stable
# identity keeps the Screen Recording and Accessibility permissions across rebuilds.
cd "$(dirname "$0")"
rm -f build.status
identity=${MACGLASS_SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk '/Developer ID Application/ { print $2; exit }')}
./build.sh $identity > build.log 2>&1
echo $? > build.status
