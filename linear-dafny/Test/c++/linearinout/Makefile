
DAFNYS=$(wildcard *.dfy)
CPPS=$(subst .dfy,.cpp,$(DAFNYS))
DOT_H_S=$(subst .dfy,.h,$(DAFNYS))
EXECS=$(subst .dfy,,$(DAFNYS))
DAFNY_DIR=../../..
DAFNY=$(DAFNY_DIR)/Binaries/dafny
DAFNY_RUNTIME_DIR=$(DAFNY_DIR)/Binaries
DAFNY_RUNTIME_CPP=$(DAFNY_RUNTIME_DIR)/DafnyRuntime.h
DAFNY_DEPS=$(DAFNY) $(DAFNY_RUNTIME_DIR)/Dafny.exe $(DAFNY_RUNTIME_DIR)/DafnyPipeline.dll $(DAFNY_RUNTIME_CPP)
TEST_LOG=tests.log

CXXFLAGS += -g -Wall -Wextra -Wpedantic -std=c++17 -I$(DAFNY_RUNTIME_DIR)

.SECONDARY: $(CPPS)

all:$(EXECS)
#all:hello ints

vars:
	echo $(DAFNYS)
	echo $(CPPS)
	echo $(EXECS)

%.cpp: %.dfy $(DAFNY) $(DAFNY_DEPS)
	$(DAFNY) /timeLimit:10 /compile:0 /spillTargetCode:3 /compileTarget:cpp $<

%.cs: %.dfy $(DAFNY) $(DAFNY_DEPS)
	$(DAFNY) /noVerify /compile:0 /spillTargetCode:3 /compileTarget:cs $<

$(EXECS): % : %.cpp $(DAFNY_RUNTIME_CPP)
	$(CXX) $(CXXFLAGS) -o $@ $<

test: $(EXECS)
	python run_cpp_tests.py $(EXECS)
#for e in $(EXECS); do echo; echo "*******************"; echo "Running $$e"; echo "*******************"; echo; ./$$e; done | tee $(TEST_LOG)
#	grep unexpected $(TEST_LOG)
#	if [ $$? -ne 0 ]; then
#	  echo "FAILED A TEST"
#	else
#		echo "SUCCESS!"
#	fi

clean:
	rm -f *.cpp $(DOT_H_S) $(EXECS) $(TEST_LOG); rm -Rf *.dSYM;
