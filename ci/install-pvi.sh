#!/bin/bash
# Make sure we exit on any error
set -e

mkdir external
cd external
python3 -m venv pvi-venv
source pvi-venv/bin/activate

git clone https://github.com/dls-controls/pvi.git
cd pvi
git checkout makefiles
git show
pip install .
cd ..

cd ..

echo "PVI=`pwd`/external/pvi" >> configure/RELEASE.local
echo "BIN_PVI=`pwd`/external/pvi-venv/bin/pvi" >> external/pvi/configure/CONFIG_SITE
