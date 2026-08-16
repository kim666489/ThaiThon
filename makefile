args ?=

build_tmi:
	javac -d ./bin ./src/*.java

run_tmi:
	java -cp ./bin ModuleManager $(args)

build:
	g++ -o ./bin/thaithon.out -I ./src/include ./src/*.cpp -static-libgcc -static-libstdc++ -std=c++17

run:
	./bin/thaithon.out $(args)

build_win:
	g++ -o ./bin/thaithon.exe -I ./src/include ./src/*.cpp -static-libgcc -static-libstdc++ -std=c++17

run_win:
	./bin/thaithon.exe $(args)

build_all: build build_win build_tmi