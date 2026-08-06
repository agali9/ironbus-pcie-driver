# userspace/Makefile

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS  :=

# -------------------------------------------------------
# Shared library: libedu_device.so
# Includes C++ class + C shim so Python ctypes can load it
# -------------------------------------------------------
LIB_SRCS := edu_device.cpp edu_device_c.cpp
LIB_OBJS := $(LIB_SRCS:.cpp=.o)
LIB      := libedu_device.so

# -------------------------------------------------------
# Demo binary
# -------------------------------------------------------
DEMO_SRC := demo.cpp
DEMO     := demo

# -------------------------------------------------------
# GoogleTest binary
# -------------------------------------------------------
TEST_SRC  := tests/test_edu_device.cpp
TEST_BIN  := tests/test_edu_device
GTEST_FLAGS := -lgtest -lgtest_main -lpthread

.PHONY: all clean test pytest

all: $(LIB) $(DEMO)

# ---- Shared library ----
%.o: %.cpp edu_device.hpp
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

# ---- Demo ----
$(DEMO): $(DEMO_SRC) edu_device.hpp $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -ledu_device -Wl,-rpath,'$$ORIGIN'

# ---- GoogleTest (optional — tests/test_edu_device.cpp must be C++) ----
$(TEST_BIN): $(TEST_SRC) edu_device.hpp $(LIB)
	$(CXX) $(CXXFLAGS) -I. -o $@ $< -L. -ledu_device \
	    -Wl,-rpath,'$$ORIGIN/..' $(GTEST_FLAGS)

# ---- Run GoogleTest ----
test: $(TEST_BIN)
	sudo LD_LIBRARY_PATH=. ./$(TEST_BIN)

# ---- Run pytest ----
pytest: $(LIB)
	sudo PYTHONPATH=. python3 -m pytest python/test_edu_device.py -v

clean:
	rm -f $(LIB_OBJS) $(LIB) $(DEMO) $(TEST_BIN)
