# thermoprint - Thermal printer driver for Fischero D11s and Cat printers
# Requires: SimpleBLE, BlueZ (libbluetooth-dev), pthread, C++17

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
CPPFLAGS = -MMD -MP
LDLIBS   = -lsimpleble -lbluetooth -lpthread -lm -ldbus-1

TARGET = thermoprint
SRCS = ble_transport.cpp cat_printer.cpp config.cpp fischero_printer.cpp \
       image.cpp main.cpp scanner.cpp spp_transport.cpp text_render.cpp tui.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

-include $(DEPS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS)

distclean: clean
	rm -f $(TARGET)

.PHONY: all clean distclean
