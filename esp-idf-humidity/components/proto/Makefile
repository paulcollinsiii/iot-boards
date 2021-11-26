all: c_proto python_proto

c_proto: *.proto
	@protoc --c_out=./build *.proto

python_proto: *.proto
	@protoc --python_out=../../test/utils/ *.proto
