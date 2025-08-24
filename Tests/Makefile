# SDL-specific tests
SDL_TESTS := Pascal/SDLFeaturesTest # Pascal/SDLRenderCopyTest

# pascal binary is assumed to be one directory up
PASCAL = ../build/bin/pascal

# List of test files (excluding SDL tests)
# Select files without extensions to avoid .out/.err/.in companions
TESTS := $(filter-out $(SDL_TESTS), $(filter-out $(wildcard Pascal/*.*), $(wildcard Pascal/*)))

.PHONY: all test


# Tests that are expected to fail compilation
#FAIL_TESTS = Pascal/ArgumentOrderMismatch \
#             Pascal/ArgumentTypeMismatch \
#             Pascal/ArrayArgumentMismatch \
#             Pascal/OpenArrayBaseTypeMismatch

# Detect if SDL was enabled in the build (via CMakeCache)
SDL_ENABLED := $(shell grep -q '^SDL:BOOL=ON$$' ../build/CMakeCache.txt 2>/dev/null && echo 1 || echo 0)

all: test

test:
	@echo "Running tests..."
	@for t in $(TESTS); do \
	echo "----------------------------------------------------"; \
	echo "Running $$t:"; \
        $(PASCAL) $$t; \
	echo "----------------------------------------------------"; \
	echo ""; \
	done
	@echo "Running negative tests..."
	@for t in $(FAIL_TESTS); do \
	echo "----------------------------------------------------"; \
	echo "Running $$t (expected failure):"; \
        if $(PASCAL) $$t >/tmp/pscal_output 2>&1; then \
	echo "ERROR: $$t unexpectedly succeeded"; \
	cat /tmp/pscal_output; \
	exit 1; \
	else \
        cat /tmp/pscal_output; \
        if [ -f $${t}.dbg ]; then \
        echo "ERROR: bytecode generated for $$t"; \
        rm -f $${t}.dbg; \
        exit 1; \
        fi; \
	fi; \
	echo "----------------------------------------------------"; \
	echo ""; \
	done
	@if [ $(SDL_ENABLED) -eq 1 ]; then \
	echo "Running SDL tests..."; \
	for t in $(SDL_TESTS); do \
	echo "----------------------------------------------------"; \
	echo "Running $$t:"; \
        $(PASCAL) $$t; \
	echo "----------------------------------------------------"; \
	echo ""; \
	done; \
	else \
	echo "Skipping SDL tests (SDL option is OFF)"; \
	fi
	@rm -f *.txt
	
