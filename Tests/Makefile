.PHONY: all test

# pscal binary is assumed to be one directory up
PSCAL = ../build/bin/pscal

# List of test files
TESTS = test1.p test2.p test3.p test4.p test5.p test6.p test7.p test8.p test9.p BoolTest1.p BoolTest2.p BoolTest3.p BoolTest4.p TestSuite.p math.p FileTests.p FileTests2.p TestCase.p inc.p pass-by-reference.p StringTruncationTest.p LowHighCharTest.p GlobalEnumTest.p EnumComparisonTest.p EnumCaseTest.p RecordFieldIndexTest.p CompileTimeBounds.p SuccEnumTest.p

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

