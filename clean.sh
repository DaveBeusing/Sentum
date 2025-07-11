#!/bin/bash

echo "Interactive Clean-Script"

# Funktion zur Ja/Nein-Abfrage
confirm() {
	while true; do
		read -rp "$1 [y/n]: " yn
		case $yn in
			[Yy]*) return 0 ;;
			[Nn]*) return 1 ;;
			*) echo "Choose y or n" ;;
		esac
	done
}

if confirm "> delete .o-Files in ./src ?"; then
	find ./src -type f -name "*.o" -exec rm -v {} \;
else
	echo "  .o-file deletion skipped!"
fi

if [ -d "build" ]; then
	if confirm "> delete build/-Directory ?"; then
		rm -rf build/
	else
		echo "  build/-Directory deletion skipped!"
	fi
else
	echo "  no build/-Directory found."
fi

echo "Project cleanup successfull"
