# Shell functions for mass-producing test cases.
# Variables:
#     slide: the base slide (e.g. Mirax/CMU-1.zip)
#     prefix: an optional prefix for test case names (e.g. mirax)
#     EDITOR: an optional text editor to use

function start() {
    # Argument: basename of new test case
    # Must be run from /test directory.
    if [ -n "$prefix" ] ; then
        testcase="$prefix-$1"
    else
        testcase="$1"
    fi

    ./driver create "$slide" "$testcase"
    pushd "../../test/cases/$testcase"
    [ -n "$EDITOR" ] && $EDITOR config.yaml
}

function finish() {
    if [ -n "$testcase" ] ; then
        popd
        ./driver pack $testcase
        ./driver run $testcase
    fi
    testcase=
}

function next() {
    # Argument: basename of new test case
    finish
    start "$1"
}
