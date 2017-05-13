CXX=g++

INCPATH=-I. -I.. -I../Include -IItem -INetWork -I$(BEHAVIORPATH) -I$(PROTOBUF_DIR)/include -I$(BOOST_ROOT) -I ../ThirdParty/hiredis
CXXFLAGS += $(OPT) -pipe -Wno-unused-local-typedefs -Wno-unused-but-set-variable -Wno-literal-suffix -Wall -std=c++11 -ggdb -fPIC -D_GNU_SOURCE -D__STDC_LIMIT_MACROS $(INCPATH)

LIBRARY=$(PROTOBUF_DIR)/lib/libprotobuf.a -L$(BOOST_ROOT)/stage/lib/ ../ThirdParty/hiredis/libhiredis.so
LDFLAGS = -lboost_system -lboost_thread -lboost_filesystem -lboost_date_time

PROTO_SRC=P_Asset.proto P_Protocol.proto P_Server.proto
PROTO_OBJ=$(patsubst %.proto,%.pb.o,$(PROTO_SRC))
PROTO_OPTIONS=--proto_path=. --proto_path=$(PROTOBUF_DIR)/include

BASE_OBJ=WorldSession.o Protocol.o Config.o TaskScheduler.o

BIN=GateServer

all: $(BIN)

clean:
	@rm -f $(BIN)  *.o *.pb.*

rebuild: clean all

GateServer: $(PROTO_OBJ) $(BASE_OBJ) $(SUB_OBJ) Main.o
	$(CXX) $^ -o $@ $(LIBRARY) $(LDFLAGS)

%.pb.cc: %.proto
	protoc $(PROTO_OPTIONS) --cpp_out=. $<

%.pb.o: %.pb.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
