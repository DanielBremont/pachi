#!/bin/sh
#
# Create subdirectories (followup nodes) in the cwd

# Config:
# Prior value weight:
priorsims=16

# Expected env:
# $CMDDIR
# $SEQDIR

pachi="$1"

cat "$SEQDIR/a.sgf" | "$CMDDIR/../sgf2gtp.pl" -g |
	sed -e 's/genmove/uct_evaluate/' |
	$pachi |
	sed -ne '/^=/,${s/^= //;/./p}' |
	while read move val; do
		mkdir "$move" || continue
		{
		echo "$(echo "$val * $priorsims" | bc)"
		echo "$priorsims"
		} >"$move"/stats
	done
