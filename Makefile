# DX7 Editor Makefile
CXX=`~/c/toad/conf/toad-config --cxx`
CXXFLAGS=`~/c/toad/conf/toad-config --cxxflags`
LDFLAGS=`~/c/toad/conf/toad-config --libs`

X=$(shell uname)
ifeq ($X, Darwin)   
CXXFLAGS+=-DDARWIN
NEXTSTEP=-framework CoreMIDI -framework CoreFoundation
endif

CXXFLAGS+=-g -Wall -D_LARGEFILE64_SOURCE -D_REENTRANT
LDFLAGS+=-lpthread

all: dx7

RESOURCE=
SRC=midi.cc thread.cc

OBJ=$(SRC:.cc=.o)

depend:
	makedepend -I. -Y $(SRC) 2> /dev/null

resource.cc: $(RESOURCE)
	$(TOADDIR)/bin/toadbin2c $(RESOURCE) > resource.cc

dx7: $(OBJ)
	$(CXX) $(NEXTSTEP) -o dx7 $(OBJ) $(LDFLAGS)

.SUFFIXES: .cc

$(OBJ): %.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f dx7 $(OBJ) core* DEADJOE
	find . -name "*~" -exec rm {} \;
# DO NOT DELETE

thread.o: thread.hh
