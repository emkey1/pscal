.PHONY: all test

# pscal binary is assumed to be one directory up
PSCAL = ../build/bin/pscal

# List of test files
TESTS = BoolTest1.p BoolTest2.p BoolTest3.p BoolTest4.p math.p FileTests.p FileTests2.p TestCase.p inc.p pass-by-reference.p \
        StringTruncationTest.p LowHighCharTest.p GlobalEnumTest.p EnumComparisonTest.p EnumCaseTest.p RecordFieldIndexTest.p \
        ProgramFileList.p EofDefaultInput.p OpenArrayParam.p PrimitiveArgCall.p SwapArrayElements.p


# Tests that are expected to fail compilation
#FAIL_TESTS = ArgumentOrderMismatch.p \
#             ArgumentTypeMismatch.p \
#             ArrayArgumentMismatch.p \
#             OpenArrayBaseTypeMismatch.p

# SDL-specific tests
SDL_TESTS = SDLFeaturesTest # SDLRenderCopyTest.p

# Detect if SDL was enabled in the build (via CMakeCache)
SDL_ENABLED := $(shell grep -q '^SDL:BOOL=ON$$' ../build/CMakeCache.txt 2>/dev/null && echo 1 || echo 0)

all: test

test:
	@echo "Running tests..."
	@for t in $(TESTS); do \
	echo "----------------------------------------------------"; \
	echo "Running $$t:"; \
	$(PSCAL) $$t; \
	echo "----------------------------------------------------"; \
	echo ""; \
	done
	@echo "Running negative tests..."
	@for t in $(FAIL_TESTS); do \
	echo "----------------------------------------------------"; \
	echo "Running $$t (expected failure):"; \
	if $(PSCAL) $$t >/tmp/pscal_output 2>&1; then \
	echo "ERROR: $$t unexpectedly succeeded"; \
	cat /tmp/pscal_output; \
	exit 1; \
	else \
	cat /tmp/pscal_output; \
	if [ -f $${t%.p}.dbg ]; then \
	echo "ERROR: bytecode generated for $$t"; \
	rm -f $${t%.p}.dbg; \
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
	$(PSCAL) $$t; \
	echo "----------------------------------------------------"; \
	echo ""; \
	done; \
	else \
	echo "Skipping SDL tests (SDL option is OFF)"; \
	fi
	
