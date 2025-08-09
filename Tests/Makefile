.PHONY: all test

# pscal binary is assumed to be one directory up
PSCAL = ../pscal

# List of test files
# Use all .p files in this directory to avoid references to removed tests
TESTS := $(wildcard *.p)

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

