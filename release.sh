#!/bin/bash
rm -rf release
mkdir -p release

cp -rf Granola *.{hpp,cpp,txt,json} LICENSE release/

mv release score-addon-granola
7z a score-addon-granola.zip score-addon-granola
