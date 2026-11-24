CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -pthread -lncurses -lrt
SRCS_PULSE = src/main.cpp src/parser.cpp src/bounded_buffer.cpp src/worker.cpp src/ui.cpp
OBJS_PULSE = $(SRCS_PULSE:.cpp=.o)
SRC_GEN = src/loggen.cpp
TARGET_PULSE = logpulse
TARGET_GEN = loggen
all: $(TARGET_PULSE) $(TARGET_GEN)
$(TARGET_PULSE): $(OBJS_PULSE)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
$(TARGET_GEN): $(SRC_GEN)
	$(CXX) $(CXXFLAGS) -o $@ $< -pthread
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
gen: $(TARGET_GEN)
	@mkdir -p data
	./$(TARGET_GEN) -n 1000000 -o data/access.log
run: all
	./$(TARGET_PULSE) -m lote -t 4 data/access.log
clean:
	rm -f $(OBJS_PULSE) $(TARGET_PULSE) $(TARGET_GEN)
	rm -rf data/*.log data/*.log.* *.csv
.PHONY: all clean run gen
