CXXFLAGS+=-g -O0 -Wall
LDFLAGS+=-lmpg123 -lao

OBJECTS=shuffleclone.o

all: shuffleclone
shuffleclone: $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) -o shuffleclone

clean:
	rm -f *~ *.o shuffleclone
