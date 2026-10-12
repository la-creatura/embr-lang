#rm build -rd
#rm bin/linux/* -rd
#rm bin/windows/* -rd
cmake --preset linux-release
cmake --build --preset linux-release --parallel 6
cmake --preset windows-release
cmake --build --preset windows-release --parallel 6
cd ./bin/linux/
./embr
