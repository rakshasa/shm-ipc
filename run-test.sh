#!/bin/sh

multitail -s 3 -l "./test | sed -l -n 's/^PARENT://p'" -l "./test | sed -l -n 's/^CHILD://p'" -l "./test | sed -l -E '/^(PARENT:|CHILD:)/d'"
