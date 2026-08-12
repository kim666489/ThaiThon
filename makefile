args ?=

build:
	g++ -o ./bin/thaithon.out -I ./src/include ./src/*.cpp -static-libgcc -static-libstdc++ -std=c++17

run:
	./bin/thaithon.out $(args)